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
// TODO: pack values tightly using liveness info
//===----------------------------------------------------------------------===//

#include "CoroInternal.h"
#include "llvm/IR/IRBuilder.h"

using namespace llvm;

static BasicBlock *splitBlockIfNotFirst(Instruction *I, const Twine &Name) {
  auto BB = I->getParent();
  if (&*BB->begin() == I) {
    if (BB->getSinglePredecessor()) {
      BB->setName(Name);
      return BB;
    }
  }
  return BB->splitBasicBlock(I, Name);
}

// Split above and below a particular instruction so that it
// is all alone by itself.
static void splitAround(Instruction *I, const Twine &Name) {
  splitBlockIfNotFirst(I, Name);
  splitBlockIfNotFirst(I->getNextNode(), "After" + Name);
}

// TODO: Implement in future patches
struct SpillInfo {};

static StructType *buildFrameType(Function &F, coro::Shape &Shape,
                                  SpillInfo &Spills) {
  LLVMContext &C = F.getContext();
  SmallString<32> Name(F.getName());
  Name.append(".Frame");
  StructType *FrameTy = StructType::create(C, Name);
  auto FramePtrTy = FrameTy->getPointerTo();
  auto FnTy = FunctionType::get(Type::getVoidTy(C), FramePtrTy,
                                /*IsVarArgs=*/false);
  auto FnPtrTy = FnTy->getPointerTo();

  SmallVector<Type *, 8> Types{FnPtrTy, FnPtrTy, Type::getInt8Ty(C)};
  // TODO: Populate from Spills.
  FrameTy->setBody(Types);

  return FrameTy;
}

static Instruction *insertSpills(SpillInfo &Spills, coro::Shape &Shape) {
  auto CB = Shape.CoroBegin;
  IRBuilder<> Builder(CB->getNextNode());
  PointerType *FramePtrTy = Shape.FrameTy->getPointerTo();
  Instruction *FramePtr =
      cast<Instruction>(Builder.CreateBitCast(CB, FramePtrTy, "FramePtr"));

  auto FramePtrBB = FramePtr->getParent();
  Shape.AllocaSpillBlock =
      FramePtrBB->splitBasicBlock(FramePtr->getNextNode(), "AllocaSpillBB");
  Shape.AllocaSpillBlock->splitBasicBlock(&Shape.AllocaSpillBlock->front(),
    "PostSpill");

  return FramePtr;
}

void coro::buildCoroutineFrame(Function &F, Shape &Shape) {
  // Split all of the blocks on CoroSave.
  for (CoroSuspendInst *CSI : Shape.CoroSuspends)
    splitAround(CSI->getCoroSave(), "CoroSave");

  // Put final CoroEnd into its own block.
  splitAround(Shape.CoroEnds.front(), "CoroEnd");

  SpillInfo Spills;
  // TODO: Compute Spills (incoming in later patches)

  Shape.FrameTy = buildFrameType(F, Shape, Spills);
  Shape.FramePtr = insertSpills(Spills, Shape);
}
