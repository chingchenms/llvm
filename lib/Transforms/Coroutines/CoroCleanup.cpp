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

#include "CoroutineCommon.h"
#include "llvm/Transforms/Coroutines.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define DEBUG_TYPE "coro-cleanup"

STATISTIC(CoroInitCounter, "Number of @llvm.coro.init replaced");
STATISTIC(CoroResumeCounter, "Number of @llvm.coro.resume replaced");
STATISTIC(CoroDestroyCounter, "Number of @llvm.coro.destroy replaced");
STATISTIC(CoroDeleteCounter, "Number of @llvm.coro.delete replaced");

namespace {
struct CoroCleanup : FunctionPass, CoroutineCommon {
  static char ID; // Pass identification, replacement for typeid

  CoroCleanup() : FunctionPass(ID) {}

  bool doInitialization(Module &M) override {
    CoroutineCommon::PerModuleInit(M);
    return false;
  }

  // in either case, we replace the intrinsic with its first operand
  // if coroutine frame is at an offset from the beginning of the block
  // we need to add approrite adjustment, in which case, init would add an
  // offset and delete would subtract it
  void ReplaceCoroInitOrDelete(IntrinsicInst *intrin) {
    Value *operand = intrin->getArgOperand(0);
    intrin->replaceAllUsesWith(operand);
    intrin->eraseFromParent();
  }

  void ReplaceCoroElide(IntrinsicInst* intrin) {
    intrin->replaceAllUsesWith(ConstantPointerNull::get(bytePtrTy));
    intrin->eraseFromParent();
  }

  bool runOnFunction(Function &F) override {
    bool changed = false;
    for (auto it = inst_begin(F), end = inst_end(F); it != end;) {
      Instruction &I = *it++;
      if (auto intrin = dyn_cast<IntrinsicInst>(&I)) {
        switch (intrin->getIntrinsicID()) {
        default:
          continue;
        case Intrinsic::experimental_coro_delete:
          ReplaceCoroInitOrDelete(intrin);
          ++CoroDeleteCounter;
          break;
        case Intrinsic::coro_init:
          ReplaceCoroInitOrDelete(intrin);
          ++CoroInitCounter;
          break;
        case Intrinsic::experimental_coro_resume:
          ReplaceWithIndirectCall(intrin, zeroConstant);
          ++CoroResumeCounter;
          break;
        case Intrinsic::experimental_coro_destroy:
          ReplaceWithIndirectCall(intrin, oneConstant);
          ++CoroDestroyCounter;
          break;
        case Intrinsic::experimental_coro_elide:
          ReplaceCoroElide(intrin);
          break;
        }
        changed = true;
      }
    }
    return changed;
  }
};
}

char CoroCleanup::ID = 0;
INITIALIZE_PASS_BEGIN(CoroCleanup, "coro-cleanup",
                      "Remove all coroutine related intrinsics", false, false)
//INITIALIZE_PASS_DEPENDENCY(CoroSplit)
//INITIALIZE_PASS_DEPENDENCY(CoroHeapElide)
INITIALIZE_PASS_END(CoroCleanup, "coro-cleanup",
                    "Remove all coroutine related intrinsics", false, false)

// TODO: add pass dependency coro-split
Pass *llvm::createCoroCleanupPass() { return new CoroCleanup(); }
