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
#include "llvm/Analysis/InlineCost.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/IPO/InlinerPass.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define DEBUG_TYPE "coro-split2"

namespace {
struct CoroutineInfo : CoroutineCommon {
  Function *ThisFunction;

  IntrinsicInst *CoroInit;
  SuspendPoint CoroDone;
  SmallVector<SuspendPoint, 4> Suspends;
  SmallVector<AllocaInst *, 8> AllAllocas;
  BasicBlock *ReturnBlock;

  BlockSet RampBlocks;
  BlockSet CleanupBlocks;

  Function *resumeFn = nullptr;
  Function *destroyFn = nullptr;
  Function *cleanupFn = nullptr;

  SmallString<16> smallString;
  StructType *frameTy = nullptr;
  PointerType *framePtrTy = nullptr;
  FunctionType *resumeFnTy = nullptr;
  PointerType *resumeFnPtrTy = nullptr;

  void init(Function &F) {
    this->ThisFunction = &F;
    smallString.clear();
    frameTy = StructType::create(
        M->getContext(), (F.getName() + ".frame").toStringRef(smallString));
    framePtrTy = PointerType::get(frameTy, 0);
    resumeFnTy = FunctionType::get(voidTy, framePtrTy, false);
    resumeFnPtrTy = PointerType::get(resumeFnTy, 0);
  }

  Value *frameInDestroy;
  Value *frameInCleanup;
  Value *frameInResume;
  Instruction *frameInRamp;

  Value *vFrameInDestroy;
  Value *vFrameInCleanup;
  Value *vFrameInResume;
  Instruction *vFrameInRamp;

  Function *CreateAuxillaryFunction(Twine suffix, Value *&frame,
                                    Value *&vFrame) {
    auto func = Function::Create(resumeFnTy, GlobalValue::InternalLinkage,
                                 ThisFunction->getName() + suffix, M);
    func->setCallingConv(CallingConv::Fast);
    frame = &*func->arg_begin();
    frame->setName("frame.ptr");

    auto entry = BasicBlock::Create(M->getContext(), "entry", func);
    vFrame = new BitCastInst(frame, bytePtrTy, "frame.void.ptr", entry);
    return func;
  }

  void CreateAuxillaryFunctions() {
    resumeFn =
        CreateAuxillaryFunction(".resume", frameInResume, vFrameInResume);
    cleanupFn =
        CreateAuxillaryFunction(".cleanup", frameInCleanup, vFrameInCleanup);
    destroyFn =
        CreateAuxillaryFunction(".destroy", frameInDestroy, vFrameInDestroy);

    auto CoroInit = FindIntrinsic(*ThisFunction, Intrinsic::coro_init);
    assert(CoroInit && "Call to @llvm.coro.init is missing");
    vFrameInRamp = CoroInit;
    frameInRamp = new BitCastInst(vFrameInRamp, framePtrTy, "frame",
                                  CoroInit->getNextNode());
  }

  void AnalyzeFunction(Function &F) {
    init(F);

    CoroInit = nullptr;
    CoroDone.clear();
    Suspends.clear();
    AllAllocas.clear();

    for (auto &I : instructions(F)) {
      if (auto AI = dyn_cast<AllocaInst>(&I)) {
        AllAllocas.emplace_back(AI);
        continue;
      }
      if (auto II = dyn_cast<IntrinsicInst>(&I)) {
        switch (II->getIntrinsicID()) {
        default:
          continue;
        case Intrinsic::coro_done:
          if (isa<ConstantPointerNull>(II->getArgOperand(0)))
            CoroDone = SuspendPoint(II);
          continue;
        case Intrinsic::coro_suspend:
          Suspends.emplace_back(II);
          continue;
        case Intrinsic::coro_init:
          CoroInit = II;
          continue;
        }
      }
    }

    assert(CoroInit && "missing @llvm.coro.init");
    assert(CoroDone.SuspendInst && "missing @llvm.coro.done start marker");
    assert(Suspends.size() != 0 && "cannot handle no suspend coroutines yet");

    ReturnBlock = CoroDone.IfTrue;

    RampBlocks.clear();
    // TODO: better name Add
    ComputeAllPredecessors(CoroDone.SuspendInst->getParent(), RampBlocks);
    ComputeAllSuccessors(ReturnBlock, RampBlocks);

    CleanupBlocks.clear();
    // everything that cleanup branches from suspends point to
    for (SuspendPoint &SP : Suspends)
      ComputeAllSuccessors(SP.IfFalse, CleanupBlocks);
    // excluding the ramp blocks
    for (BasicBlock *BB : RampBlocks)
      CleanupBlocks.erase(BB);
  }

  BasicBlock *DestroyCallBlock;
  bool TryReplaceWithDestroyCall(BranchInst &BI, unsigned index) {
    BasicBlock *Successor = BI.getSuccessor(index);

    // see if there is straight line to the cleanup block
    while (CleanupBlocks.count(Successor) == 0) {
      // can't have suspends
      if (SuspendPoint SP{Successor})
        return false;

      Successor = Successor->getSingleSuccessor();
      if (Successor == nullptr)
        return false;
    }

    // create one if does not exist
    if (DestroyCallBlock == nullptr) {
      DestroyCallBlock =
          BasicBlock::Create(M->getContext(), "cleanup", ThisFunction);
      // FIXME: what if we are elided?
      // FIXME: add call to Destroy?
      BranchInst::Create(ReturnBlock, DestroyCallBlock);
      RampBlocks.insert(DestroyCallBlock);
    }

    BI.setSuccessor(index, DestroyCallBlock);
    return true;
  }

  void ReplaceSuccessors(BasicBlock *B, BasicBlock *oldTarget,
                         BasicBlock *newTarget) {
    // FIXME: can't handle anything but the branch instructions
    auto BI = cast<BranchInst>(B->getTerminator());
    unsigned ReplacedCount = 0;
    for (unsigned i = 0; i < BI->getNumSuccessors(); ++i) {
      if (BI->getSuccessor(i) == oldTarget) {
        BI->setSuccessor(i, newTarget);
        ++ReplacedCount;
      }
    }
    assert(ReplacedCount > 0 && "failed to replace any successors");
  }

  BasicBlock *CloneBlock(BasicBlock *BB, BasicBlock *PrevBlock) {
    ValueToValueMapTy VMap;
    auto NewBB = CloneBasicBlock(BB, VMap, ".clone", ThisFunction);
    ReplaceSuccessors(PrevBlock, BB, NewBB);
    for (Instruction &I : *NewBB)
      RemapInstruction(&I, VMap,
                       RF_NoModuleLevelChanges | RF_IgnoreMissingEntries);
    return NewBB;
  }

  BasicBlock *FirstResume = nullptr;

  bool runOnCoroutine(Function &F) {
    RemoveNoOptAttribute(F);
    RemoveFakeSuspends(F);
    AnalyzeFunction(F);
    CreateAuxillaryFunctions();

    // find the first suspend
    BasicBlock *PrevBlock = CoroDone.SuspendInst->getParent();
    BasicBlock *Candidate = CoroDone.IfFalse;
    DestroyCallBlock = nullptr;

    for (;;) {
      while (Candidate->getSinglePredecessor() == 0) {
        Candidate = CloneBlock(Candidate, PrevBlock);
      }
      RampBlocks.insert(Candidate);
      auto T = Candidate->getTerminator();
      if (SuspendPoint SP{Candidate}) {
        auto T = Candidate->getTerminator();
        BranchInst::Create(ReturnBlock, T);
        // FIXME: check that it is not final suspend
        FirstResume = SP.IfTrue;
        T->eraseFromParent();
        break;
      }
      PrevBlock = Candidate;
      if (auto Next = Candidate->getSingleSuccessor()) {
        Candidate = Next;
        continue;
      }
      auto BI = dyn_cast<BranchInst>(T);
      assert(BI && "Cannot handle any terminator but BranchInst yet");
      if (TryReplaceWithDestroyCall(*BI, 0))
        Candidate = BI->getSuccessor(1);
      else if (TryReplaceWithDestroyCall(*BI, 1))
        Candidate = BI->getSuccessor(0);
      else
        llvm_unreachable("cannot handle branches where one success does not "
                         "lead to cleanup");
    }

    return true;
  }
};
}

namespace {
#if 1
// incubating new coro-split path

struct CoroSplit2 : public ModulePass, CoroutineInfo {
  static char ID; // Pass identification, replacement for typeid
  CoroSplit2() : ModulePass(ID) {}

  SmallVector<Function *, 8> Coroutines;

  bool runOnModule(Module &M) override {
    CoroutineCommon::PerModuleInit(M);

    bool changed = false;
    for (Function &F : M.getFunctionList())
      if (isCoroutine(F)) {
        changed |= runOnCoroutine(F);
      }
    return changed;
  }
};
#endif
#if 0
  struct CoroSplit2 : public ModulePass, CoroutineCommon {
    static char ID; // Pass identification, replacement for typeid
    CoroSplit2() : ModulePass(ID) {}

    bool runOnModule(Module &M) override {
      CoroutineCommon::PerModuleInit(M);

      bool changed = false;
      for (Function &F : M.getFunctionList())
        if (isCoroutine(F))
          changed |= runOnCoroutine(F);
      return changed;
    }
  };
#endif
#if 0
  class CoroSplit2 : public Inliner {
  public:
    CoroSplit2() : Inliner(ID) {
      initializeSimpleInlinerPass(*PassRegistry::getPassRegistry());
    }
    static char ID; // Pass identification, replacement for typeid

    InlineCost getInlineCost(CallSite CS) override {
      return InlineCost::getNever();
    }

    bool runOnSCC(CallGraphSCC &SCC) override {
      return Inliner::runOnSCC(SCC);
    }
    //void getAnalysisUsage(AnalysisUsage &AU) const override;
  };
#endif
}
char CoroSplit2::ID = 0;
namespace llvm {
INITIALIZE_PASS_BEGIN(CoroSplit2, "coro-split2", "Split coroutine into ramp/resume/destroy/cleanup functions v2",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(CoroSplit2, "coro-split2", "Split coroutine into ramp/resume/destroy/cleanup functions v2",
                    false, false)

Pass *createCoroSplit2() { return new CoroSplit2(); }
}
