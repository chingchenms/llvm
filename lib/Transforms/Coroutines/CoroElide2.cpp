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

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Pass.h"
#include "llvm/PassSupport.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define DEBUG_TYPE "coro-elide2"

STATISTIC(CoroHeapElide2Counter, "Number of heap elision performed");

namespace {

struct CoroHeapElide2 : ModulePass, CoroutineCommon {
  static char ID; // Pass identification, replacement for typeid

  CoroHeapElide2() : ModulePass(ID) {}


  bool runOnModule(Module& M) override {
    CoroutineCommon::PerModuleInit(M);

    bool changed = false;
    for (Function &F : M.getFunctionList()) {
      changed |= runOnFunction(F);
    }
    return changed;
  }

  bool runOnFunction(Function &F) {
    bool changed = false;

    //if (hasResumeOrDestroy(F)) {
    //  if (tryElide(F)) {
    //    ++CoroHeapElideCounter;
    //    changed = true;
    //  }
    //}
    return changed;
  }
};
}

char CoroHeapElide2::ID = 0;
INITIALIZE_PASS_BEGIN(
    CoroHeapElide2, "coro-elide2",
    "Coroutine frame allocation elision and indirect calls replacement", false,
    false)
INITIALIZE_PASS_DEPENDENCY(CoroSplit3)
INITIALIZE_PASS_END(
    CoroHeapElide2, "coro-elide2",
    "Coroutine frame allocation elision and indirect calls replacement", false,
    false)

//Pass *llvm::createCoroHeapElide2Pass() { return new CoroHeapElide2(); }