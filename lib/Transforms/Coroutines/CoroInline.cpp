//===- CoroInline.cpp - Inline Outlined Coroutine Parts -------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements a custom inliner that inlines that calls to coroutine
// parts that were outlined by CoroOutinePass for protection against code
// motion.
//
//===----------------------------------------------------------------------===//

#include "CoroInstr.h"
#include "CoroutineCommon.h"

#include "llvm/Transforms/IPO.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Transforms/IPO/InlinerPass.h"

using namespace llvm;

#define DEBUG_TYPE "coro-inline"

namespace {

/// \brief Inline outlined coroutine parts.
class CoroInline : public Inliner {
  std::vector<Function*> OutlinedParts;
public:
  CoroInline() : Inliner(ID, /*InsertLifetime*/ true) {
    initializeCoroInlinePass(*PassRegistry::getPassRegistry());
  }

  static char ID; // Pass identification, replacement for typeid

  InlineCost getInlineCost(CallSite CS) override;

  bool runOnSCC(CallGraphSCC &SCC) override {
    // If we did not discover any outlined parts, do not run the inliner at all.
    if (OutlinedParts.empty())
      return false;

    return Inliner::runOnSCC(SCC);
  }

  // doInitialization - find all outlined coroutine parts
  bool doInitialization(CallGraph & CG) override;

#if 0
  using llvm::Pass::doFinalization;
  bool doFinalization(CallGraph &CG) override {
    return removeDeadFunctions(CG, /*AlwaysInlineOnly=*/ true);
  }
#endif
};

}

char CoroInline::ID = 0;
INITIALIZE_PASS_BEGIN(CoroInline, "coro-inline",
                "Inline outlined coroutine parts", false, false)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
//INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(CoroInline, "coro-inline",
                "Inline outlined coroutine parts", false, false)

Pass *llvm::createCoroInlinePass() { return new CoroInline(); }

bool CoroInline::doInitialization(CallGraph & CG) {
  Inliner::doInitialization(CG);
#if 0
  Module &M = CG.getModule();
  Function *CoroInitFn = Intrinsic::getDeclaration(&M, Intrinsic::coro_init);

  // Look for any CoroInit instructions with Outlined Parts
  for (User* U : CoroInitFn->users())
    if (auto CoroInit = dyn_cast<CoroInitInst>(U))
      if (auto P = CoroInit->getParts())
        for (unsigned i = 2, n = P->getNumOperands(); i < n; ++i) {
          auto Part = cast<Function>(
              cast<ValueAsMetadata>(P->getOperand(i))->getValue());
          OutlinedParts.push_back(Part);
        }

  // sort and unique for quick lookup later
  if (!OutlinedParts.empty()) {
    std::sort(OutlinedParts.begin(), OutlinedParts.end());
    OutlinedParts.erase(std::unique(OutlinedParts.begin(),
      OutlinedParts.end()),
      OutlinedParts.end());
  }
#endif
  return false;
}

InlineCost CoroInline::getInlineCost(CallSite CS) {
  Function *Callee = CS.getCalledFunction();

  bool isOutlinedPart =
      std::binary_search(OutlinedParts.begin(), OutlinedParts.end(), Callee);

  DEBUG(isOutlinedPart ? ((dbgs() << "CoroInline: "), CS->dump()) : 0);

  return isOutlinedPart ? InlineCost::getAlways() : InlineCost::getNever();
}
