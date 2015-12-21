//===- CoroSplit2.cpp - Manager for Coroutine Passes -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// wait for it
//
//===----------------------------------------------------------------------===//

#include "CoroutineCommon.h"
#include "llvm/Transforms/Coroutines.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/IPO/InlinerPass.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Analysis/TargetLibraryInfo.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Type.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define DEBUG_TYPE "coro-split3"

namespace {
struct CoroSplit3 : public ModulePass, CoroutineCommon {
//struct CoroSplit3 : public FunctionPass, CoroutineCommon {
  static char ID; // Pass identification, replacement for typeid
  CoroSplit3() : ModulePass(ID) {}
//  CoroSplit3() : FunctionPass(ID) {}

  SmallVector<Function *, 8> Coroutines;

  SmallPtrSet<BasicBlock*, 8> SuspendBlocks;

  void processValue(Value *V, DominatorTree &DT) {
    Instruction* DefInst = cast<Instruction>(V);
    BasicBlock* DefBlock = DefInst->getParent();
    assert(SuspendBlocks.count(DefBlock) == 0);

    for (auto UI = V->use_begin(), UE = V->use_end(); UI != UE;) {
      Use &U = *UI++;
      Instruction* I = cast<Instruction>(U.getUser());
      auto UseBlock = I->getParent();
      if (UseBlock == DefBlock)
        continue;
      if (auto II = dyn_cast<IntrinsicInst>(I))
        if (II->getIntrinsicID() == Intrinsic::coro_kill2)
          continue;

      BasicBlock* BB = nullptr;
      PHINode* PI = dyn_cast<PHINode>(I);
      if (PI)
        BB = PI->getIncomingBlock(U);
      else {
        BB = DT[UseBlock]->getIDom()->getBlock();
      }
      while (BB != DefBlock) {
        if (SuspendBlocks.count(BB)) {
          Instruction* InsertPt = I;
          // figure out whether we need a new block
          if (PI) {
            auto IB = PI->getIncomingBlock(U);
            if (IB == BB) {
              auto ResumeBlock =
                BasicBlock::Create(M->getContext(), BB->getName() + ".resume",
                  BB->getParent(), UseBlock);
              InsertPt = BranchInst::Create(UseBlock, ResumeBlock);
              auto SuspendTerminator = cast<BranchInst>(BB->getTerminator());
              assert(SuspendTerminator->getNumSuccessors() == 2);
              if (SuspendTerminator->getSuccessor(0) == UseBlock)
                SuspendTerminator->setSuccessor(0, ResumeBlock);
              else
                SuspendTerminator->setSuccessor(1, ResumeBlock);
            }
          }
          auto LoadFn = Intrinsic::getDeclaration(M, Intrinsic::coro_kill2, U->getType());
          auto Reload = CallInst::Create(LoadFn, { V }, V->getName() + ".spill", InsertPt);
          U.set(Reload);
          break;
        }
        BB = DT[BB]->getIDom()->getBlock();
      }
    }
  }

  void insertSpills(Function& F, DominatorTree &DT) {
    SmallVector<Value*, 8> Values;

    SuspendBlocks.clear();

    for (auto &BB : F) {
      for (auto &I : BB) {
        if (auto II = dyn_cast<IntrinsicInst>(&I))
          if (II->getIntrinsicID() == Intrinsic::coro_suspend)
            SuspendBlocks.insert(II->getParent());

        if (I.user_empty())
          continue;
        if (isa<AllocaInst>(&I))
          continue;

        for (User* U: I.users())
          if (auto UI = dyn_cast<Instruction>(U))
            if (UI->getParent() != &BB) {
              Values.push_back(&I);
              break;
            }
      }
    }

    for (auto Value : Values) {
      processValue(Value, DT);
    }
  }

  bool runOnCoroutine(Function& F) {
    DominatorTree &DT = getAnalysis<DominatorTreeWrapperPass>(F).getDomTree();
    insertSpills(F, DT);
    return true;
  }

#if 1
  bool runOnModule(Module &M) override {
    CoroutineCommon::PerModuleInit(M);

    bool changed = false;
    for (Function &F : M.getFunctionList())
      if (F.hasFnAttribute(Attribute::Coroutine)) {
        changed |= runOnCoroutine(F);
      }
    return changed;
  }
#else
  bool doInitialization(Module& M) override {
    CoroutineCommon::PerModuleInit(M);
    return false;
  }

  bool runOnFunction(Function &F) override {
    bool changed = false;
    if (F.hasFnAttribute(Attribute::Coroutine)) {
      changed |= runOnCoroutine(F);
    }
    return changed;
  }

#endif
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
  }
};
}

char CoroSplit3::ID = 0;
namespace llvm {
INITIALIZE_PASS_BEGIN(
    CoroSplit3, "coro-split3",
    "Split coroutine into ramp/resume/destroy/cleanup functions v3", false,
    false)
//INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
//INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
//INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_END(
    CoroSplit3, "coro-split3",
    "Split coroutine into ramp/resume/destroy/cleanup functions v3", false,
    false)

Pass *createCoroSplit3() { return new CoroSplit3(); }
}
