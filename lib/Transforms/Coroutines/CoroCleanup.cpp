//===- CoroCleanup.cpp - Coroutine Cleanup Pass ---------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// This pass lowers all remaining coroutine intrinsics.
//===----------------------------------------------------------------------===//

#include "CoroInternal.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

#define DEBUG_TYPE "coro-cleanup"

namespace {
// Created on demand if CoroCleanup pass has work to do.
class Lowerer : coro::LowererBase {
  SmallVector<IntrinsicInst *, 4> ReplaceWithNullConstant;
  SmallVector<IntrinsicInst *, 4> ReplaceWithFirstArg;

public:
  Lowerer(Module &M) : LowererBase(M) {}
  bool lowerRemainingCoroIntrinsics(Function &F);
};
}

static void simplifyCFG(Function &F) {
  removeUnreachableBlocks(F);
  llvm::legacy::FunctionPassManager FPM(F.getParent());
  FPM.add(createCFGSimplificationPass());

  FPM.doInitialization();
  FPM.run(F);
  FPM.doFinalization();
}

bool Lowerer::lowerRemainingCoroIntrinsics(Function &F) {
  bool Changed = false;

  ReplaceWithNullConstant.clear();
  ReplaceWithFirstArg.clear();

  for (auto &I : instructions(F)) {
    if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(&I)) {
      switch (II->getIntrinsicID()) {
      default:
        continue;
      case Intrinsic::coro_begin:
      case Intrinsic::coro_free:
        ReplaceWithFirstArg.push_back(II);
        break;
      case Intrinsic::coro_alloc:
        ReplaceWithNullConstant.push_back(II);
      }
      Changed = true;
    }
  }

  if (!Changed)
    return false;

  for (auto *II : ReplaceWithFirstArg) {
    WeakVH WeakH(II);
    replaceAndRecursivelySimplify(II, II->getArgOperand(0));
    if (WeakH)
      II->eraseFromParent();
  }

  for (auto *II : ReplaceWithNullConstant) {
    WeakVH WeakH(II);
    replaceAndRecursivelySimplify(II, NullPtr);
    if (WeakH)
      II->eraseFromParent();
  }

  if (Changed)
    simplifyCFG(F);

  return true;
}

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

namespace {

struct CoroCleanup : FunctionPass {
  static char ID; // Pass identification, replacement for typeid

  CoroCleanup() : FunctionPass(ID) {}

  std::unique_ptr<Lowerer> L;

  // This pass has work to do only if we find intrinsics we are going to lower
  // in the module.
  bool doInitialization(Module &M) override {
    if (coro::declaresIntrinsics(
            M, {"llvm.coro.alloc", "llvm.coro.begin", "llvm.coro.free"}))
      L = llvm::make_unique<Lowerer>(M);
    return false;
  }

  bool runOnFunction(Function &F) override {
    if (L)
      return L->lowerRemainingCoroIntrinsics(F);
    return false;
  }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    if (!L)
      AU.setPreservesAll();
  }
};
}

char CoroCleanup::ID = 0;
INITIALIZE_PASS(CoroCleanup, "coro-cleanup",
                "Lower all coroutine related intrinsics", false, false)

Pass *llvm::createCoroCleanupPass() { return new CoroCleanup(); }
