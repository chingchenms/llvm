//===- CoroInline.cpp - Coroutine Inline Pass -----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// CoroInline - A wrapper pass around regular inliner pass
//
//===----------------------------------------------------------------------===//

#include "CoroutineCommon.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/Transforms/Coroutines.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Support/Debug.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Transforms/Scalar.h"

using namespace llvm;

#define DEBUG_TYPE "coro-inline"
namespace llvm {
  Pass *createCoroSplit3();
}

namespace {
  /// PrintCallGraphPass - Print a Module corresponding to a call graph.
  ///
  class CoroInline : public CallGraphSCCPass {
  public:
    static char ID;
    CoroInline()
      : CallGraphSCCPass(ID) {
      initializeCoroInlinePass(*PassRegistry::getPassRegistry());
    }

    struct CoroutineData {
      Type* FrameTy;
      PointerType* FramePtrTy;
      FunctionType* ResumeFnTy;
      PointerType* ResumeFnPtrTy;

      struct SubInfo {
        Function* Func;
        Value* Frame;
        Value* vFrame;

        void Init(Function& F, Twine Suffix, CoroutineData& Data) {
          Func = Function::Create(Data.ResumeFnTy, GlobalValue::InternalLinkage,
                                  F.getName() + Suffix, F.getParent());
          Func->setCallingConv(CallingConv::Fast);
          Frame = &*Func->arg_begin();
          Frame->setName("frame.ptr." + Suffix);
        }
      };

      SubInfo Ramp;
      SubInfo Resume;
      SubInfo Destroy;
      SubInfo Cleanup;

      CoroutineData(Function& F) {
        SmallString<16> smallString;
        FrameTy = StructType::create(
          F.getContext(), (F.getName() + ".frame").toStringRef(smallString));
        FramePtrTy = PointerType::get(FrameTy, 0);
        ResumeFnTy = FunctionType::get(Type::getVoidTy(F.getContext()), FramePtrTy, false);
        ResumeFnPtrTy = PointerType::get(ResumeFnTy, 0);

        Ramp.Func = &F;
        Ramp.Frame = Ramp.vFrame = nullptr;

        Resume.Init(F, ".resume", *this);
        Destroy.Init(F, ".destroy", *this);
        Cleanup.Init(F, ".cleanup", *this);
      }
    };

    SmallSetVector<Function*, 8> Coroutines;
    SmallVector<CoroutineData, 8> CoroData;

    void AddCoroutine(CallGraph& CG, Function& F) {
      if (Coroutines.insert(&F)) {
        CoroData.emplace_back(F);
        CG.getOrInsertFunction(CoroData.back().Resume.Func);
        CG.getOrInsertFunction(CoroData.back().Cleanup.Func);
        CG.getOrInsertFunction(CoroData.back().Destroy.Func);
      }
    }


    bool HasCoroInit;

    //void getAnalysisUsage(AnalysisUsage &AU) const override {
    //  Inliner->getAnalysisUsage(AU);
    //}

    bool tryCoroElide(CallGraphSCC &SCC) {
      return false;
    }

    void splitCoroutine(Function& F) {
      legacy::FunctionPassManager FPM(F.getParent());
      //FPM.add(new DominatorTreeWrapperPass());
      FPM.add(createSROAPass());
      FPM.add(createCoroSplit3());
      FPM.doInitialization();
      FPM.run(F);
      FPM.doFinalization();
    }

    bool runOnSCC(CallGraphSCC &SCC) override {
      bool changed = false;
      if (HasCoroInit)
        changed |= tryCoroElide(SCC);

      SmallPtrSet<Function*, 8> SCCFunctions;
      //DEBUG(dbgs() << "Inliner visiting SCC:");
      for (CallGraphNode *Node : SCC) {
        Function *F = Node->getFunction();
        if (F && Coroutines.count(F)) {
          splitCoroutine(*F);
          changed = true;
        }
        //DEBUG(dbgs() << " " << (F ? F->getName() : "INDIRECTNODE"));
      }

      return changed; // Inliner->runOnSCC(SCC);
    }

    bool doInitialization(CallGraph &CG) override {
      Module& M = CG.getModule();
      auto CoroSuspend = Intrinsic::getDeclaration(&M, Intrinsic::coro_suspend);
      auto CoroInit = Intrinsic::getDeclaration(&M, Intrinsic::coro_init);
      HasCoroInit = !CoroInit->user_empty();
      for (User* U : CoroSuspend->users())
        if (auto* I = dyn_cast<Instruction>(U))
          AddCoroutine(CG, *I->getParent()->getParent());
      return !Coroutines.empty();
    }

    //bool doFinalization(CallGraph &CG) override {
    //  return Inliner->doFinalization(CG);
    //}

  };

} // end anonymous namespace.

char CoroInline::ID = 0;
INITIALIZE_PASS_BEGIN(CoroInline, "coro-inline",
  "Coroutine Integration/Inlining", false, false)
  INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
  INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(InlineCostAnalysis)
  INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
  INITIALIZE_PASS_END(CoroInline, "coro-inline",
    "Coroutine Integration/Inlining", false, false)

// TODO: add pass dependency coro-split
Pass *llvm::createCoroInline() { 
  return new CoroInline(); 
}
