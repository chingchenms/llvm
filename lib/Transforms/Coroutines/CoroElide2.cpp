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
#include "llvm/Transforms/IPO/InlinerPass.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/TargetLibraryInfo.h"

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

  struct CoroHeapElide2 : public Inliner, CoroutineCommon {
    InlineCostAnalysis *ICA;

  public:
    // Use extremely low threshold.
    CoroHeapElide2() : Inliner(ID, -2000000000, /*InsertLifetime*/ true),
      ICA(nullptr) {
      initializeAlwaysInlinerPass(*PassRegistry::getPassRegistry());
    }

    static char ID; // Pass identification, replacement for typeid

    InlineCost getInlineCost(CallSite CS) override;

    void getAnalysisUsage(AnalysisUsage &AU) const override;
    bool runOnSCC(CallGraphSCC &SCC) override;
  };
}

char CoroHeapElide2::ID = 0;
INITIALIZE_PASS_BEGIN(
    CoroHeapElide2, "coro-elide2",
    "Coroutine frame allocation elision and indirect calls replacement", false,
    false)
INITIALIZE_PASS_DEPENDENCY(CoroSplit3)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(InlineCostAnalysis)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(
    CoroHeapElide2, "coro-elide2",
    "Coroutine frame allocation elision and indirect calls replacement", false,
    false)

//Pass *llvm::createCoroHeapElide2Pass() { return new CoroHeapElide2(); }

InlineCost CoroHeapElide2::getInlineCost(CallSite CS) {
  Function *Callee = CS.getCalledFunction();

  // Only inline direct calls to functions that are viable for inlining.
  // FIXME: We shouldn't even get here for declarations.
  if (!Callee || Callee->isDeclaration() ||
    !ICA->isInlineViable(*Callee))
    return InlineCost::getNever();

#if 0
  if (FindIntrinsic(*Callee, Intrinsic::coro_init))
    return InlineCost::getAlways();
  if (CalledFromCoroutineInitBlock.count(Callee) != 0)
    return InlineCost::getAlways();

  if (MustInline.count(Callee) != 0)
    return InlineCost::getAlways();
#endif
  return InlineCost::getNever();
}

bool CoroHeapElide2::runOnSCC(CallGraphSCC &SCC) {
  ICA = &getAnalysis<InlineCostAnalysis>();
  return Inliner::runOnSCC(SCC);
}

void CoroHeapElide2::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<InlineCostAnalysis>();
  Inliner::getAnalysisUsage(AU);
}
