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
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/IPO/InlinerPass.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Type.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define DEBUG_TYPE "coro-split2"

namespace {
struct CoroutineInfo : CoroutineCommon {
  Function *ThisFunction;

  IntrinsicInst *CoroInit;
  IntrinsicInst *CoroDelete;

  struct Suspend {
    IntrinsicInst* SuspendInst;
    BasicBlock* OnResume = nullptr;
    BasicBlock* OnCleanup = nullptr;

    Suspend(IntrinsicInst* I) : SuspendInst(I) {}
  };

  SmallVector<Suspend, 8> Suspends;
  SmallVector<AllocaInst *, 8> AllAllocas;
  SmallSetVector<AllocaInst *, 8> SharedAllocas;
  BasicBlock *ReturnBlock;
  BasicBlock *DeleteBlock;

  BlockSet RampBlocks;
  BlockSet CleanupBlocks;

  Function *resumeFn = nullptr;
  Function *destroyFn = nullptr;
  Function *cleanupFn = nullptr;
  Function *startFn = nullptr;

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
  Value *frameInStart;
  Instruction *frameInRamp;

  Function *CreateAuxillaryFunction(Twine suffix, Value *&frame) {
    auto func = Function::Create(resumeFnTy, GlobalValue::InternalLinkage,
                                 ThisFunction->getName() + suffix, M);
    func->setCallingConv(CallingConv::Fast);
    frame = &*func->arg_begin();
    frame->setName("frame.ptr" + suffix);
    return func;
  }

  void CreateAuxillaryFunctions() {
    resumeFn = CreateAuxillaryFunction(".resume", frameInResume);
    cleanupFn = CreateAuxillaryFunction(".cleanup", frameInCleanup);
    destroyFn = CreateAuxillaryFunction(".destroy", frameInDestroy);
    startFn = CreateAuxillaryFunction(".start", frameInStart);

    auto CoroInit = FindIntrinsic(*ThisFunction, Intrinsic::coro_init);
    assert(CoroInit && "Call to @llvm.coro.init is missing");
    frameInRamp = new BitCastInst(CoroInit, framePtrTy, "frame",
                                  CoroInit->getNextNode());
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

  BasicBlock *MoveInstructionWithItsUsers(Twine Name, Instruction* Instr) {  
    Function* Fn = Instr->getParent()->getParent();
    SmallSet<Instruction*, 8> InstructionsToMove;
    SmallVector<Instruction*, 8> Worklist;
    Worklist.push_back(Instr);

    while (!Worklist.empty()) {
      Instruction* I = Worklist.pop_back_val();
      if (isa<AllocaInst>(I))
        continue;

      InstructionsToMove.insert(I);
      for (Use& Operand : I->operands()) {
        if (auto Op = dyn_cast<Instruction>(&Operand)) {
          if (InstructionsToMove.count(Op) == 0)
            Worklist.push_back(Op);
          continue;
        }
        assert(!isa<Argument>(&Operand) && "Cannot move");
      }
    }

    if (isa<TerminatorInst>(Instr)) {
      // if we are removing a terminator, put a ret instruction
      // instead
      ReturnInst::Create(M->getContext(), Instr->getParent());
    }

    Worklist.clear();
    for (auto& I : instructions(Fn))
      if (InstructionsToMove.count(&I) != 0)
        Worklist.push_back(&I);

    InstructionsToMove.clear();

    auto NewBlock = BasicBlock::Create(M->getContext(), Name, Fn);
    auto InsertionPt = ReturnInst::Create(M->getContext(), NewBlock);
    for (auto I: Worklist) {
      I->removeFromParent();
      I->insertBefore(InsertionPt);
    }
    InsertionPt->eraseFromParent();

    // check that all users are in the new block
    for (auto& I : *NewBlock)
      for (auto U : I.users())
        if (auto I = dyn_cast<Instruction>(U))
          assert(I->getParent() == NewBlock && "all users must be in the same block");

    return NewBlock;
  }

  // replace all uses of allocas with gep from frame struct
  void ReplaceSharedUses() {
    APInt fieldNo(32, 2); // Fields start with after 2
    for (AllocaInst *AI : SharedAllocas) {
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
    ComputeDefChain(frameInRamp, RampBlocks, coroFrameUses);
    MoveInReverseOrder(coroFrameUses, &*inst_begin(ThisFunction));
  }

  void CreateFrameStruct() {
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

  bool SingleSuspend;

  void AnalyzeFunction(Function& F) {
    ReturnInst* Return = nullptr;
    CoroInit = nullptr;
    CoroDelete = nullptr;
    bool SuspendInInitBlock = false;

    BasicBlock *InitBlock = nullptr;

    for (auto &BB : F) {
      auto *Term = BB.getTerminator();
      if (auto RI = dyn_cast<ReturnInst>(Term)) {
        assert(!Return && "cannot handle multiple returns yet");
        Return = RI;
      }

      for (auto &I : BB) {
        if (auto AI = dyn_cast<AllocaInst>(&I)) {
          AllAllocas.emplace_back(AI);
          continue;
        }
        if (auto II = dyn_cast<IntrinsicInst>(&I)) {
          switch (II->getIntrinsicID()) {
          default:
            continue;
          case Intrinsic::coro_suspend:
            Suspends.emplace_back(II);
            assert(InitBlock && "@llvm.coro.suspend before @llvm.coro.init");
            if (II->getParent() == InitBlock)
              SuspendInInitBlock = true;
            continue;
          case Intrinsic::coro_init:
            assert(!CoroDelete && "more than one @llvm.coro.init in a coroutine");
            CoroInit = II;
            InitBlock = II->getParent();
            continue;
          case Intrinsic::coro_delete:
            assert(!CoroDelete && "more than one @llvm.coro.delete in a coroutine");
            CoroDelete = II;
            continue;
          }
        }
      }
    }
    assert(CoroInit && "missing @llvm.coro.init");
    assert(CoroDelete && "missing @llvm.coro.delete");
    assert(Return && "missing ret instruction");
    assert(Suspends.size() != 0 && "cannot handle no suspend coroutines yet");

    auto StartBlock = InitBlock->getSingleSuccessor();
    assert(StartBlock && "expecting a single successor of InitBlock");

    Suspend* FinalSuspend = nullptr;
    // part 1, split out suspends and fill out the branches
    for (auto &S : Suspends) {
      auto CI = cast<ConstantInt>(S.SuspendInst->getOperand(2));
      APInt Val = CI->getValue();

      if (S.SuspendInst->getNumUses() == 0) {
        // split up the block
        // FIXME: more validation
        auto next = S.SuspendInst->getParent()->splitBasicBlock(
            S.SuspendInst->getNextNode(), Twine("resume.") + Twine(Val.getZExtValue()));
        S.OnResume = S.OnCleanup = next;
        continue;
      }
      assert(S.SuspendInst->getNumUses() == 1 && "unexpected number of uses");
      auto BR = cast<BranchInst>(S.SuspendInst->user_back());
      if (BR->getNumSuccessors() == 1) {
        S.OnResume = S.OnCleanup = BR->getSuccessor(0);
      }
      else {
        S.OnResume = BR->getSuccessor(0);
        S.OnCleanup = BR->getSuccessor(1);
      }
      // if it is final suspend, correct the branch to point only at
      // the cleanup
      if (Val == 0) {
        S.OnResume = nullptr;
        FinalSuspend = &S;
      }
    }

    // part 2, split out return instruction into its own block
    ReturnBlock = MoveInstructionWithItsUsers("ramp.return", Return);
    RampBlocks.clear();
    RampBlocks.insert(InitBlock);
    RampBlocks.insert(ReturnBlock);

    // part 3, figure out allocas 
    for (auto AI: AllAllocas) {
      for (User* U : AI->users())
        if (auto I = dyn_cast<Instruction>(U))
          if (I->getParent() != InitBlock && I->getParent() != ReturnBlock) {
            SharedAllocas.insert(AI);
            break;
          }
    }

    auto realSuspendNum = Suspends.size() - (FinalSuspend ? 1 : 0);
    if (realSuspendNum != 1)
      llvm_unreachable("realSuspendNum != 1. Cannot handle yet");

    SingleSuspend = (realSuspendNum == 1);

    // part 4, replace allocas 
    CreateFrameStruct();
    ReplaceSharedUses();

    // part 2.5 get rid of all suspends
    ReplaceSuspendsCFG();

    // part 5, duplicate the delete block and extract cleanup part from it
    if (SuspendInInitBlock)
      llvm_unreachable("SuspendInInitBlock. Cannot handle yet");

    // replace all frame uses with arg
    ReplaceAllUsesOutsideTheRamp(frameInRamp, frameInResume);

    // move all but ramp blocks in resumeFn
    Function::BasicBlockListType &oldBlocks = ThisFunction->getBasicBlockList();
    Function::BasicBlockListType &newBlocks = resumeFn->getBasicBlockList();
    for (auto it = ThisFunction->begin(), end = ThisFunction->end(); it != end;) {
      BasicBlock& BB = *it++;
      if (RampBlocks.count(&BB) == 0) {
        oldBlocks.remove(BB);
        newBlocks.push_back(&BB);
      }
    }

    // clone into start and cleanup

    SmallVector<ReturnInst*, 8> Returns;
    ValueToValueMapTy VMap;
    VMap[frameInResume] = frameInCleanup;
    CloneFunctionInto(cleanupFn, resumeFn,
      VMap, false,
      Returns);
    PolishCleanup(VMap[CoroDelete], VMap, FinalSuspend);

    Returns.clear();
    VMap.clear();
    VMap[frameInResume] = frameInStart;
    CloneFunctionInto(startFn, resumeFn,
      VMap, false,
      Returns);
    PolishStart(cast<BasicBlock>(VMap[StartBlock]));

    Returns.clear();
    VMap.clear();
    VMap[frameInResume] = frameInDestroy;
    CloneFunctionInto(destroyFn, resumeFn,
      VMap, false,
      Returns);
    PolishDestroy(VMap[CoroDelete]);

    PolishResume();
    PolishRamp(InitBlock);

    ReplaceCoroFrame(ThisFunction, frameInRamp);
    ReplaceCoroFrame(startFn, frameInStart);
    ReplaceCoroFrame(resumeFn, frameInResume);
    ReplaceCoroFrame(cleanupFn, frameInCleanup);
    ReplaceCoroFrame(destroyFn, frameInDestroy);

    ReplaceSuspends(ThisFunction, frameInRamp);
    ReplaceSuspends(startFn, frameInStart);
    ReplaceSuspends(resumeFn, frameInResume);
    ReplaceSuspends(cleanupFn, frameInCleanup);
    ReplaceSuspends(destroyFn, frameInDestroy);
  }

  void ReplaceCoroFrame(Function* F, Value* Frame) {
    for (auto it = inst_begin(F), e = inst_end(F); it != e;) {
      Instruction& I = *it++;
      if (auto II = dyn_cast<IntrinsicInst>(&I))
        if (II->getIntrinsicID() == Intrinsic::coro_frame) {
          auto vFrame = new BitCastInst(Frame, bytePtrTy, "", II);
          II->replaceAllUsesWith(vFrame);
          II->eraseFromParent();
        }
    }
  }

  void PolishCleanup(Value *CoroDelete, ValueToValueMapTy &VMap,
                     Suspend *FinalSuspend) {
    // find the intruction that uses delete
    assert(CoroDelete->getNumUses() == 1 && "unexpected number of uses");
    auto DeleteInstr = cast<Instruction>(CoroDelete->user_back());
    auto notNeeded = MoveInstructionWithItsUsers("", DeleteInstr);
    notNeeded->eraseFromParent();

    auto Entry = BasicBlock::Create(M->getContext(), "entry", cleanupFn,
      &cleanupFn->getEntryBlock());

    if (FinalSuspend) {
      auto gep = GetElementPtrInst::Create(
        frameTy, frameInCleanup, { zeroConstant, zeroConstant }, "", Entry);
      auto resumeAddr = new LoadInst(gep, "", Entry);
      auto isNull =
        new ICmpInst(*Entry, CmpInst::Predicate::ICMP_EQ,
          ConstantPointerNull::get(resumeFnPtrTy), resumeAddr);

      BasicBlock* swBlock = BasicBlock::Create(M->getContext(), "switch", cleanupFn);
      BranchInst::Create(cast<BasicBlock>(VMap[FinalSuspend->OnCleanup]),
                         swBlock, isNull, Entry);
      Entry = swBlock;
    }

    auto CaseCount = Suspends.size() - (FinalSuspend ? 1 : 0);

    auto Unreachable = BasicBlock::Create(M->getContext(), "unreach", cleanupFn);
    new UnreachableInst(M->getContext(), Unreachable);

    auto gepIndex = GetElementPtrInst::Create(
      frameTy, frameInCleanup, { zeroConstant, twoConstant }, "", Entry);
    auto index = new LoadInst(gepIndex, "resume.index", Entry);
    auto switchInst = SwitchInst::Create(index, Unreachable, CaseCount, Entry);
    for (unsigned i = 0; i < Suspends.size(); ++i) {
      Suspend& sp = Suspends[i];
      if (FinalSuspend != &sp) {
        switchInst->addCase(ConstantInt::get(M->getContext(), APInt(32, i)),
                            cast<BasicBlock>(VMap[sp.OnCleanup]));
      }
    }
  }

  void PolishDestroy(Value* CoroDelete) {
    // find the intruction that uses delete
    assert(CoroDelete->getNumUses() == 1 && "unexpected number of uses");
    auto DeleteInstr = cast<Instruction>(CoroDelete->user_back());
    auto entry = MoveInstructionWithItsUsers("entry", DeleteInstr);
    ReturnInst::Create(M->getContext(), entry);
    
    for (auto& BB : *destroyFn)
      if (&BB != entry)
        BB.dropAllReferences();

    for (auto BI = destroyFn->begin(), E = destroyFn->end(); BI != E;) {
      BasicBlock& BB = *BI++;
      if (&BB != entry)
        BB.eraseFromParent();
    }

    auto call = CallInst::Create(cleanupFn, { frameInDestroy }, "", &*entry->begin());
    call->setCallingConv(CallingConv::Fast);
  }

  void PolishRamp(BasicBlock* InitBlock) {
    auto Term = InitBlock->getTerminator();

    auto gep0 = GetElementPtrInst::Create(
      frameTy, frameInRamp, { zeroConstant, zeroConstant }, "", Term);
    new StoreInst(resumeFn, gep0, Term);

    auto gep1 = GetElementPtrInst::Create(
      frameTy, frameInRamp, { zeroConstant, oneConstant }, "", Term);
    new StoreInst(destroyFn, gep1, Term);


    if (auto BR = dyn_cast<BranchInst>(Term)) {
      assert(BR->getNumSuccessors() == 1 && "InitialBlock has unexpected number of successors");
      auto call = CallInst::Create(startFn, {frameInRamp}, "", BR);
      call->setCallingConv(CallingConv::Fast);
      BR->setSuccessor(0, ReturnBlock);
      return;
    }
    llvm_unreachable("cannot handle yet");
  }
  
  void PolishStart(BasicBlock* StartBlock) {
    auto Entry = BasicBlock::Create(M->getContext(), "entry", startFn,
      &startFn->getEntryBlock());

    BranchInst::Create(StartBlock, Entry);
  }

  void PolishResume() {
    auto entry = BasicBlock::Create(M->getContext(), "entry", resumeFn,
                                    &resumeFn->getEntryBlock());
    int Count = 0;
    BasicBlock* LastResume = nullptr;

    for (auto & SP : Suspends) {
      if (SP.OnResume) {
        ++Count;
        LastResume = SP.OnResume;
      }
    }

    assert(Count == 1 && "Cannot yet handle multple suspend points");

    BranchInst::Create(LastResume, entry);
  }

  void CallAwaitSuspend(IntrinsicInst *I, Value *Frame) {
    Value *op = I->getArgOperand(1);
    while (const ConstantExpr *CE = dyn_cast<ConstantExpr>(op)) {
      if (!CE->isCast())
        break;
      // Look through the bitcast
      op = cast<ConstantExpr>(op)->getOperand(0);
    }
    Function* fn = cast<Function>(op);
    assert(fn->getType() == awaitSuspendFnPtrTy && "unexpected await_suspend fn type");
    auto vFrame = new BitCastInst(Frame, bytePtrTy, "", I);
    auto call = CallInst::Create(fn, { I->getArgOperand(0), vFrame }, "", I);
    InlineFunctionInfo IFI;
    InlineFunction(call, IFI);
  }

  void ReplaceSuspendCFG(Suspend &sp) {
    auto const Index = &sp - Suspends.begin();
    BasicBlock* SuspendBlock = sp.SuspendInst->getParent();
    SuspendBlock->getTerminator()->eraseFromParent();

    Value *frame = nullptr;

    if (sp.SuspendInst->getParent() == CoroInit->getParent()) {
      frame = frameInRamp;
      BranchInst::Create(ReturnBlock, SuspendBlock);
    }
    else {
      frame = frameInResume;
      ReturnInst::Create(M->getContext(), SuspendBlock);
    }

    if (sp.OnResume == nullptr) {
      if (!SingleSuspend) {
        // will null out resumeFn
        auto gep = GetElementPtrInst::Create(frameTy, frame,
        { zeroConstant, zeroConstant }, "",
          sp.SuspendInst);
        new StoreInst(ConstantPointerNull::get(resumeFnPtrTy), gep,
          sp.SuspendInst);
      }
    }
    else {
      if (!SingleSuspend) {
        auto jumpIndex = ConstantInt::get(M->getContext(), APInt(32, Index));
        auto gep = GetElementPtrInst::Create(frameTy, frame,
        { zeroConstant, twoConstant }, "",
          sp.SuspendInst);
        new StoreInst(jumpIndex, gep, sp.SuspendInst);
      }
    }
  }

  void ReplaceSuspendsCFG() {
    for (Suspend& sp : Suspends) 
      ReplaceSuspendCFG(sp);
  }


  void ReplaceSuspends(Function* F, Value* Frame) {
    for (auto it = inst_begin(*F), end = inst_end(*F); it != end;) {
      Instruction& I = *it++;
      if (auto II = dyn_cast<IntrinsicInst>(&I)) {
        if (II->getIntrinsicID() == Intrinsic::coro_suspend) {
          CallAwaitSuspend(II, Frame);
          II->eraseFromParent();
        }
      }
    }
  }

  void ReplaceAllUsesOutsideTheRamp(Value *OldFrameValue,         
    Value *NewFrameValue) {
    for (auto UI = OldFrameValue->use_begin(), E = OldFrameValue->use_end();
         UI != E;) {
      Use &U = *UI++;

      if (auto I = dyn_cast<Instruction>(U.getUser()))
        if (RampBlocks.count(I->getParent()) == 1)
          continue;

      U.set(NewFrameValue);
    }
  }

  bool runOnCoroutine(Function &F) {
    init(F);

    //RemoveNoOptAttribute(F);
    //RemoveFakeSuspends(F);
    CreateAuxillaryFunctions();
    AnalyzeFunction(F);
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
INITIALIZE_PASS_BEGIN(
    CoroSplit2, "coro-split2",
    "Split coroutine into ramp/resume/destroy/cleanup functions v2", false,
    false)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(
    CoroSplit2, "coro-split2",
    "Split coroutine into ramp/resume/destroy/cleanup functions v2", false,
    false)

Pass *createCoroSplit2() { return new CoroSplit2(); }
}
