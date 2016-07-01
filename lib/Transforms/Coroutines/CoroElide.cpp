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
#include "CoroInstr.h"
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
                                SmallVectorImpl<CoroSubFnInst *> &Users) {
  if (Users.empty())
    return;

  // All intrinsics in Users list should have the same type  
  auto IntrTy = Users.front()->getType();
  auto ValueTy = Value->getType();
  if (ValueTy != IntrTy) {
    assert(ValueTy->isPointerTy() && IntrTy->isPointerTy());
    Value = ConstantExpr::getBitCast(Value, IntrTy);
  }

  for (CoroSubFnInst *I : Users) {
    I->replaceAllUsesWith(Value);
    I->eraseFromParent();
  }
  
  // do constant propagation
  CoroCommon::constantFoldUsers(Value);
}

static bool replaceIndirectCalls(CoroBeginInst *CoroBegin) {
  SmallVector<CoroSubFnInst*, 8> ResumeAddr;
  SmallVector<CoroSubFnInst*, 8> DestroyAddr;

  for (auto U : CoroBegin->users())
    if (auto II = dyn_cast<CoroSubFnInst>(U))
      if (II->getIndex() == 0)
        ResumeAddr.push_back(II);
      else
        DestroyAddr.push_back(II);

  if (ResumeAddr.empty() && DestroyAddr.empty())
    return false;

  ConstantArray* Resumers = CoroBegin->getInfo().Resumers;

  auto ResumeAddrConstant = ConstantFolder().CreateExtractValue(Resumers, 0);
  auto DestroyAddrConstant = ConstantFolder().CreateExtractValue(Resumers, 1);

  replaceWithConstant(ResumeAddrConstant, ResumeAddr);
  replaceWithConstant(DestroyAddrConstant, DestroyAddr);

  return true;
}

bool CoroElide::runOnFunction(Function &F) {
  bool changed = false;

  // Collect all coro inits
  SmallVector<CoroBeginInst*, 4> CoroBegins;
  for (auto& I : instructions(F))
    if (auto CB = dyn_cast<CoroBeginInst>(&I))
      if (CB->getInfo().postSplit())
        CoroBegins.push_back(CB);

  for (auto CB : CoroBegins)
    changed |= replaceIndirectCalls(CB);

  return changed;
}
