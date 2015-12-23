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
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Coroutines.h"
#include "llvm/Transforms/IPO/InlinerPass.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
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
      SuspendBr = BranchInst::Create(ResumeBB, CleanupBB, SuspendInst, BB);

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

    ConstantInt *getIndex() const {
      return cast<ConstantInt>(SuspendInst->getOperand(2));
    }

    bool isFinalSuspend() const { return getIndex()->isZero(); }
    explicit operator bool() const { return SuspendInst; }
  };

  struct SuspendInfo {
    SmallPtrSet<BasicBlock*, 8> SuspendBlocks;
    SmallVector<SuspendPoint, 8> SuspendPoints;
    bool HasFinalSuspend;

    // Canonical suspend is where
    // an @llvm.coro.suspend is followed by a
    // branch instruction 
    bool canonicalizeSuspends(Function& F) {
      bool changed = false;
      SuspendPoints.clear();
      HasFinalSuspend = false;
      for (auto BI = F.begin(), BE = F.end(); BI != BE;) {
        auto& BB = *BI++;
        for (auto &I : BB)
          if (SuspendPoint SI{ I }) {
            changed |= SI.canonicalize();
            SuspendPoints.push_back(SI);
            HasFinalSuspend |= SI.isFinalSuspend();
            break;
          }
      }
      if (changed)
        CoroutineCommon::simplifyAndConstantFoldTerminators(F);

      SuspendBlocks.clear();
      for (auto SP : SuspendPoints)
        SuspendBlocks.insert(SP.SuspendInst->getParent());

      return changed;
    }

    bool isSuspendBlock(BasicBlock* BB) const { return SuspendBlocks.count(BB); }
  };

  Function* ThisFunction;

  void processValue(Instruction *DefInst, DominatorTree &DT,
                    SuspendInfo const &Info,
                    SmallVectorImpl<AllocaInst *> &SharedAllocas) {

    BasicBlock* DefBlock = DefInst->getParent();
    AllocaInst* Spill = nullptr;

    for (auto UI = DefInst->use_begin(), UE = DefInst->use_end(); UI != UE;) {
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
          if (auto Gep = dyn_cast<GetElementPtrInst>(DefInst)) {
            if (isa<AllocaInst>(Gep->getPointerOperand()))
              if (Gep->hasAllConstantIndices()) {
                auto Dup = Gep->clone();
                DEBUG(dbgs() << "Cloned: " << *Dup << "\n");
                Dup->insertBefore(InsertPt);
                U.set(Dup);
                break;
              }
          }

          // see if we already created a spill slot
          // otherwise, create a spill slot
          if (!Spill) {
            Function* F = DefBlock->getParent();
            Spill = new AllocaInst(DefInst->getType(),
                                   DefInst->getName() + ".spill.alloca",
                                   F->getEntryBlock().getTerminator());
            new StoreInst(DefInst, Spill, DefInst->getNextNode());
            SharedAllocas.push_back(Spill);
          }

          // load from the spill slot
          auto Reload = new LoadInst(Spill, DefInst->getName() + ".spill", InsertPt);
          U.set(Reload);
          DEBUG(dbgs() << "Created spill: " << *Reload << "\n");
          break;
        }
        BB = DT[BB]->getIDom()->getBlock();
      }
    }
  }

  void insertSpills(Function &F, DominatorTree &DT, SuspendInfo const &Info,
                    SmallVectorImpl<AllocaInst *>& SharedAllocas) {

    SmallVector<Instruction*, 8> Values;
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
      processValue(Value, DT, Info, SharedAllocas);
    }
  }

  struct CoroutineInfo {
    SmallVector<AllocaInst*, 4> ResumeAllocas;
    SmallVector<AllocaInst*, 8> SharedAllocas;
    SmallPtrSet<BasicBlock*, 16> PostStartBlocks;
    BasicBlock* ReturnBlock;
    IntrinsicInst* CoroInit;
    IntrinsicInst* CoroDone;
    BasicBlock* Unreachable;

    CoroutineInfo() {}
    CoroutineInfo(CoroutineInfo const&) = delete;
    CoroutineInfo& operator=(CoroutineInfo const&) = delete;

    BasicBlock* findReturnBlock(Function& F) {
      for (auto& I : instructions(F))
        if (auto II = dyn_cast<IntrinsicInst>(&I))
          if (II->getIntrinsicID() == Intrinsic::coro_done)
            if (isa<ConstantPointerNull>(II->getOperand(0))) {
              assert(II->getNumUses() == 1 && "@llvm.coro.done unexpected num users");
              CoroDone = II;
              auto BR = cast<BranchInst>(II->user_back());
              auto ReturnBlock = BR->getSuccessor(0);
              assert(isa<ReturnInst>(ReturnBlock->getTerminator()));
              return ReturnBlock;
            }
      llvm_unreachable("did not find @llvm.coro.done marking the return block");
    }

    void analyzeFunction(Function &F, SuspendInfo &Info) {
      ReturnBlock = findReturnBlock(F);
      CoroInit = FindIntrinsic(F, Intrinsic::coro_init);
      assert(CoroInit && "missing @llvm.coro.init");

      ResumeAllocas.clear();
      SharedAllocas.clear();
      PostStartBlocks.clear();
      Unreachable = BasicBlock::Create(F.getContext(), "unreach", &F);
      new UnreachableInst(F.getContext(), Unreachable);

      for (auto SP : Info.SuspendPoints) {
        ComputeAllSuccessors(SP.getCleanupBlock(), PostStartBlocks);
        if (!SP.isFinalSuspend())
          ComputeAllSuccessors(SP.getResumeBlock(), PostStartBlocks);
      }
      PostStartBlocks.erase(ReturnBlock);

      for (auto& I : instructions(F)) {
        if (auto AI = dyn_cast<AllocaInst>(&I)) {
          assert(isa<ConstantInt>(AI->getArraySize()) && "cannot handle non-const allocas yet");
          if (AI->getName() == "__promise") {
            // promise must be always in the shared state
            // and it must be the first field
            SharedAllocas.push_back(AI);
            if (SharedAllocas.size() > 1)
              std::swap(SharedAllocas.front(), SharedAllocas.back());
            continue;
          }
          bool seenInStart = false;
          bool seenInResume = false;
          for (User* U : AI->users()) {
            Instruction *UI = cast<Instruction>(U);
            bool inResume = PostStartBlocks.count(UI->getParent());
            seenInStart |= !inResume;
            seenInResume |= inResume;
          }
          if (seenInResume)
            if (seenInStart)
              SharedAllocas.push_back(AI);
            else
              ResumeAllocas.push_back(AI);
          else
            if (!seenInStart)
              errs() << "dead alloca: " << *AI << "\n";
        }
      }
    }
  };

  // TODO: move them to some struct
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

  Function *CreateAuxillaryFunction(Twine suffix, Value *&frame) {
    auto func = Function::Create(resumeFnTy, GlobalValue::InternalLinkage,
      ThisFunction->getName() + suffix, M);
    func->setCallingConv(CallingConv::Fast);
    frame = &*func->arg_begin();
    frame->setName("frame.ptr" + suffix);
    return func;
  }

  Function *resumeFn = nullptr;
  Function *destroyFn = nullptr;
  Value *frameInDestroy = nullptr;
  Value *frameInResume = nullptr;
  Value *frameInRamp = nullptr;

  void CreateAuxillaryFunctions() {
    resumeFn = CreateAuxillaryFunction(".resume", frameInResume);
    destroyFn = CreateAuxillaryFunction(".destroy", frameInDestroy);
  }

  void createFrameStruct(SmallVectorImpl<AllocaInst *>& SharedAllocas) {
    SmallVector<Type *, 8> typeArray;

    typeArray.clear();
    typeArray.push_back(resumeFnPtrTy); // 0 res-type
    typeArray.push_back(resumeFnPtrTy); // 1 dtor-type
    typeArray.push_back(int32Ty);       // 2 index

    for (AllocaInst *AI : SharedAllocas) {
      typeArray.push_back(AI->getType()->getElementType());
    }
    frameTy->setBody(typeArray);

    // TODO: when we optimize storage layout, keep coro_size as intrinsic
    // for later passes to plug in the right amount
    const DataLayout &DL = M->getDataLayout();
    APInt size(32, DL.getTypeAllocSize(frameTy));
    ReplaceIntrinsicWith(*ThisFunction, Intrinsic::coro_size, ConstantInt::get(int32Ty, size));
  }
  // replace all uses of allocas with gep from frame struct
  void ReplaceSharedUses(CoroutineInfo const& Info) {
    APInt fieldNo(32, 2); // Fields start with after 2
    for (AllocaInst *AI : Info.SharedAllocas) {
      smallString = AI->getName();
      if (smallString == "__promise") {
        assert(fieldNo == 2 && "promise shall be the first field");
      }
      AI->setName(""); // FIXME: use TakeName
      auto index = ConstantInt::get(M->getContext(), ++fieldNo);

      while (!AI->use_empty()) {
        Use &U = *AI->use_begin();
        User *user = U.getUser();
        Value *frame = frameInRamp;
        auto gep =
          GetElementPtrInst::Create(frameTy, frame, { zeroConstant, index },
            smallString, cast<Instruction>(user));
        U.set(gep);
      }
      AI->eraseFromParent();
    }

    // we may end up replacing allocas with gep before frame is defined
    // move definition of frame to the beginning of function
    InstrSetVector coroFrameUses;
    ComputeDefChainNotIn(Info.CoroInit, Info.PostStartBlocks, coroFrameUses);
    MoveInReverseOrder(coroFrameUses, &*inst_begin(ThisFunction));
  }

  static void fixupPhiNodes(BasicBlock *Target, BasicBlock *OldPred,
                            BasicBlock *NewPred) {
    for (Instruction& I : *Target) {
      PHINode* PN = dyn_cast<PHINode>(&I);
      if (!PN)
        break;

      auto N = PN->getNumIncomingValues();
      for (unsigned i = 0; i != N; ++i)
        if (PN->getIncomingBlock(i) == OldPred)
          PN->setIncomingBlock(i, NewPred);
    }
  }

  void prepareFrame(CoroutineInfo& CoroInfo) {
    createFrameStruct(CoroInfo.SharedAllocas);
    auto InsertPt = CoroInfo.CoroInit->getNextNode();

    frameInRamp = new BitCastInst(CoroInfo.CoroInit, framePtrTy, "frame",
      InsertPt);
    auto gep0 = GetElementPtrInst::Create(
      frameTy, frameInRamp, { zeroConstant, zeroConstant }, "", InsertPt);
    new StoreInst(resumeFn, gep0, InsertPt);

    auto gep1 = GetElementPtrInst::Create(
      frameTy, frameInRamp, { zeroConstant, oneConstant }, "", InsertPt);
    new StoreInst(destroyFn, gep1, InsertPt);
  }

  bool runOnCoroutine(Function& F) {
    DEBUG(dbgs() << "CoroSplit function: " << F.getName() << "\n");

    SuspendInfo Suspends;
    CoroutineInfo CoroInfo;

    init(F);
    CreateAuxillaryFunctions();
    //assert(F.getPrefixData() == nullptr && "coroutine should not have function prefix");
    //F.setPrefixData(destroyFn);

    DominatorTreeWrapperPass& DTA = getAnalysis<DominatorTreeWrapperPass>(F);
    // FIXME: make canonicalize update DT
    if (Suspends.canonicalizeSuspends(F)) {
      DTA.runOnFunction(F);
    }
    CoroInfo.analyzeFunction(F, Suspends);

    DominatorTree &DT = DTA.getDomTree();
    insertSpills(F, DT, Suspends, CoroInfo.SharedAllocas);

    // move this into PrepareFrame func
    prepareFrame(CoroInfo);

    ReplaceSharedUses(CoroInfo);

    for (auto InResume : CoroInfo.ResumeAllocas)
      errs() << "resume alloca: " << *InResume << "\n";

    BasicBlock* ResumeEntry = createSwitch("resume.entry", CoroInfo, Suspends);
    BasicBlock *DestroyEntry =
        createSwitch("destroy.entry", CoroInfo, Suspends, /*destroy=*/true);

    replaceSuspends(CoroInfo, Suspends);

    ThisFunction->removeFnAttr(Attribute::Coroutine);

    createResumeOrDestroy(resumeFn, ResumeEntry, frameInResume, CoroInfo, Suspends);
    createResumeOrDestroy(destroyFn, DestroyEntry, frameInDestroy, CoroInfo, Suspends);
    CoroInfo.CoroDone->replaceAllUsesWith(ConstantInt::getFalse(M->getContext()));
    removeUnreachableBlocks(F);
    simplifyAndConstantFoldTerminators(F);
    removeUnreachableBlocks(F);

    ReplaceIntrinsicWith(*ThisFunction, Intrinsic::coro_frame, CoroInfo.CoroInit);

    return true;
  }

  BasicBlock* createSwitch(StringRef Name, CoroutineInfo &Info, SuspendInfo &Suspends,
                    bool Destroy = false) {
    auto Entry = BasicBlock::Create(M->getContext(), Name, ThisFunction);
    auto CaseCount =
      Suspends.SuspendPoints.size() - ((Destroy || Suspends.HasFinalSuspend) ? 1 : 0);

    auto gepIndex = GetElementPtrInst::Create(
      frameTy, frameInRamp, { zeroConstant, twoConstant }, "", Entry);
    auto index = new LoadInst(gepIndex, "index", Entry);
    auto switchInst = SwitchInst::Create(index, Info.Unreachable, CaseCount, Entry);

    for (auto SP : Suspends.SuspendPoints) {
      BasicBlock* Target = nullptr;
      if (Destroy)
        Target = SP.getCleanupBlock();
      else if (SP.isFinalSuspend())
        continue;
      else 
        Target = SP.getResumeBlock();
      switchInst->addCase(SP.getIndex(), Target);
      fixupPhiNodes(Target, SP.SuspendInst->getParent(), Entry);
    }
    return Entry;
  }

  void CallAwaitSuspend(IntrinsicInst *I, Value *FramePtr) {
    auto vFrame = new BitCastInst(FramePtr, bytePtrTy, "", I);
    Value *op = I->getArgOperand(1);
    while (const ConstantExpr *CE = dyn_cast<ConstantExpr>(op)) {
      if (!CE->isCast())
        break;
      // Look through the bitcast
      op = cast<ConstantExpr>(op)->getOperand(0);
    }
    Function* fn = cast<Function>(op);
    assert(fn->getType() == awaitSuspendFnPtrTy && "unexpected await_suspend fn type");

    auto call = CallInst::Create(fn, { I->getArgOperand(0), vFrame }, "", I);
    InlineFunctionInfo IFI;
    InlineFunction(call, IFI);
  }

  void replaceSuspends(CoroutineInfo &Info, SuspendInfo const &Suspends) {
    for (auto SP : Suspends.SuspendPoints) {
      BranchInst::Create(Info.ReturnBlock, SP.SuspendBr);
      SP.SuspendBr->eraseFromParent();
      auto gep = GetElementPtrInst::Create(frameTy, frameInRamp,
      { zeroConstant, twoConstant }, "",
        SP.SuspendInst);
      new StoreInst(SP.getIndex(), gep, SP.SuspendInst);
      CallAwaitSuspend(SP.SuspendInst, frameInRamp);
      SP.SuspendInst->eraseFromParent();
    }
  }

  void createResumeOrDestroy(
    Function * NewFn,
    BasicBlock* CaseBlock,
    Value* FramePtr,
    CoroutineInfo &CoroInfo, SuspendInfo& Suspends) {

    ValueToValueMapTy VMap;
    for (auto &A : ThisFunction->args())
      VMap[&A] = UndefValue::get(A.getType());

    VMap[frameInRamp] = FramePtr;

    SmallVector<ReturnInst*, 4> Returns;
    CloneFunctionInto(NewFn, ThisFunction, VMap, false, Returns);

    auto Entry = cast<BasicBlock>(VMap[CaseBlock]);
    Entry->removeFromParent();
    Entry->insertInto(NewFn, &*NewFn->begin());

    auto Exit = BasicBlock::Create(M->getContext(), "exit", NewFn);
    ReturnInst::Create(M->getContext(), Exit);
    for (auto RI : Returns) {
      auto RB = RI->getParent();
      RB->replaceAllUsesWith(Exit);
      RB->eraseFromParent();
    }

    auto InsertPt = &Entry->front();

    for (auto & AI : CoroInfo.ResumeAllocas) {
      auto clone = cast<AllocaInst>(VMap[AI]);
      clone->removeFromParent();
      clone->insertBefore(InsertPt);
    }

    VMap[frameInRamp]->replaceAllUsesWith(FramePtr);

    BlockSet OldEntryBlocks;
    InstrSetVector Used;
    auto OldEntry = cast<BasicBlock>(VMap[CoroInfo.CoroInit->getParent()]);
    OldEntryBlocks.insert(OldEntry);
    for (auto& I : *OldEntry)
      for (User* U : I.users())
        if (cast<Instruction>(U)->getParent() != OldEntry) {
          ComputeDefChain(&I, OldEntryBlocks, Used);
          break;
        }
    MoveInReverseOrder(Used, InsertPt);

    auto vFrame = new BitCastInst(FramePtr, bytePtrTy, "", InsertPt);
    ReplaceIntrinsicWith(*NewFn, Intrinsic::coro_frame, vFrame);

    removeUnreachableBlocks(*NewFn);
    simplifyAndConstantFoldTerminators(*NewFn);
    removeUnreachableBlocks(*NewFn);
    NewFn->setCallingConv(CallingConv::Fast);
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
