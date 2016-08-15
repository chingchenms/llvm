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
    BasicBlock* BB = Mapping.indexToBlock(&BD - &Block[0]);
    return llvm::successors(BB);
  }

  BlockData &getBlockData(BasicBlock *BB) {
    return Block[Mapping.blockToIndex(BB)];
  }

  void dump();
  void dump(StringRef Label, BitVector const& BV);

  SuspendCrossingInfo(Function& F, coro::Shape& Shape);

  bool hasPathCrossingSuspendPoint(BasicBlock* DefBB, BasicBlock* UseBB) {
    size_t const DefIndex = Mapping.blockToIndex(DefBB);
    size_t const UseIndex = Mapping.blockToIndex(UseBB);

    assert(Block[UseIndex].Consumes[DefIndex] && "use must consume def");
    bool const Result = Block[UseIndex].Kills[DefIndex];
    DEBUG(dbgs() << UseBB->getName() << " => " << DefBB->getName()
      << " answer is " << Result << "\n");
    return Result;
  }

  bool definitionAcrossSuspend(BasicBlock* DefBB, User* U) {
    auto I = cast<Instruction>(U);

    // We rewritten PHINodes, so that only the ones with exactly one incoming
    // value need to be analyzed.
    if (auto PN = dyn_cast<PHINode>(I))
      if (PN->getNumIncomingValues() > 1)
        return false;

    BasicBlock* UseBB = I->getParent();
    return hasPathCrossingSuspendPoint(DefBB, UseBB);
  }

  bool definitionAcrossSuspend(Argument &A, User *U) {
    return definitionAcrossSuspend(&A.getParent()->getEntryBlock(), U);
  }

  bool definitionAcrossSuspend(Instruction &I, User *U) {
    return definitionAcrossSuspend(I.getParent(), U);
  }
};

// TODO: Implement in future patches.
struct SpillInfo {};

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
