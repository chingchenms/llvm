//===- CoroElide.cpp - Coroutine Frame Allocation Elision Pass ------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements coro-elide pass that replaces dynamic allocation 
// of coroutine frame with alloca and replaces calls to @llvm.coro.resume and
// @llvm.coro.destroy with direct calls to coroutine sub-functions
// see ./Coroutines.rst for details
//
//===----------------------------------------------------------------------===//

#include "CoroutineCommon.h"
#include "llvm/Transforms/Coroutines.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
using namespace llvm;

#define DEBUG_TYPE "coro-elide"

STATISTIC(CoroElideCounter, "Number of coroutine allocation elision performed");

namespace {

  // TODO: paste explanation
  struct CoroElide : FunctionPass {
    static char ID;
    CoroElide() : FunctionPass(ID) {}
    bool runOnFunction(Function &F) override;
  };

}

char CoroElide::ID = 0;
INITIALIZE_PASS(
  CoroElide, "coro-elide",
  "Coroutine frame allocation elision and indirect calls replacement", false,
  false);

Pass *llvm::createCoroElidePass() { return new CoroElide(); }

static void replaceWithConstant(Constant *Value,
                         SmallVectorImpl<IntrinsicInst*> &Users) {
  if (Users.empty())
    return;

  // All intrinsics in Users list should have the same type  
  auto IntrTy = Users.front()->getType();
  auto ValueTy = Value->getType();
  if (ValueTy != IntrTy) {
    assert(ValueTy->isPointerTy() && IntrTy->isPointerTy());
    Value = ConstantExpr::getBitCast(Value, IntrTy);
  }

  for (IntrinsicInst *I : Users) {
    I->replaceAllUsesWith(Value);
    I->eraseFromParent();
  }
  
  // do constant propagation
  CoroCommon::constantFoldUsers(Value);
}

static bool replaceIndirectCalls(CoroInitInst *CoroInit) {
  SmallVector<IntrinsicInst*, 8> ResumeAddr;
  SmallVector<IntrinsicInst*, 8> DestroyAddr;

  for (auto U : CoroInit->users()) {
    if (auto II = dyn_cast<IntrinsicInst>(U))
      switch (II->getIntrinsicID()) {
      default:
        continue;
      case Intrinsic::coro_resume_addr:
        ResumeAddr.push_back(II);
        break;
      case Intrinsic::coro_destroy_addr:
        DestroyAddr.push_back(II);
        break;
      }
  }

  CoroInfo Info = CoroInit->meta().getCoroInfo();

  replaceWithConstant(Info.Resume, ResumeAddr);
  replaceWithConstant(Info.Destroy, DestroyAddr);

  return !ResumeAddr.empty() || !DestroyAddr.empty();
}

bool CoroElide::runOnFunction(Function &F) {
  DEBUG(dbgs() << "CoroElide is looking at " << F.getName() << "\n");
  bool changed = false;

  // Collect all coro inits
  SmallVector<CoroInitInst*, 4> CoroInits;
  for (auto& I : instructions(F))
    if (auto CI = dyn_cast<CoroInitInst>(&I))
        CoroInits.push_back(CI);

  for (auto CI : CoroInits)
    changed |= replaceIndirectCalls(CI);

  return changed;
}
