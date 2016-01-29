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
#if 0
  struct CoroModuleEarly : public CallGraphSCCPass {
    static char ID; // Pass identification, replacement for typeid
    StringRef name;
    CoroModuleEarly() : CallGraphSCCPass(ID), name("CoroModuleEarly") {}
    Module* M;

    bool doInitialization(Module&) override {
      //errs() << "init: " << name << "\n";
      return false;
    }

    bool doFinalization(Module&) override {
      //errs() << "fini: " << name << "\n";
      return false;
    }

    void processCoroutine(Function& F, CallGraph& CG) {
      // walk CG up until we hit
    }

    bool doInitialization(CallGraph &CG) override {
      CallGraphSCCPass::doInitialization(CG);
      M = &CG.getModule();

      for (auto& F : *M)
        if (F.hasFnAttribute(Attribute::Coroutine))
          processCoroutine(F, CG);

      return false;
    }

    bool runOnSCC(CallGraphSCC &SCC) override {
      return false;
    }

    // We don't modify the program, so we preserve all analyses.
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.setPreservesAll();
    }
  };
#else
  class CoroModuleEarly : public Inliner, CoroutineCommon {
    InlineCostAnalysis *ICA;

  public:
    // Use extremely low threshold.
    CoroModuleEarly() : Inliner(ID, -2000000000, /*InsertLifetime*/ true),
      ICA(nullptr) {
      initializeAlwaysInlinerPass(*PassRegistry::getPassRegistry());
    }

    static char ID; // Pass identification, replacement for typeid

    InlineCost getInlineCost(CallSite CS) override;

    void getAnalysisUsage(AnalysisUsage &AU) const override;
    bool runOnSCC(CallGraphSCC &SCC) override;

    SmallVector<Function*, 4> Coroutines;
    SmallPtrSet<Function*, 16> CalledFromCoroutineInitBlock;
    SmallPtrSet<Function*, 8> MustInline;

    using llvm::Pass::doFinalization;
    bool doInitialization(CallGraph &CG) override {
      CalledFromCoroutineInitBlock.clear();
      Coroutines.clear();
      MustInline.clear();
      for (auto& F : CG.getModule()) {
        if (F.hasFnAttribute(Attribute::Coroutine)) {
          Coroutines.push_back(&F);
          Instruction* II = FindIntrinsic(F, Intrinsic::coro_init);
          assert(II->getParent() == &F.getEntryBlock() && "@llvm.coro.init is not in the entry block");
          for (auto it = ++BasicBlock::iterator(II),
            end = F.getEntryBlock().end();
            it != end; ++it) {
            CallSite CS(&*it);
            if (!CS.isCall())
              continue;

            Function *Callee = CS.getCalledFunction();

            if (!Callee || Callee->isDeclaration())
              continue;

            CalledFromCoroutineInitBlock.insert(Callee);
          }
        }
        else if (F.isIntrinsic()) {
          auto ID = F.getIntrinsicID();
          if (ID == Intrinsic::coro_resume || ID == Intrinsic::coro_destroy) {
            if (F.use_empty())
              continue;

            for (User *U : F.users())
              if (auto I = dyn_cast<Instruction>(U))
                MustInline.insert(I->getParent()->getParent());
          }
        }
      }

      SmallVector<Function *, 8> Worklist(CalledFromCoroutineInitBlock.begin(),
                                          CalledFromCoroutineInitBlock.end());
      while (!Worklist.empty()) {
        auto F = Worklist.pop_back_val();
        for (auto CR : *CG[F]) {
          auto Callee = CR.second->getFunction();
          if (!Callee || Callee->isDeclaration() ||
            CalledFromCoroutineInitBlock.count(Callee) != 0)
            continue;

          CalledFromCoroutineInitBlock.insert(Callee);
          Worklist.push_back(Callee);
        }
      }

      Worklist.clear();
      Worklist.append(MustInline.begin(), MustInline.end());
      while (!Worklist.empty()) {
        auto F = Worklist.pop_back_val();

        for (User *U : F->users())
          if (auto I = dyn_cast<Instruction>(U))
            MustInline.insert(I->getParent()->getParent());
      }

      return false;
    }
    //bool doFinalization(CallGraph &CG) override {
    //  return removeDeadFunctions(CG, /*AlwaysInlineOnly=*/ true);
    //}
  };
#endif
}
char CoroModuleEarly::ID = 0;
static RegisterPass<CoroModuleEarly> Y2("CoroModuleEarly", "CoroModuleEarly Pass");

INITIALIZE_PASS_BEGIN(CoroModuleEarly, "CoroModuleEarly",
                      "CoroModuleEarly Pass", false, false)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(InlineCostAnalysis)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(CoroModuleEarly, "CoroModuleEarly", "CoroModuleEarly Pass",
                    false, false)


// from_promise -- inline into coroutine
// resume / destroy-- inline into functions that call coroutines
// if function has from_promise in it inline it into a parent


InlineCost CoroModuleEarly::getInlineCost(CallSite CS) {
  Function *Callee = CS.getCalledFunction();

  // Only inline direct calls to functions that are viable for inlining.
  // FIXME: We shouldn't even get here for declarations.
  if (!Callee || Callee->isDeclaration() || 
    !ICA->isInlineViable(*Callee) ||
    CS.hasFnAttr(Attribute::Coroutine))
    return InlineCost::getNever();

  if (CalledFromCoroutineInitBlock.count(Callee) != 0)
    return InlineCost::getAlways();

  if (MustInline.count(Callee) != 0)
    return InlineCost::getAlways();

  return InlineCost::getNever();
}

bool CoroModuleEarly::runOnSCC(CallGraphSCC &SCC) {
  ICA = &getAnalysis<InlineCostAnalysis>();
  return Inliner::runOnSCC(SCC);
}

void CoroModuleEarly::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<InlineCostAnalysis>();
  Inliner::getAnalysisUsage(AU);
}

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
    return createCoroSplitPass();
  //return new CoroModuleEarly();
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

namespace {
  // inline little things in a coroutine, like a void or bool
  // function with only a ret instruction returning a constant
#if 1
  struct CoroPreSplit : public ModulePass, CoroutineCommon {
    static char ID; // Pass identification, replacement for typeid
    CoroPreSplit() : ModulePass(ID) {}

    SmallPtrSet<Function*, 8> RampFunctions;

    void ReplaceIfEmpty(Instruction& I, const Function & F) {
      if (F.size() == 1)
        if (F.getEntryBlock().size() == 1)
          if (isa<ReturnInst>(&*inst_begin(F)))
            I.eraseFromParent();
    }

    void ReplaceIfConstant(Instruction& I, const Function& F) {
      if (F.size() == 1)
        if (F.getEntryBlock().size() == 1)
          if (auto RetInst = dyn_cast<ReturnInst>(&*inst_begin(F)))
            if (auto ConstVal = dyn_cast<ConstantInt>(RetInst->getReturnValue())) {
              I.replaceAllUsesWith(ConstVal);
              I.eraseFromParent();
            }
    }

    void ReplaceSuspendIfEmpty(IntrinsicInst& I) {
      Value *op = I.getArgOperand(1);
      while (const ConstantExpr *CE = dyn_cast<ConstantExpr>(op)) {
        if (!CE->isCast())
          break;
        // Look through the bitcast
        op = cast<ConstantExpr>(op)->getOperand(0);
      }
      Function* fn = cast<Function>(op);
      RampFunctions.insert(fn);
      if (fn->size() != 1)
        return;

      BasicBlock& B = fn->back();
      if (B.size() < 2)
        return;

      auto LastInstr = B.getTerminator()->getPrevNode();
      CallInst* Call = cast<CallInst>(LastInstr);
      Function* awaitSuspendFn = Call->getCalledFunction();
      if (!awaitSuspendFn)
        return;
      if (awaitSuspendFn->isDeclaration())
        return;

      if (awaitSuspendFn->size() == 1)
        if (awaitSuspendFn->getEntryBlock().size() == 1)
          if (isa<ReturnInst>(&*inst_begin(*awaitSuspendFn))) 
            I.setOperand(0, ConstantPointerNull::get(bytePtrTy));
    }

    void RemoveLifetimeIntrinsics(Function& F) {
      bool changed = false;
      for (auto it = inst_begin(F), end = inst_end(F); it != end;) {
        Instruction &I = *it++;
        if (auto intrin = dyn_cast<IntrinsicInst>(&I)) {
          switch (intrin->getIntrinsicID()) {
          default:
            continue;
          case Intrinsic::lifetime_start:
          case Intrinsic::lifetime_end:
            intrin->eraseFromParent();
            changed = true;
            break;
          }
        }
      }
      if (changed)
        simplifyAndConstantFoldTerminators(F);
    }

    bool runOnCoroutine(Function& F) {
      // TODO: try alias analysis
      //RemoveNoOptAttribute(F);
      RemoveLifetimeIntrinsics(F);
#if 0
      Function* coroKill = 
        Intrinsic::getDeclaration(M, Intrinsic::coro_kill2, { int32Ty });
      coroKill->dump();

      Function* coroSave =
        Intrinsic::getDeclaration(M, Intrinsic::coro_save, { int32Ty });
      coroSave->dump();

      Function* coroLoad =
        Intrinsic::getDeclaration(M, Intrinsic::coro_load, { int32Ty });
      coroLoad->dump();

      Function* coroFromPromise = M->getFunction("llvm.coro.from.promise.p0struct.minig::promise_type");
        //Intrinsic::getDeclaration(M, Intrinsic::coro_load, { int32Ty });

#endif
      for (auto it = inst_begin(F), end = inst_end(F); it != end;) {
        Instruction& I = *it++;

        CallSite CS(cast<Value>(&I));
        if (!CS)
          continue;
        const Function *Callee = CS.getCalledFunction();
        if (!Callee)
          continue; // indirect call, nothing to do
        if (Callee->isIntrinsic()) {
          if (Callee->getIntrinsicID() == Intrinsic::coro_suspend)
            ReplaceSuspendIfEmpty(*cast<IntrinsicInst>(&I));
          continue;
        }

        if (Callee->isDeclaration())
          continue;

        // only inline void and bool returning functions
        const Type *RetType = Callee->getReturnType();
        if (RetType == voidTy) ReplaceIfEmpty(I, *Callee);
        else if (RetType == boolTy) ReplaceIfConstant(I, *Callee);
      }
      return true;
    }

    void handleRampFunction(Function& F) {
      SmallVector<CallSite, 8> CallSites;
      for (Instruction& I: instructions(F))
        if (auto CS = CallSite(&I))
          CallSites.push_back(CS);

      for (auto CS : CallSites) {
        InlineFunctionInfo IFI;
        InlineFunction(CS, IFI);
      }
    }

    // inline small functions into its parent until we hit a coroutine
    void handleFromPromise(Function& F) {
      for (auto& U : F.uses()) {
        User *UR = U.getUser();

        if (!isa<CallInst>(UR) && !isa<InvokeInst>(UR))
          continue;

        CallSite CS(cast<Instruction>(UR));
        if (!CS.isCallee(&U))
          continue;

        InlineFunctionInfo IFI;
        InlineFunction(CS, IFI);
      }
    }

    bool replaceCoroPromise(Function& F) {
      bool changed = false;
      for (auto it = inst_begin(F), end = inst_end(F); it != end;) {
        Instruction &I = *it++;
        if (auto intrin = dyn_cast<IntrinsicInst>(&I)) {
          switch (intrin->getIntrinsicID()) {
          default:
            continue;
          case Intrinsic::coro_promise:
            ReplaceCoroPromise(intrin);
            changed = true;
            break;
          case Intrinsic::coro_from_promise:
            ReplaceCoroPromise(intrin, /*From=*/true);
            changed = true;
            break;
          }
        }
      }
      return changed;
    }

    bool runOnModule(Module &M) override {
      CoroutineCommon::PerModuleInit(M);
      RampFunctions.clear();

      bool changed = false;
      for (Function &F : M.getFunctionList()) {
        if (isCoroutine(F))
          changed |= runOnCoroutine(F);
        else switch (F.getIntrinsicID()) {
        default:
          changed |= replaceCoroPromise(F);
          break;
        case Intrinsic::coro_from_promise:
          handleFromPromise(F);
          break;
          // why these are here?
        case Intrinsic::coro_destroy:
        case Intrinsic::coro_resume:
          break;
        }
      }

      for (Function* F : RampFunctions) {
        handleRampFunction(*F);
        changed = true;
      }
      return changed;
    }
  };
#else
struct CoroPreSplit : public FunctionPass, CoroutineCommon {
  static char ID; // Pass identification, replacement for typeid
  CoroPreSplit() : FunctionPass(ID) {}

  SmallPtrSet<Function*, 8> RampFunctions;

  void ReplaceIfEmpty(Instruction& I, const Function & F) {
    if (F.size() == 1)
      if (F.getEntryBlock().size() == 1)
        if (isa<ReturnInst>(&*inst_begin(F)))
          I.eraseFromParent();
  }

  void ReplaceIfConstant(Instruction& I, const Function& F) {
    if (F.size() == 1)
      if (F.getEntryBlock().size() == 1)
        if (auto RetInst = dyn_cast<ReturnInst>(&*inst_begin(F)))
          if (auto ConstVal = dyn_cast<ConstantInt>(RetInst->getReturnValue())) {
            I.replaceAllUsesWith(ConstVal);
            I.eraseFromParent();
          }
  }

  void ReplaceSuspendIfEmpty(IntrinsicInst& I) {
    Value *op = I.getArgOperand(1);
    while (const ConstantExpr *CE = dyn_cast<ConstantExpr>(op)) {
      if (!CE->isCast())
        break;
      // Look through the bitcast
      op = cast<ConstantExpr>(op)->getOperand(0);
    }
    Function* fn = cast<Function>(op);
    RampFunctions.insert(fn);
    if (fn->size() != 1)
      return;

    BasicBlock& B = fn->back();
    if (B.size() < 2)
      return;

    auto LastInstr = B.getTerminator()->getPrevNode();
    CallInst* Call = cast<CallInst>(LastInstr);
    Function* awaitSuspendFn = Call->getCalledFunction();
    if (!awaitSuspendFn)
      return;
    if (awaitSuspendFn->isDeclaration())
      return;

    if (awaitSuspendFn->size() == 1)
      if (awaitSuspendFn->getEntryBlock().size() == 1)
        if (isa<ReturnInst>(&*inst_begin(*awaitSuspendFn)))
          I.setOperand(0, ConstantPointerNull::get(bytePtrTy));
  }

  void RemoveLifetimeIntrinsics(Function& F) {
    bool changed = false;
    for (auto it = inst_begin(F), end = inst_end(F); it != end;) {
      Instruction &I = *it++;
      if (auto intrin = dyn_cast<IntrinsicInst>(&I)) {
        switch (intrin->getIntrinsicID()) {
        default:
          continue;
        case Intrinsic::lifetime_start:
        case Intrinsic::lifetime_end:
          intrin->eraseFromParent();
          changed = true;
          break;
        }
      }
    }
    if (changed)
      simplifyAndConstantFoldTerminators(F);
  }

  bool runOnCoroutine(Function& F) {
    // TODO: try alias analysis
    //RemoveNoOptAttribute(F);
    RemoveLifetimeIntrinsics(F);
#if 0
    Function* coroKill =
      Intrinsic::getDeclaration(M, Intrinsic::coro_kill2, { int32Ty });
    coroKill->dump();

    Function* coroSave =
      Intrinsic::getDeclaration(M, Intrinsic::coro_save, { int32Ty });
    coroSave->dump();

    Function* coroLoad =
      Intrinsic::getDeclaration(M, Intrinsic::coro_load, { int32Ty });
    coroLoad->dump();

    Function* coroFromPromise = M->getFunction("llvm.coro.from.promise.p0struct.minig::promise_type");
    //Intrinsic::getDeclaration(M, Intrinsic::coro_load, { int32Ty });

#endif
    for (auto it = inst_begin(F), end = inst_end(F); it != end;) {
      Instruction& I = *it++;

      CallSite CS(cast<Value>(&I));
      if (!CS)
        continue;
      const Function *Callee = CS.getCalledFunction();
      if (!Callee)
        continue; // indirect call, nothing to do
      if (Callee->isIntrinsic()) {
        if (Callee->getIntrinsicID() == Intrinsic::coro_suspend)
          ReplaceSuspendIfEmpty(*cast<IntrinsicInst>(&I));
        continue;
      }

      if (Callee->isDeclaration())
        continue;

      // only inline void and bool returning functions
      const Type *RetType = Callee->getReturnType();
      if (RetType == voidTy) ReplaceIfEmpty(I, *Callee);
      else if (RetType == boolTy) ReplaceIfConstant(I, *Callee);
    }
    return true;
  }

  void handleRampFunction(Function& F) {
    SmallVector<CallSite, 8> CallSites;
    for (Instruction& I : instructions(F))
      if (auto CS = CallSite(&I))
        CallSites.push_back(CS);

    for (auto CS : CallSites) {
      InlineFunctionInfo IFI;
      InlineFunction(CS, IFI);
    }
  }

  // inline small functions into its parent until we hit a coroutine
  void handleFromPromise(Function& F) {
    for (auto& U : F.uses()) {
      User *UR = U.getUser();

      if (!isa<CallInst>(UR) && !isa<InvokeInst>(UR))
        continue;

      CallSite CS(cast<Instruction>(UR));
      if (!CS.isCallee(&U))
        continue;

      InlineFunctionInfo IFI;
      InlineFunction(CS, IFI);
    }
  }

  bool runOnModule(Module &M) override {
    CoroutineCommon::PerModuleInit(M);
    RampFunctions.clear();

    bool changed = false;
    for (Function &F : M.getFunctionList()) {
      if (isCoroutine(F))
        changed |= runOnCoroutine(F);
      else switch (F.getIntrinsicID()) {
      default:
        continue;
      case Intrinsic::coro_from_promise:
        handleFromPromise(F);
        break;
        // why these are here?
      case Intrinsic::coro_destroy:
      case Intrinsic::coro_resume:
        break;
      }
    }

    for (Function* F : RampFunctions) {
      handleRampFunction(*F);
      changed = true;
    }
    return changed;
  }
};
#endif
}
char CoroPreSplit::ID = 0;
static RegisterPass<CoroPreSplit> Y6("CoroPreSplit", "inline little things");
namespace llvm {
  Pass *createCoroPreSplit() { return new CoroPreSplit(); }
}
