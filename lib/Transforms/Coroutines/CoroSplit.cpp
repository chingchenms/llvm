//===- CoroSplit2.cpp - Manager for Coroutine Passes -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// TODO: explaing what it is
//
//===----------------------------------------------------------------------===//

#include "CoroutineCommon.h"
#include <llvm/Transforms/Coroutines.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
using namespace llvm;

#define DEBUG_TYPE "coro-split"

namespace {
  struct CoroSplit : public CallGraphSCCPass {
    static char ID; // Pass identification, replacement for typeid
    CoroSplit() : CallGraphSCCPass(ID) {}

    bool runOnSCC(CallGraphSCC &SCC) override {
      DEBUG(dbgs() << "CoroSplit is looking at something\n");
      return false;
    }
  };
}

char CoroSplit::ID = 0;
INITIALIZE_PASS(
  CoroSplit, "coro-split",
  "Split coroutine into a set of funcitons driving its state machine", false,
  false);
Pass *llvm::createCoroSplitPass() { return new CoroSplit(); }
