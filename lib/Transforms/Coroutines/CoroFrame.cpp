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
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/circular_raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

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
          DEBUG(dump("S.Kills", S.Kills));
          DEBUG(dump("SavedKills", SavedKills));
        }
        if (S.Consumes != SavedConsumes) {
          DEBUG(dbgs() << "\nblock " << I << " follower " << SI << "\n");
          DEBUG(dump("S.Consume", S.Consumes));
          DEBUG(dump("SavedCons", SavedConsumes));
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

struct Spill : std::pair<Value *, Instruction *> {
  using base = std::pair<Value *, Instruction *>;

  Spill(Value *Def, User *U) : base(Def, cast<Instruction>(U)) {}

  Value *def() const { return first; }
  Instruction *user() const { return second; }
  BasicBlock *userBlock() const { return second->getParent(); }

  std::pair<Value *, BasicBlock *> getKey() const {
    return {def(), userBlock()};
  }

  bool operator<(Spill const &rhs) const { return getKey() < rhs.getKey(); }
};

using SpillInfo = SmallVector<Spill, 8>;

static void dump(StringRef Title, SpillInfo const &Spills) {
  dbgs() << "------------- " << Title << "--------------\n";
  Value *CurrentValue = nullptr;
  for (auto const &E : Spills) {
    if (CurrentValue != E.def()) {
      CurrentValue = E.def();
      CurrentValue->dump();
    }
    dbgs() << "   user: ";
    E.user()->dump();
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

static void rewritePHIs(BasicBlock &BB) {
  // For every incoming edge we will create a block holding all
  // incoming values in a single PHI nodes.
  //
  // loop:
  //    %n.val = phi i32[%n, %entry], [%inc, %loop]
  //
  // It will create:
  //
  // loop.from.entry:
  //    %n.loop.pre = phi i32 [%n, %entry]
  //    br %label loop
  // loop.from.loop:
  //    %inc.loop.pre = phi i32 [%inc, %loop]
  //    br %label loop
  //
  // After this rewrite, further analysis will ignore any phi nodes with more
  // than one incoming edge.

  SmallVector<BasicBlock *, 8> Preds(pred_begin(&BB), pred_end(&BB));
  for (BasicBlock *Pred : Preds) {
    auto IncomingBB = SplitEdge(Pred, &BB);
    IncomingBB->setName(BB.getName() + Twine(".from.") + Pred->getName());
    auto PN = cast<PHINode>(&BB.front());
    do {
      int Index = PN->getBasicBlockIndex(IncomingBB);
      Value *V = PN->getIncomingValue(Index);
      PHINode *InputV = PHINode::Create(
          V->getType(), 1, V->getName() + Twine(".") + BB.getName(),
          &IncomingBB->front());
      InputV->addIncoming(V, Pred);
      PN->setIncomingValue(Index, InputV);
      PN = dyn_cast<PHINode>(PN->getNextNode());
    } while (PN);
  }
}

static void rewritePHIs(Function &F) {
  SmallVector<BasicBlock *, 8> WorkList;

  for (BasicBlock &BB : F)
    if (auto PN = dyn_cast<PHINode>(&BB.front()))
      if (PN->getNumIncomingValues() > 1)
        WorkList.push_back(&BB);

  for (BasicBlock *BB : WorkList)
    rewritePHIs(*BB);
}

// Check for instructions that we can recreate on resume as opposed to spill
// the result into a coroutine frame.
static bool materializable(Instruction &V) {
  return isa<CastInst>(&V) || isa<GetElementPtrInst>(&V) ||
         isa<BinaryOperator>(&V) || isa<CmpInst>(&V) || isa<SelectInst>(&V);
}

// For every use of the value that is across suspend point, recreate that value
// after a suspend point.
static void rewriteMaterializableInstructions(IRBuilder<> &IRB,
                                              SpillInfo const &Spills) {
  BasicBlock *CurrentBlock = nullptr;
  Instruction *CurrentMaterialization = nullptr;
  Instruction *CurrentDef = nullptr;

  // Clone an instruction.
  auto CloneInstruction = [&](Instruction *InsertPt) {
    auto ClonedInst = cast<Instruction>(CurrentDef)->clone();
    ClonedInst->setName(CurrentDef->getName());
    ClonedInst->insertBefore(InsertPt);
    return ClonedInst;
  };

  for (auto const &E : Spills) {
    // If it is a new definition, update CurrentXXX variables.
    if (CurrentDef != E.def()) {
      CurrentDef = cast<Instruction>(E.def());
      CurrentBlock = nullptr;
      CurrentMaterialization = nullptr;
    }

    // If we have not seen this block, materialize the value.
    if (CurrentBlock != E.userBlock()) {
      CurrentBlock = E.userBlock();
      CurrentMaterialization = cast<Instruction>(CurrentDef)->clone();
      CurrentMaterialization->setName(CurrentDef->getName());
      CurrentMaterialization->insertBefore(
          &*CurrentBlock->getFirstInsertionPt());
    }

    if (auto PN = dyn_cast<PHINode>(E.user())) {
      assert(PN->getNumIncomingValues() == 1 && "unexpected number of incoming "
                                                "values in the PHINode");
      PN->replaceAllUsesWith(CurrentMaterialization);
      PN->eraseFromParent();
      continue;
    }

    // Replace all uses of CurrentDef in the current instruction with the
    // CurrentMaterialization for the block.
    for (Use &U : E.user()->operands())
      if (U.get() == CurrentDef)
        U.set(CurrentMaterialization);
  }
}

// Move early uses of spilled variable after CoroBegin.
// For example, if a parameter had address taken, we may end up with the code
// like:
//        define @f(i32 %n) {
//          %n.addr = alloca i32
//          store %n, %n.addr
//          ...
//          call @coro.begin
//    we need to move the store after coro.begin
static void fixupUses(Function &F, SpillInfo const &Spills,
  CoroBeginInst *CoroBegin) {
  DominatorTree DT(F);
  SmallVector<Instruction*, 8> NeedsMoving;

  Value* CurrentValue = nullptr;

  for (auto const &E : Spills) {
    if (CurrentValue == E.def())
      continue;

    CurrentValue = E.def();

    for (User* U : CurrentValue->users()) {
      Instruction* I = cast<Instruction>(U);
      if (DT.dominates(I, CoroBegin)) {
        DEBUG({
          for (User* UI : I->users())
          assert(DT.dominates(CoroBegin, cast<Instruction>(UI)) &&
            "cannot move instruction"
            " since users are not dominated by CoroBegin");
        dbgs() << "will move: " << *I << "\n";
        });
        NeedsMoving.push_back(I);
      }
    }
  }

  auto InsertPt = CoroBegin->getNextNode();
  for (Instruction* I : NeedsMoving)
    I->moveBefore(InsertPt);
}

void coro::buildCoroutineFrame(Function &F, Shape &Shape) {

  // Make sure that all coro.saves and the fallthrough coro.end are in their
  // own block to simplify the logic of building up SuspendCrossing data.
  for (CoroSuspendInst *CSI : Shape.CoroSuspends)
    splitAround(CSI->getCoroSave(), "CoroSave");

  // Put final CoroEnd into its own block.
  splitAround(Shape.CoroEnds.front(), "CoroEnd");

  // Transforms multi-edge PHI Nodes, so that any value feeding into a PHI will
  // never has its definition separated from the PHI by the suspend point.
  rewritePHIs(F);

  // Build suspend crossing info.
  SuspendCrossingInfo Checker(F, Shape);

  IRBuilder<> Builder(F.getContext());
  SpillInfo Spills;

  // See if there are materializable instructions across suspend points.
  for (Instruction &I : instructions(F))
    if (materializable(I))
      for (User *U : I.users())
        if (Checker.definitionAcrossSuspend(I, U))
          Spills.emplace_back(&I, U);

  // Rewrite materializable instructions to be materialized at the use point.
  std::sort(Spills.begin(), Spills.end());
  DEBUG(dump("Materializations", Spills));
  rewriteMaterializableInstructions(Builder, Spills);

  // Collect the spills for arguments and other not-materializable values.
  Spills.clear();
  for (Argument &A : F.getArgumentList())
    for (User *U : A.users())
      if (Checker.definitionAcrossSuspend(A, U))
        Spills.emplace_back(&A, U);

  for (Instruction &I : instructions(F)) {
    // token returned by CoroSave is an artifact of how we build save/suspend
    // pairs and should not be part of the Coroutine Frame
    if (isa<CoroSaveInst>(&I))
      continue;
    // CoroBeginInst returns a handle to a coroutine which is passed as a sole
    // parameter to .resume and .cleanup parts and should not go into coroutine
    // frame.
    if (isa<CoroBeginInst>(&I))
      continue;

    for (User *U : I.users())
      if (Checker.definitionAcrossSuspend(I, U)) {
        assert(!materializable(I) &&
               "rewriteMaterializable did not do its job");
        Spills.emplace_back(&I, U);
      }
  }
  std::sort(Spills.begin(), Spills.end());
  DEBUG(dump("Spills", Spills));
  fixupUses(F, Spills, Shape.CoroBegin);

  Shape.FrameTy = buildFrameType(F, Shape, Spills);
  Shape.FramePtr = insertSpills(Spills, Shape);
}
