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
STATISTIC(CoroDoneCounter, "Number of @llvm.coro.done replaced");

namespace {
struct CoroCleanup : FunctionPass, CoroutineCommon {
  static char ID; // Pass identification, replacement for typeid

  CoroCleanup() : FunctionPass(ID) {}

  bool doInitialization(Module &M) override {
    CoroutineCommon::PerModuleInit(M);
    return false;
  }

  void ReplaceCoroInit(IntrinsicInst *intrin) {
    Value *operand = intrin->getArgOperand(0);
    intrin->replaceAllUsesWith(operand);
    intrin->eraseFromParent();
  }

  void ReplaceCoroDone(IntrinsicInst *intrin) {
    Value *rawFrame = intrin->getArgOperand(0);

    // this could be a coroutine start marker
    // it that is the case, keep it
    if (dyn_cast<ConstantPointerNull>(rawFrame))
      return;

    auto frame = new BitCastInst(rawFrame, anyFramePtrTy, "", intrin);
#if 0
    auto gepIndex = GetElementPtrInst::Create(
        anyFrameTy, frame, {zeroConstant, zeroConstant}, "", intrin);
    auto index = new LoadInst(gepIndex, "", intrin); // FIXME: alignment
    auto cmp = new ICmpInst(intrin, ICmpInst::ICMP_EQ,
      ConstantPointerNull::get(anyResumeFnPtrTy), index);
#else
    auto gepIndex = GetElementPtrInst::Create(
      anyFrameTy, frame, { zeroConstant, twoConstant }, "", intrin);
    auto index = new LoadInst(gepIndex, "", intrin); // FIXME: alignment
    auto cmp = new ICmpInst(intrin, ICmpInst::ICMP_EQ,
                            ConstantInt::get(int32Ty, 0), index);
#endif
    intrin->replaceAllUsesWith(cmp);
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
        case Intrinsic::coro_delete:
        case Intrinsic::coro_init:
          ReplaceCoroInit(intrin);
          ++CoroInitCounter;
          break;
        case Intrinsic::coro_resume:
          ReplaceWithIndirectCall(intrin, zeroConstant);
          ++CoroResumeCounter;
          break;
        case Intrinsic::coro_destroy:
          ReplaceWithIndirectCall(intrin, oneConstant);
          ++CoroDestroyCounter;
          break;
        case Intrinsic::coro_done:
          ReplaceCoroDone(intrin);
          ++CoroDoneCounter;
          break;
        case Intrinsic::coro_elide:
          ReplaceCoroElide(intrin);
          break;
        case Intrinsic::coro_suspend:
//          assert(isFakeSuspend(intrin) && "coro-split pass did not run");
          intrin->eraseFromParent();
          break;
        case Intrinsic::coro_promise:
          ReplaceCoroPromise(intrin);
          break;
        case Intrinsic::coro_from_promise:
          ReplaceCoroPromise(intrin, /*From=*/true);
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
INITIALIZE_PASS_DEPENDENCY(CoroSplit)
INITIALIZE_PASS_DEPENDENCY(CoroHeapElide)
INITIALIZE_PASS_END(CoroCleanup, "coro-cleanup",
                    "Remove all coroutine related intrinsics", false, false)

// TODO: add pass dependency coro-split
Pass *llvm::createCoroCleanupPass() { return new CoroCleanup(); }
