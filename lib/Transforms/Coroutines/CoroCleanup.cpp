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

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Coroutines.h"

using namespace llvm;

#define DEBUG_TYPE "coro-cleanup"

STATISTIC(CoroInitCounter, "Number of @llvm.coro.init replaced");
STATISTIC(CoroResumeCounter, "Number of @llvm.coro.resume replaced");
STATISTIC(CoroDestroyCounter, "Number of @llvm.coro.destroy replaced");
STATISTIC(CoroDeleteCounter, "Number of @llvm.coro.delete replaced");

namespace {
struct CoroCleanup : FunctionPass {
  static char ID; // Pass identification, replacement for typeid

  CoroCleanup() : FunctionPass(ID) {}

  bool doInitialization(Module &M) override {
    return false;
  }

  bool runOnFunction(Function &F) override {
    DEBUG(dbgs() << "CoroCleanup is looking at " << F.getName() << "\n");
    return false;
  }
};
}

char CoroCleanup::ID = 0;
INITIALIZE_PASS(CoroCleanup, "coro-cleanup",
                "Remove all coroutine related intrinsics", false, false)

Pass *llvm::createCoroCleanupPass() { return new CoroCleanup(); }
