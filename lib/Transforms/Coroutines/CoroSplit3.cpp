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
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/Transforms/Coroutines.h"
#include "llvm/IR/LegacyPassManager.h"

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

}

namespace {
struct CoroSplit3 : public ModulePass, CoroutineCommon {
//struct CoroSplit3 : public FunctionPass, CoroutineCommon {
  static char ID; // Pass identification, replacement for typeid
  CoroSplit3() : ModulePass(ID) {}
//  CoroSplit3() : FunctionPass(ID) {}

  SmallVector<Function *, 8> Coroutines;

  struct SuspendPoint {
    IntrinsicInst* SuspendInst;
    BranchInst* SuspendBr;

    SuspendPoint(Instruction &I) : SuspendInst(dyn_cast<IntrinsicInst>(&I)) {
      if (!SuspendInst)
        return;
      if (SuspendInst->getIntrinsicID() != Intrinsic::coro_suspend) {
        SuspendInst = nullptr;
        return;
      }
      SuspendBr = dyn_cast<BranchInst>(SuspendInst->getNextNode());
      if (!SuspendBr)
        return;

      if (isFinalSuspend()) {
        assert(SuspendBr->getNumSuccessors() == 1);
      }
      else {
        assert(SuspendBr->getNumSuccessors() == 2);
        assert(SuspendBr->getOperand(0) == SuspendInst);
      }
    }

    bool isCanonical() const { return SuspendBr; }

    void remapSuspendInst(BasicBlock* BB, ConstantInt* CI) {
      ValueToValueMapTy VMap;
      VMap[SuspendInst] = CI;
      for (Instruction &I : *BB)
        RemapInstruction(&I, VMap,
          RF_NoModuleLevelChanges | RF_IgnoreMissingEntries);
    }

    static void fixupPhiNodes(BasicBlock *ResumeBB, BasicBlock *CleanupBB,
                              ValueToValueMapTy &VMap) {
      for (BasicBlock* BB : successors(CleanupBB)) {
        for (Instruction& I : *BB) {
          PHINode* PN = dyn_cast<PHINode>(&I);
          if (!PN)
            break;

          auto N = PN->getNumIncomingValues();
          SmallBitVector remapNeeded(N);
          for (unsigned i = 0; i != N; ++i)
            if (PN->getIncomingBlock(i) == ResumeBB)
              remapNeeded.set(i);

          for (int i = remapNeeded.find_first(); i != -1;
               i = remapNeeded.find_next(i)) {
            auto NewValue = VMap[PN->getIncomingValue(i)];
            PN->addIncoming(NewValue, CleanupBB);
          }
        }
      }
    }

    bool canonicalize() {
      if (isCanonical())
        return false;
      BasicBlock* BB = SuspendInst->getParent();
      Function* F = BB->getParent();
      Module* M = F->getParent();
      BasicBlock* ResumeBB =
        BB->splitBasicBlock(SuspendInst->getNextNode(), BB->getName() + ".resume");

      ValueToValueMapTy VMap;
      auto CleanupBB = CloneBasicBlock(ResumeBB, VMap, ".cleanup", F);
      CleanupBB->setName(BB->getName() + ".cleanup");
      remapSuspendInst(CleanupBB, ConstantInt::getFalse(M->getContext()));
      remapSuspendInst(ResumeBB, ConstantInt::getTrue(M->getContext()));
      
      BB->getTerminator()->eraseFromParent();
      BranchInst::Create(ResumeBB, CleanupBB, SuspendInst, BB);

      fixupPhiNodes(ResumeBB, CleanupBB, VMap);

      DEBUG(dbgs() << "Canonicalize block " << BB->getName() << ". New edges: "
        << ResumeBB->getName() << " " << CleanupBB->getName() << "\n");
      return true;
    }

    BasicBlock* getResumeBlock() const {
      return isFinalSuspend() ? nullptr : SuspendBr->getSuccessor(0);
    }
    BasicBlock* getCleanupBlock() const {
      return isFinalSuspend() ? SuspendBr->getSuccessor(0)
        : SuspendBr->getSuccessor(1);
    }

    bool isFinalSuspend() const {
      assert(SuspendInst);
      return cast<ConstantInt>(SuspendInst->getOperand(2))->isZero();
    }
    explicit operator bool() const { return SuspendInst; }
  };

  struct SuspendInfo : SmallVector<SuspendPoint, 8> {
    using Base = SmallVector<SuspendPoint, 8>;
    SmallPtrSet<BasicBlock*, 8> SuspendBlocks;

    // Canonical suspend is where
    // an @llvm.coro.suspend is followed by a
    // branch instruction 
    bool canonicalizeSuspends(Function& F) {
      bool changed = false;
      Base::clear();
      for (auto BI = F.begin(), BE = F.end(); BI != BE;) {
        auto& BB = *BI++;
        for (auto &I : BB)
          if (SuspendPoint SI{ I }) {
            changed |= SI.canonicalize();
            Base::push_back(SI);
            break;
          }
      }
      if (changed)
        CoroutineCommon::simplifyAndConstantFoldTerminators(F);

      SuspendBlocks.clear();
      for (auto SP : *this)
        SuspendBlocks.insert(SP.SuspendInst->getParent());

      return changed;
    }

    bool isSuspendBlock(BasicBlock* BB) const { return SuspendBlocks.count(BB); }
  };

  Function* ThisFunction;

  void processValue(Value *V, DominatorTree &DT, SuspendInfo const& Info) {
    Instruction* DefInst = cast<Instruction>(V);
    BasicBlock* DefBlock = DefInst->getParent();

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
        if (Info.isSuspendBlock(BB)) {
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

          // we may be able to recreate instruction
          if (auto Gep = dyn_cast<GetElementPtrInst>(V)) {
            if (isa<AllocaInst>(Gep->getPointerOperand()))
              if (Gep->hasAllConstantIndices()) {
                auto Dup = Gep->clone();
                DEBUG(dbgs() << "Cloned: " << *Dup << "\n");
                Dup->insertBefore(InsertPt);
                U.set(Dup);
                break;
              }
          }
          // otherwise, create a spill slot
          auto LoadFn = Intrinsic::getDeclaration(M, Intrinsic::coro_kill2, U->getType());
          auto Reload = CallInst::Create(LoadFn, { V }, V->getName() + ".spill", InsertPt);
          U.set(Reload);
          DEBUG(dbgs() << "Created spill: " << *Reload << "\n");
          break;
        }
        BB = DT[BB]->getIDom()->getBlock();
      }
    }
  }

  void insertSpills(Function& F, DominatorTree &DT, SuspendInfo const& Info) {

    SmallVector<Value*, 8> Values;
    ThisFunction = &F;

    for (auto &BB : F) {
      for (auto &I : BB) {
        if (I.user_empty())
          continue;
        if (isa<AllocaInst>(&I))
          continue;

        for (User* U: I.users())
          if (auto UI = dyn_cast<Instruction>(U)) {
            BasicBlock* UseBlock = UI->getParent();
            if (UseBlock == &BB)
              continue; 
            if (!DT.isReachableFromEntry(UseBlock))
              continue;
            Values.push_back(&I);
            break;
          }
      }
    }

    for (auto Value : Values) {
      processValue(Value, DT, Info);
    }
  }

  bool runOnCoroutine(Function& F) {
    DEBUG(dbgs() << "CoroSplit function: " << F.getName() << "\n");

    SuspendInfo Info;
    DominatorTreeWrapperPass& DTA = getAnalysis<DominatorTreeWrapperPass>(F);
    if (Info.canonicalizeSuspends(F)) {
      DTA.runOnFunction(F);
    }
    DominatorTree &DT = DTA.getDomTree();
    insertSpills(F, DT, Info);
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
