//===- CoroInline.cpp - Inline Outlined Coroutine Parts -------------------===//
// TODO: rename: CoroPreSplit.cpp
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
#include "llvm/IR/InstIterator.h"
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
class CoroInline : public CallGraphSCCPass {
public:
  CoroInline() : CallGraphSCCPass(ID) {}

  static char ID; // Pass identification, replacement for typeid

  bool runOnSCC(CallGraphSCC &SCC, bool& Devirt) override {
    bool changed = false;

    for (CallGraphNode *CGN : SCC)
      if (auto F = CGN->getFunction())
        if (auto CI = CoroCommon::findCoroInit(F, Phase::ReadyForSplit))
          changed |= processParts(CI->meta().getParts());

    return changed;
  }

  bool processParts(MDNode::op_range Parts) {
    bool changed = false;
    for (Metadata* MD : Parts) {
      auto F = cast<Function>(cast<ValueAsMetadata>(MD)->getValue());
      F->removeFnAttr(Attribute::NoInline);
      F->addFnAttr(Attribute::AlwaysInline);
      changed = true;
    }
    return changed;
  }
};

}

char CoroInline::ID = 0;
INITIALIZE_PASS_BEGIN(CoroInline, "coro-inline",
                "Remove noinline attr from outlined coroutine parts", false, false)
INITIALIZE_PASS_END(CoroInline, "coro-inline",
                "Remove noinline attr from outlined coroutine parts", false, false)

Pass *llvm::createCoroInlinePass() { return new CoroInline(); }
