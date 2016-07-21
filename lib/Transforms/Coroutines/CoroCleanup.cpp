//===- CoroCleanup.cpp - Coroutine Cleanup Pass ---------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// CoroCleanup - Remove remaining coroutine related intrinsics
// see ./Coroutines.rst for details
//
//===----------------------------------------------------------------------===//

#include "CoroUtils.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Pass.h"
#include <llvm/IR/InstIterator.h>
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Coroutines.h"

using namespace llvm;

#define DEBUG_TYPE "coro-cleanup"

//STATISTIC(CoroBeginCounter, "Number of @llvm.coro.begin replaced");
//STATISTIC(CoroResumeCounter, "Number of @llvm.coro.resume replaced");
//STATISTIC(CoroDestroyCounter, "Number of @llvm.coro.destroy replaced");
//STATISTIC(CoroDeleteCounter, "Number of @llvm.coro.delete replaced");
static Value* lowerSubFn(IRBuilder<>& Builder, CoroSubFnInst* SubFn) {
  Builder.SetInsertPoint(SubFn);
  Value *FrameRaw = SubFn->getFrame();
  int Index = SubFn->getIndex();

// FIXME: this should be queried from FrameBuilding layer, not here
  auto FrameTy = StructType::get(SubFn->getContext(), 
      {Builder.getInt8PtrTy(), Builder.getInt8PtrTy()});
  PointerType* FramePtrTy = FrameTy->getPointerTo();

  Builder.SetInsertPoint(SubFn);
  auto FramePtr = Builder.CreateBitCast(FrameRaw, FramePtrTy);
  auto Gep = Builder.CreateConstInBoundsGEP2_32(FrameTy, FramePtr, 0, Index);
  auto Load = Builder.CreateLoad(Gep);

  return Load;
}

static bool lowerRemainingCoroIntrinsics(Function& F) {
  bool changed = false;
  IRBuilder<> Builder(F.getContext());
  for (auto IB = inst_begin(F), IE = inst_end(F); IB != IE;) {
    IntrinsicInst* II = dyn_cast<IntrinsicInst>(&*IB++);
    if (!II)
      continue;
    Value *ReplacementValue = nullptr;
    if (auto CB = dyn_cast<CoroBeginInst>(II))
      ReplacementValue = CB->getMem();
    else if (auto CF = dyn_cast<CoroFreeInst>(II))
      ReplacementValue = CF->getArgOperand(0);
    else if (auto CA = dyn_cast<CoroAllocInst>(II))
      ReplacementValue =
          ConstantPointerNull::get(cast<PointerType>(CA->getType()));
    else if (auto FN = dyn_cast<CoroSubFnInst>(II))
      ReplacementValue = lowerSubFn(Builder, FN);
    else if (isa<CoroEndInst>(II))
      ReplacementValue = nullptr;
    else
      continue;

    // TODO: Run ConstantFolding after replacement
    if (ReplacementValue) 
      II->replaceAllUsesWith(ReplacementValue);
    II->eraseFromParent();
    changed = true;
  }
  return changed;
}

namespace {
struct CoroCleanup : FunctionPass {
  static char ID; // Pass identification, replacement for typeid

  CoroCleanup() : FunctionPass(ID) {}

  bool runOnFunction(Function &F) override {
    return lowerRemainingCoroIntrinsics(F);
  }
};
}

char CoroCleanup::ID = 0;
INITIALIZE_PASS(CoroCleanup, "coro-cleanup",
                "Remove all coroutine related intrinsics", false, false)

Pass *llvm::createCoroCleanupPass() { return new CoroCleanup(); }
