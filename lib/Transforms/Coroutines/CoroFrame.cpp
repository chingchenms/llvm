//===- CoroFrame.cpp - Builds and manipulates coroutine frame -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// This file contains classes used to discover if for a particular value
// there from sue to definition that crosses a suspend block.
//
// Using the information discovered we form a Coroutine Frame structure to
// contain those values. All uses of those values are replaced with appropriate
// GEP + load from the coroutine frame. At the point of the definition we spill
// the value into the coroutine frame.
//
// TODO: pack values tightly using liveness info.
//===----------------------------------------------------------------------===//

#include "CoroInternal.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Debug.h"
#include <llvm/Support/circular_raw_ostream.h>

using namespace llvm;

// The "coro-suspend-crossing" flag is very noisy. There is another debug type,
// "coro-frame", which has results in leaner debug spew.
#define DEBUG_TYPE "coro-suspend-crossing"

enum { SmallVectorThreshold = 32 };

// Provides two way mapping between the blocks and numbers
class BlockToIndexMapping {
  SmallVector<BasicBlock *, SmallVectorThreshold> V;

public:
  size_t size() const { return V.size(); }

  BlockToIndexMapping(Function &F) {
    for (BasicBlock &BB : F)
      V.push_back(&BB);
    std::sort(V.begin(), V.end());
  }

  size_t blockToIndex(BasicBlock *BB) const {
    auto *I = std::lower_bound(V.begin(), V.end(), BB);
    assert(I != V.end() && *I == BB && "BasicBlockNumberng: Unknown block");
    return I - V.begin();
  }

  BasicBlock *indexToBlock(unsigned Index) { return V[Index]; }
};

// The SuspendCrossingInfo maintains data that allows to answer a question
// whether given two BasicBlocks A and B there is a path from A to B that
// passes through a suspend point.
//
// For every basic block 'i' it maintains a block data that consists of:
//   Consumes:  a bit vector which contains a set of indicies of blocks that can
//              reach block 'i'
//   Kills: a bit vector which contains a set of indicies of blocks that can
//          reach block 'i', but one of the path will cross a suspend point
//   Suspend: a boolean indicating whether block 'i' contains a suspend point.
//   End: a boolean indicating whether block 'i' contains a coro.end intrinsic.
//
struct SuspendCrossingInfo {
  BlockToIndexMapping Mapping;

  struct BlockData {
    BitVector Consumes;
    BitVector Kills;
    bool Suspend = false;
    bool End = false;
  };
  SmallVector<BlockData, SmallVectorThreshold> Block;

  iterator_range<succ_iterator> successors(BlockData const &BD) {
    BasicBlock *BB = Mapping.indexToBlock(&BD - &Block[0]);
    return llvm::successors(BB);
  }

  BlockData &getBlockData(BasicBlock *BB) {
    return Block[Mapping.blockToIndex(BB)];
  }

  void dump();
  void dump(StringRef Label, BitVector const &BV);

  SuspendCrossingInfo(Function &F, coro::Shape &Shape);

  bool hasPathCrossingSuspendPoint(BasicBlock *DefBB, BasicBlock *UseBB) {
    size_t const DefIndex = Mapping.blockToIndex(DefBB);
    size_t const UseIndex = Mapping.blockToIndex(UseBB);

    assert(Block[UseIndex].Consumes[DefIndex] && "use must consume def");
    bool const Result = Block[UseIndex].Kills[DefIndex];
    DEBUG(dbgs() << UseBB->getName() << " => " << DefBB->getName()
                 << " answer is " << Result << "\n");
    return Result;
  }

  bool definitionAcrossSuspend(BasicBlock *DefBB, User *U) {
    auto I = cast<Instruction>(U);

    // We rewritten PHINodes, so that only the ones with exactly one incoming
    // value need to be analyzed.
    if (auto PN = dyn_cast<PHINode>(I))
      if (PN->getNumIncomingValues() > 1)
        return false;

    BasicBlock *UseBB = I->getParent();
    return hasPathCrossingSuspendPoint(DefBB, UseBB);
  }

  bool definitionAcrossSuspend(Argument &A, User *U) {
    return definitionAcrossSuspend(&A.getParent()->getEntryBlock(), U);
  }

  bool definitionAcrossSuspend(Instruction &I, User *U) {
    return definitionAcrossSuspend(I.getParent(), U);
  }
};

void SuspendCrossingInfo::dump(StringRef Label, BitVector const &BV) {
  dbgs() << Label << ":";
  for (size_t I = 0, N = BV.size(); I < N; ++I)
    if (BV[I])
      dbgs() << " " << Mapping.indexToBlock(I)->getName();
  dbgs() << "\n";
}

void SuspendCrossingInfo::dump() {
  for (size_t I = 0, N = Block.size(); I < N; ++I) {
    BasicBlock *const B = Mapping.indexToBlock(I);
    dbgs() << B->getName() << ":\n";
    dump("   Consumes", Block[I].Consumes);
    dump("      Kills", Block[I].Kills);
  }
  dbgs() << "\n";
}

SuspendCrossingInfo::SuspendCrossingInfo(Function &F, coro::Shape &Shape)
    : Mapping(F) {
  const size_t N = Mapping.size();
  Block.resize(N);

  // Initialize every block so that it consumes itself
  for (size_t I = 0; I < N; ++I) {
    auto &B = Block[I];
    B.Consumes.resize(N);
    B.Kills.resize(N);
    B.Consumes.set(I);
  }

  // Mark all CoroEnd Blocks
  for (auto CE : Shape.CoroEnds)
    getBlockData(CE->getParent()).End = true;

  // Mark all suspend blocks and indicate that kill everything they consume.
  // Note, that crossing coro.save is used to indicate suspend, as any code
  // between coro.save and coro.suspend may resume the coroutine and all of the
  // state needs to be saved by that time.
  for (CoroSuspendInst *CSI : Shape.CoroSuspends) {
    CoroSaveInst *const CoroSave = CSI->getCoroSave();
    BasicBlock *const CoroSaveBB = CoroSave->getParent();
    auto &B = getBlockData(CoroSaveBB);
    B.Suspend = true;
    B.Kills |= B.Consumes;
  }

  // Iterate propagating consumes and kills until they stop changing
  int Iteration = 0;

  bool Changed;
  do {
    DEBUG(dbgs() << "iteration " << ++Iteration);
    DEBUG(dbgs() << "==============\n");

    Changed = false;
    for (size_t I = 0; I < N; ++I) {
      auto &B = Block[I];
      for (BasicBlock *SI : successors(B)) {

        auto SuccNo = Mapping.blockToIndex(SI);

        // Saved Consumes and Kills bitsets so that it is easy to see
        // if anything changed after propagation.
        auto &S = Block[SuccNo];
        auto SavedConsumes = S.Consumes;
        auto SavedKills = S.Kills;

        // Propagate Kills and Consumes from block B into its successor S.
        S.Consumes |= B.Consumes;
        S.Kills |= B.Kills;

        // If block B is a suspend block, it should propagate kills into the
        // its successor for every block B consumes.
        if (B.Suspend) {
          S.Kills |= B.Consumes;
        }
        if (S.Suspend) {
          // If block S is a suspend block, it should kill all of the blocks it
          // consumes.
          S.Kills |= S.Consumes;
        } else if (S.End) {
          // If block S is an end block, it should not propagate kills as the
          // blocks following coro.end() are reached during initial invocation
          // of the coroutine while all the data are still available on the
          // stack or in the registers.
          S.Kills.reset();
        } else {
          // This is reached when S block it not Suspend nor coro.end and it
          // need to make sure that it is not in the kill set.
          S.Kills.reset(SuccNo);
        }

        // See if anything changed.
        Changed |= (S.Kills != SavedKills) || (S.Consumes != SavedConsumes);

        if (S.Kills != SavedKills) {
          DEBUG(dbgs() << "\nblock " << I << " follower " << SI->getName()
                       << "\n");
          DEBUG(dump("s.kills", S.Kills));
          DEBUG(dump("savedKills", SavedKills));
        }
        if (S.Consumes != SavedConsumes) {
          DEBUG(dbgs() << "\nblock " << I << " follower " << SI << "\n");
          DEBUG(dump("s.consume", S.Consumes));
          DEBUG(dump("savedCons", SavedConsumes));
        }
      }
    }
  } while (Changed);
  DEBUG(dump());
}

#undef DEBUG_TYPE // "coro-suspend-crossing"
#define DEBUG_TYPE "coro-frame"

// We build up the list of spills for every case where a use is separated
// from the definition by a suspend point.

struct Spill : std::pair<Value*, Instruction*> {
  using base = std::pair<Value*, Instruction*>;

  Spill(Value* Def, User* U) : base(Def, cast<Instruction>(U)) {}

  Value* def() const { return first; }
  Instruction* user() const { return second; }
  BasicBlock* userBlock() const { return second->getParent(); }

  std::pair<Value *, BasicBlock *> getKey() const {
    return{ def(), userBlock() };
  }

  bool operator<(Spill const &rhs) const { return getKey() < rhs.getKey(); }
};

using SpillInfo = SmallVector<Spill, 8>;

static void dump(StringRef Title, SpillInfo const& Spills) {
  dbgs() << "------------- " << Title << "--------------\n";
  Value* CurrentValue = nullptr;
  for (auto const &E : Spills) {
    if (CurrentValue != E.def()) {
      CurrentValue = E.def();
      CurrentValue->dump();
    }
    dbgs() << "   user: "; E.user()->dump();
  }
}

// Build a struct that will keep state for an active coroutine.
//   struct f.frame {
//     ResumeFnTy ResumeFnAddr;
//     ResumeFnTy DestroyFnAddr;
//     int ResumeIndex;
//     ... promise (if present) ...
//     ... spills ...
//   };
static StructType *buildFrameType(Function &F, coro::Shape &Shape,
                                  SpillInfo &Spills) {
  LLVMContext &C = F.getContext();
  SmallString<32> Name(F.getName());
  Name.append(".Frame");
  StructType *FrameTy = StructType::create(C, Name);
  auto *FramePtrTy = FrameTy->getPointerTo();
  auto *FnTy = FunctionType::get(Type::getVoidTy(C), FramePtrTy,
                                 /*IsVarArgs=*/false);
  auto *FnPtrTy = FnTy->getPointerTo();

  if (Shape.CoroSuspends.size() > UINT32_MAX)
    report_fatal_error("Cannot handle coroutine with this many suspend points");

  SmallVector<Type *, 8> Types{FnPtrTy, FnPtrTy, Type::getInt32Ty(C)};
  // TODO: Populate from Spills.
  FrameTy->setBody(Types);

  return FrameTy;
}

// Replace all alloca and SSA values that are accessed across suspend points
// with GetElementPointer from coroutine frame + loads and stores. Create an
// AllocaSpillBB that will become the new entry block for the resume parts of
// the coroutine:
//
//    %hdl = coro.begin(...)
//    whatever
//
// becomes:
//
//    %hdl = coro.begin(...)
//    %FramePtr = bitcast i8* hdl to %f.frame*
//    br label %AllocaSpillBB
//
//  AllocaSpillBB:
//    ; geps corresponding to allocas that were moved to coroutine frame
//    br label PostSpill
//
//  PostSpill:
//    whatever
//
//
static Instruction *insertSpills(SpillInfo &Spills, coro::Shape &Shape) {
  auto *CB = Shape.CoroBegin;
  IRBuilder<> Builder(CB->getNextNode());
  PointerType *FramePtrTy = Shape.FrameTy->getPointerTo();
  auto *FramePtr =
      cast<Instruction>(Builder.CreateBitCast(CB, FramePtrTy, "FramePtr"));

  // TODO: Insert Spills.

  auto *FramePtrBB = FramePtr->getParent();
  Shape.AllocaSpillBlock =
      FramePtrBB->splitBasicBlock(FramePtr->getNextNode(), "AllocaSpillBB");
  Shape.AllocaSpillBlock->splitBasicBlock(&Shape.AllocaSpillBlock->front(),
                                          "PostSpill");
  // TODO: Insert geps for alloca moved to coroutine frame.

  return FramePtr;
}

void coro::buildCoroutineFrame(Function &F, Shape &Shape) {
  SpillInfo Spills;
  // TODO: Compute Spills (incoming in later patches).

  Shape.FrameTy = buildFrameType(F, Shape, Spills);
  Shape.FramePtr = insertSpills(Spills, Shape);
}
