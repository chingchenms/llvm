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
#if 0
namespace {
  struct CoroEarly : public FunctionPass, CoroutineCommon {
    static char ID; // Pass identification, replacement for typeid
    CoroEarly() : FunctionPass(ID) {}

    bool doInitialization(Module& M) override {
      CoroutineCommon::PerModuleInit(M);
      return false;
    }

    struct SuspendInfo
    {
      IntrinsicInst* SuspendInst;
      BranchInst* SuspendBr;

      SuspendInfo() : SuspendInst(nullptr) {}
      SuspendInfo(Instruction &I) : SuspendInst(dyn_cast<IntrinsicInst>(&I)) {
        if (!SuspendInst)
          return;
        if (SuspendInst->getIntrinsicID() != Intrinsic::coro_suspend) {
          SuspendInst = nullptr;
          return;
        }
        SuspendBr = cast<BranchInst>(SuspendInst->getNextNode());
        if (isFinalSuspend()) {
          assert(SuspendBr->getNumSuccessors() == 1);
        }
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

    bool runOnFunction(Function &F) override {
      if (!F.hasFnAttribute(Attribute::Coroutine))
        return false;

      SmallVector<SuspendInfo, 8> Suspends;
      SmallPtrSet<BasicBlock*, 8> CleanupBlocks;
      SmallPtrSet<BasicBlock*, 8> SuspendBlocks;

      for (auto& BB: F)
        for (auto& I : BB)
        if (SuspendInfo SP{ I }) {
          Suspends.push_back(SP);
          SuspendBlocks.insert(&BB);
        }

      SmallPtrSet<BasicBlock*, 8> SharedBlocks;

      for (SuspendInfo SP : Suspends) {
        auto BB = SP.getCleanupBlock();
        for (;;) {
          if (!BB->getSinglePredecessor()) {
            SharedBlocks.insert(BB);
            break;
          }
          CleanupBlocks.insert(BB);
          if (succ_empty(BB))
            break;
          BB = BB->getSingleSuccessor();
          assert(BB && "unexpected successors on a cleanup edge");
        }
      }

      // SharedBlocks are candidates for duplication
      SmallVector<BasicBlock*, 8> Predecessors;

      while (!SharedBlocks.empty()) {
        // look at all predecessors, find all that belongs to cleanup
        // duplicate the block

        BasicBlock *B = *SharedBlocks.begin();
        SharedBlocks.erase(B);
        Predecessors.clear();
        bool notCleanup = false;
        for (auto P : predecessors(B))
          if (CleanupBlocks.count(P))
            Predecessors.push_back(P);
          else
            notCleanup = true;

        if (notCleanup) {
          ValueToValueMapTy VMap;
          auto NewBB = CloneBasicBlock(B, VMap, ".clone", &F);

          for (Instruction &I : *NewBB)
            RemapInstruction(&I, VMap,
              RF_NoModuleLevelChanges | RF_IgnoreMissingEntries);

          for (auto P : Predecessors) {
            assert(P->getSingleSuccessor() == B);
            cast<BranchInst>(P->getTerminator())->setSuccessor(0, NewBB);
          }
        }
      }


      return true;
    }

    // We don't modify the funciton much, so we preserve all analyses.
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.setPreservesAll();
    }
  };
}
char CoroEarly::ID = 0;
INITIALIZE_PASS(CoroEarly, "coro-early",
  "Pre-split coroutine transform", false, false)

#define DEBUG_TYPE "coro-early"

namespace {
  struct CoroEarly2 : public FunctionPass, CoroutineCommon {
    static char ID; // Pass identification, replacement for typeid
    CoroEarly2() : FunctionPass(ID) {}

    bool doInitialization(Module& M) override {
      CoroutineCommon::PerModuleInit(M);
      return false;
    }

    void ReplaceCoroDone(IntrinsicInst *intrin) {
      Value *rawFrame = intrin->getArgOperand(0);

      // this could be a coroutine start marker
      // it that is the case, keep it
      if (dyn_cast<ConstantPointerNull>(rawFrame))
        return;

      auto frame = new BitCastInst(rawFrame, anyFramePtrTy, "", intrin);
      auto gepIndex = GetElementPtrInst::Create(
        anyFrameTy, frame, { zeroConstant, zeroConstant }, "", intrin);
      auto index = new LoadInst(gepIndex, "", intrin); // FIXME: alignment
      auto cmp = new ICmpInst(intrin, ICmpInst::ICMP_EQ,
        ConstantPointerNull::get(anyResumeFnPtrTy), index);
      intrin->replaceAllUsesWith(cmp);
      intrin->eraseFromParent();
    }
#if 0
    void MakeOptnoneUntilCoroSplit(Function& F) {
      if (F.hasFnAttribute(Attribute::OptimizeNone)) {
        // put a marker that the function was originally no opt
        InsertFakeSuspend(ConstantPointerNull::get(bytePtrTy), &*inst_begin(F));
      }
      else {
        // we need to preserve coroutine unchanged until coro-split pass
        F.addFnAttr(Attribute::OptimizeNone);
        F.addFnAttr(Attribute::NoInline);
      }
    }
#endif

    bool runOnFunction(Function &F) override {
      bool changed = false;
      bool isCoroutine = false;

      for (auto it = inst_begin(F), end = inst_end(F); it != end;) {
        Instruction &I = *it++;
        if (auto intrin = dyn_cast<IntrinsicInst>(&I)) {
          switch (intrin->getIntrinsicID()) {
          default:
            continue;
          case Intrinsic::coro_done:
            changed = true;
            ReplaceCoroDone(intrin);
            break;
          case Intrinsic::coro_suspend:
            if (!isCoroutine) {
              changed = true;
              isCoroutine = true;
              //MakeOptnoneUntilCoroSplit(F);
            }
            break;
            // FIXME: figure out what to do with this two
          case Intrinsic::lifetime_start:
          case Intrinsic::lifetime_end:
            intrin->eraseFromParent();
            break;
          }
        }
      }
      return changed;
    }

    // We don't modify the funciton much, so we preserve all analyses.
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.setPreservesAll();
    }
  };
}
char CoroEarly2::ID = 0;
INITIALIZE_PASS(CoroEarly2, "coro-early2",
  "Pre-split coroutine transform", false, false)

  // pass incubator

namespace {
  struct CoroModuleEarly : public ModulePass {
    static char ID; // Pass identification, replacement for typeid
    StringRef name;
    CoroModuleEarly() : ModulePass(ID), name("CoroModuleEarly") {}
    Module* M;

    bool doInitialization(Module&) override {
      //errs() << "init: " << name << "\n";
      return false;
    }

    bool doFinalization(Module&) override {
      //errs() << "fini: " << name << "\n";
      return false;
    }

    bool runOnModule(Module&) override {
      return false;
    }
  };
}
char CoroModuleEarly::ID = 0;
static RegisterPass<CoroModuleEarly> Y2("CoroModuleEarly", "CoroModuleEarly Pass");

INITIALIZE_PASS_BEGIN(CoroModuleEarly, "CoroModuleEarly",
                      "CoroModuleEarly Pass", false, false)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(CoroModuleEarly, "CoroModuleEarly", "CoroModuleEarly Pass",
                    false, false)


namespace {
  struct CoroScalarLate : public FunctionPass {
    static char ID; // Pass identification, replacement for typeid
    StringRef name;
    CoroScalarLate() : FunctionPass(ID), name("CoroScalarLate") {}

    bool doInitialization(Module&) override {
      //errs() << "init: " << name << "\n";
      return false;
    }

    bool doFinalization(Module&) override {
      //errs() << "fini: " << name << "\n";
      return false;
    }

    bool runOnFunction(Function &F) override {
      return false;
    }

    // We don't modify the program, so we preserve all analyses.
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.setPreservesAll();
    }
  };
}
char CoroScalarLate::ID = 0;
static RegisterPass<CoroScalarLate> Y3("CoroScalarLate", "CoroScalarLate Pass");

namespace {
  struct CoroLast : public FunctionPass {
    static char ID; // Pass identification, replacement for typeid
    StringRef name;
    CoroLast() : FunctionPass(ID), name("CoroLast") {}

    bool doInitialization(Module&) override {
      //errs() << "init: " << name << "\n";
      return false;
    }

    bool doFinalization(Module&) override {
      //errs() << "fini: " << name << "\n";
      return false;
    }

    bool runOnFunction(Function &F) override {
      return false;
    }

    // We don't modify the program, so we preserve all analyses.
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.setPreservesAll();
    }
  };
}
char CoroLast::ID = 0;
static RegisterPass<CoroLast> Y4("CoroLast", "CoroLast Pass");

namespace {
  struct CoroOpt0 : public FunctionPass {
    static char ID; // Pass identification, replacement for typeid
    StringRef name;
    CoroOpt0() : FunctionPass(ID), name("coro-opt0") {}

    bool doInitialization(Module&) override {
      //errs() << "init: " << name << "\n";
      return false;
    }

    bool doFinalization(Module&) override {
      //errs() << "fini: " << name << "\n";
      return false;
    }

    bool runOnFunction(Function &F) override {
      return false;
    }

    // We don't modify the program, so we preserve all analyses.
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.setPreservesAll();
    }
  };
}
char CoroOpt0::ID = 0;
static RegisterPass<CoroOpt0> Y5("CoroOpt0", "CoroOpt0 Pass");

Pass *llvm::createCoroEarlyPass() {
  //return new CoroLast();
  return new CoroEarly();
} // must be a function pass

Pass *llvm::createCoroModuleEarlyPass() {
  //  return createCoroSplitPass();
  return new CoroModuleEarly();
}
Pass *llvm::createCoroScalarLatePass() {
  //  return createPrintModulePass(outs());
    return createCoroHeapElidePass(); 
  //return new CoroScalarLate();
}
Pass *llvm::createCoroLastPass() {
    return createCoroCleanupPass();
  //return new CoroLast();
}
Pass *llvm::createCoroOnOpt0() { return new CoroOpt0(); }
#endif
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
        case Intrinsic::coro_promise:
          ReplaceCoroPromise(intrin);
          break;
        case Intrinsic::coro_from_promise:
          ReplaceCoroPromise(intrin, /*From=*/true);
          break;
        case Intrinsic::coro_done:
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
