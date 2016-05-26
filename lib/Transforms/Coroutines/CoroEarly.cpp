//===- CoroEarly.cpp - Coroutine Early Function Pass ----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// CoroEarly - FunctionPass ran at extension point EP_EarlyAsPossible
// see ./Coroutines.rst for details
//
//===----------------------------------------------------------------------===//

#include "CoroutineCommon.h"
#include "llvm/Transforms/Coroutines.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"
#include "llvm/Transforms/IPO/InlinerPass.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/TargetLibraryInfo.h"

#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/CallSite.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define DEBUG_TYPE "coro-early"

namespace {
  // inline little things in a coroutine, like a void or bool
  // function with only a ret instruction returning a constant
struct CoroEarly : public FunctionPass, CoroutineCommon {
  static char ID; // Pass identification, replacement for typeid
  CoroEarly() : FunctionPass(ID) {}

  bool doInitialization(Module& M) override {
    CoroutineCommon::PerModuleInit(M);
    return false;
  }

  // replaces coro_(from_)promise and coro_done intrinsics
  bool runOnFunction(Function &F) override {
    bool changed = false;

    for (auto it = inst_begin(F), end = inst_end(F); it != end;) {
      Instruction &I = *it++;
      if (auto intrin = dyn_cast<IntrinsicInst>(&I)) {
        switch (intrin->getIntrinsicID()) {
        default:
          continue;
        case Intrinsic::experimental_coro_promise:
          ReplaceCoroPromise(intrin);
          break;
        case Intrinsic::experimental_coro_from_promise:
          ReplaceCoroPromise(intrin, /*From=*/true);
          break;
        case Intrinsic::experimental_coro_done:
          ReplaceCoroDone(intrin);
          break;
        }
        changed = true;
      }
    }
    return changed;
  }
};
}
char CoroEarly::ID = 0;
static RegisterPass<CoroEarly> Y("CoroEarly", "replace early coro intrinsics");
namespace llvm {
  Pass *createCoroEarlyPass() { return new CoroEarly(); }
}
