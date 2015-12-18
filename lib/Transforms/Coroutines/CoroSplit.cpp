//===- Hello.cpp - Example code from "Writing an LLVM Pass" ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements two versions of the LLVM "Hello World" pass described
// in docs/WritingAnLLVMPass.html
// see ./Coroutines.rst for details
//
//===----------------------------------------------------------------------===//

#include "CoroutineCommon.h"
#include "llvm/Transforms/Coroutines.h"

#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRPrintingPasses.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/SetVector.h"

#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"

#define DEBUG_TYPE "coro-split"
STATISTIC(CoroutineCounter, "Number of coroutines processed");

using namespace llvm;

namespace {

struct CoroutineProcessor : CoroutineCommon {
  // per module data

  CoroutineProcessor(Module &M) {
    CoroutineCommon::PerModuleInit(M);
  }

  // per function data

  Function *F = nullptr;
  SmallVector<SuspendPoint, 16> Suspends;

  SmallPtrSet<AllocaInst *, 16> SharedAllocas;

  IntrinsicInst* FinalSuspend;
  unsigned FinalSuspendIndex; // only valid if FinalSuspend is not null
  IntrinsicInst *InitialSuspend; // FIXME: deal with more complicated cases than
                                 // a single inital suspend
  BasicBlock *ReturnBlock;
  BasicBlock *DeleteBlock;

//  SmallVector<llvm::Type *, 8> typeArray;
  SmallString<16> smallString;
  StructType *frameTy = nullptr;
  PointerType *framePtrTy = nullptr;
  FunctionType *resumeFnTy = nullptr;
  PointerType *resumeFnPtrTy = nullptr;

  SmallPtrSet<BasicBlock *, 16> RampBlocks;
  SmallPtrSet<BasicBlock *, 16> CleanupBlocks;

  Function *resumeFn = nullptr;
  Function *destroyFn = nullptr;
  Function *cleanupFn = nullptr;

  Value *frameInDestroy;
  Value *frameInCleanup;
  Value *frameInResume;
  Instruction *frameInRamp;

  Value *vFrameInDestroy;
  Value *vFrameInCleanup;
  Value *vFrameInResume;
  Instruction *vFrameInRamp;

  Function* CreateAuxillaryFunction(Twine suffix, Value* & frame, Value* & vFrame) {
    auto func = Function::Create(resumeFnTy, GlobalValue::InternalLinkage,
      F->getName() + suffix, M);
    func->setCallingConv(CallingConv::Fast);
    frame = &*func->arg_begin();
    frame->setName("frame.ptr");

    auto entry = BasicBlock::Create(M->getContext(), "entry", func);
    vFrame = new BitCastInst(frame, bytePtrTy, "frame.void.ptr", entry);
    return func;
  }

  void CreateAuxillaryFunctions() {
    resumeFn = CreateAuxillaryFunction(".resume", frameInResume, vFrameInResume);
    cleanupFn = CreateAuxillaryFunction(".cleanup", frameInCleanup, vFrameInCleanup);
    destroyFn = CreateAuxillaryFunction(".destroy", frameInDestroy, vFrameInDestroy);

    auto CoroInit = FindIntrinsic(*F, Intrinsic::coro_init);
    assert(CoroInit && "Call to @llvm.coro.init is missing"); 
    vFrameInRamp = CoroInit;
    frameInRamp = new BitCastInst(vFrameInRamp, framePtrTy, "frame",
                                  CoroInit->getNextNode());
  }

  void CallAwaitSuspend(IntrinsicInst *I, Value *vFrame) {
    Value *op = I->getArgOperand(1);
    while (const ConstantExpr *CE = dyn_cast<ConstantExpr>(op)) {
      if (!CE->isCast())
        break;
      // Look through the bitcast
      op = cast<ConstantExpr>(op)->getOperand(0);
    }
    Function* fn = cast<Function>(op);
    assert(fn->getType() == awaitSuspendFnPtrTy && "unexpected await_suspend fn type");

    CallInst::Create(fn, {I->getArgOperand(0), vFrame}, "", I);
  }

  void ReplaceSuspend(SuspendPoint &sp) {
    auto const Index = &sp - Suspends.begin();

    auto vFrame = vFrameInResume;
    auto frame = frameInResume;
    if (sp.SuspendInst == InitialSuspend) {
      vFrame = vFrameInRamp;
      frame = frameInRamp;
    }

    if (sp.SuspendInst == FinalSuspend) {
      // will null out resumeFn
      auto gep = GetElementPtrInst::Create(frameTy, frame,
                                           {zeroConstant, zeroConstant}, "",
                                           sp.SuspendInst);
      new StoreInst(ConstantPointerNull::get(resumeFnPtrTy), gep,
                    sp.SuspendInst);
    } else {
      auto jumpIndex = ConstantInt::get(M->getContext(), APInt(32, Index));
      auto gep = GetElementPtrInst::Create(frameTy, frame,
                                           {zeroConstant, twoConstant}, "",
                                           sp.SuspendInst);
      new StoreInst(jumpIndex, gep, sp.SuspendInst);
    }

    CallAwaitSuspend(sp.SuspendInst, vFrame);

    BasicBlock* thisBlock = sp.SuspendInst->getParent();
    TerminatorInst* BR = thisBlock->getTerminator();

    if (sp.SuspendInst == InitialSuspend) {
      BranchInst::Create(ReturnBlock, thisBlock);
    }
    else {
      ReturnInst::Create(M->getContext(), thisBlock);
    }
    BR->eraseFromParent();
    sp.SuspendInst->eraseFromParent();
  }

  void ReplaceSuspends() {
    for (SuspendPoint& sp : Suspends)
      ReplaceSuspend(sp);
  }

  void init(Function &F) {
    this->F = &F;
    smallString.clear();
    frameTy = StructType::create(
        M->getContext(), (F.getName() + ".frame").toStringRef(smallString));
    framePtrTy = PointerType::get(frameTy, 0);
    resumeFnTy = FunctionType::get(voidTy, framePtrTy, false);
    resumeFnPtrTy = PointerType::get(resumeFnTy, 0);
  }

  bool notInRamp(BasicBlock& B) const {
    return (RampBlocks.count(&B) == 0);
  }

  bool notInRamp(AllocaInst &AI) const {
    for (User *U : AI.users()) {
      Instruction *Instr = cast<Instruction>(U);
      if (notInRamp(*Instr->getParent()))
        return true;
    }
    return false;
  }

  void FindUsefulInstructions(BasicBlock &ReturnBlock,
                              IntrinsicInst *firstSuspend) {
    SharedAllocas.clear();
    FinalSuspend = nullptr;
    InitialSuspend = firstSuspend;
    Suspends.clear();

    for (Instruction &I : instructions(F)) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
        if (notInRamp(*AI) || AI->getName() == "__promise")
          SharedAllocas.insert(AI);
      } else if (IntrinsicInst *intrin = dyn_cast<IntrinsicInst>(&I)) {
        switch (intrin->getIntrinsicID()) {
        default:
          break;
        case Intrinsic::coro_suspend:
          Suspends.emplace_back(intrin);
          if (Suspends.back().IfTrue == &ReturnBlock) {
            FinalSuspend = intrin;
            FinalSuspendIndex = Suspends.size() - 1;
          }
          break;
        }
      }
    }
  }

  Value* getFramePtr(BasicBlock &B) const {
    if (&B == DeleteBlock) return frameInDestroy;
    if (RampBlocks.count(&B) != 0) return frameInRamp;
    if (CleanupBlocks.count(&B) != 0) return frameInCleanup;
    return frameInResume;
  }

  Value* getFramePtr(User *user) const {
    auto I = cast<Instruction>(user);
    BasicBlock *B = I->getParent();
    return getFramePtr(*B);
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
        Value *frame = getFramePtr(user);
        auto gep =
            GetElementPtrInst::Create(frameTy, frame, {zeroConstant, index},
                                      smallString, cast<Instruction>(user));
        U.set(gep);
      }
      AI->eraseFromParent();
    }

    // we may end up replacing allocas with gep before frame is defined
    // move definition of frame to the beginning of function
    InstrSetVector coroFrameUses;
    ComputeDefChain(frameInRamp, RampBlocks, coroFrameUses);
    MoveInReverseOrder(coroFrameUses, &*inst_begin(F));
  }

  void CreateFrameStruct() {
    SmallVector<llvm::Type *, 8> typeArray;

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
    ReplaceIntrinsicWith(*F, Intrinsic::coro_size, ConstantInt::get(int32Ty, size));
  }

  void FindDeleteBlocks() {
    DeleteBlock = nullptr;
    for (BasicBlock& B : *F) {
      if (B.getSingleSuccessor() == ReturnBlock && notInRamp(B)) {
        DeleteBlock = &B;
        DeleteBlock->getTerminator()->eraseFromParent();
        new UnreachableInst(M->getContext(), DeleteBlock);
        return;
      }
    }
    llvm_unreachable("delete block not found");
  }

  void FindCleanupBlocks() {
    CleanupBlocks.clear();
    for (SuspendPoint& sp : Suspends) {
      ComputeAllSuccessors(sp.IfFalse, CleanupBlocks);
    }
    CleanupBlocks.erase(DeleteBlock);
    // make sure that none of the Ramp Blocks ended up here
    for (BasicBlock* B : RampBlocks)
      CleanupBlocks.erase(B);
  }

  BasicBlock* CreateUnreachableBlock(Function& F) {
    auto B = BasicBlock::Create(M->getContext(), "unreachable", &F);
    new UnreachableInst(M->getContext(), B);
    return B;
  }

  void PatchEdges(SmallPtrSet<BasicBlock *, 16> &scope, BasicBlock *oldTarget,
                  BasicBlock *newTarget) {
    for (BasicBlock* B : scope) {
      TerminatorInst* T = B->getTerminator();
      unsigned N = T->getNumSuccessors();
      for (unsigned i = 0; i < N; ++i)
        if (T->getSuccessor(i) == oldTarget)
          T->setSuccessor(i, newTarget);
    }
  }

  void SplitOutCleanupFunction() {
    BasicBlock* entry = &cleanupFn->getEntryBlock();
    BasicBlock* ret = BasicBlock::Create(M->getContext(), "ret", cleanupFn);
    ReturnInst::Create(M->getContext(), ret);

    DeleteBlock->replaceAllUsesWith(ret);

    // we may have a cleanup edge going to return blocks in the ramp
    // replace them with 'ret's in this function
    PatchEdges(CleanupBlocks, ReturnBlock, ret);
    for (BasicBlock *B : successors(ReturnBlock)) {
      PatchEdges(CleanupBlocks, B, ret);
    }

    Function::BasicBlockListType &oldBlocks = F->getBasicBlockList();
    Function::BasicBlockListType &newBlocks = cleanupFn->getBasicBlockList();

    for (BasicBlock* B : CleanupBlocks) {
      oldBlocks.remove(*B);
      newBlocks.push_back(B);
    }
    ReplaceIntrinsicWith(*cleanupFn, Intrinsic::coro_frame, vFrameInCleanup);

    // await_resume edge for final_suspend
    // points at a cleanup return block
    // inserting return block in the cleanup list allows SplitOutResumeFunction
    // to discover this and clean it up
    CleanupBlocks.insert(ret); 

    unsigned nonTrivialCleanups = 0;
    for (SuspendPoint& sp : Suspends) {
      if (sp.IfFalse == DeleteBlock)
        sp.IfFalse = ret;
      else
        ++nonTrivialCleanups;
    }
    if (nonTrivialCleanups == 0) {
      BranchInst::Create(ret, &cleanupFn->getEntryBlock());
      return;
    }

    if (FinalSuspend) {      
      auto gep = GetElementPtrInst::Create(
        frameTy, frameInCleanup, { zeroConstant, zeroConstant}, "", entry);
      auto resumeAddr = new LoadInst(gep, "", entry);
      auto isNull =
          new ICmpInst(*entry, CmpInst::Predicate::ICMP_EQ,
                       ConstantPointerNull::get(resumeFnPtrTy), resumeAddr);

      BasicBlock* swBlock = BasicBlock::Create(M->getContext(), "switch", cleanupFn);
      BranchInst::Create(Suspends[FinalSuspendIndex].IfFalse, swBlock, isNull, entry);
      entry = swBlock;
    }

    auto gepIndex = GetElementPtrInst::Create(
      frameTy, frameInCleanup, { zeroConstant, twoConstant }, "", entry);
    auto index = new LoadInst(gepIndex, "resume.index", entry);
    auto switchInst = SwitchInst::Create(index, ret, nonTrivialCleanups, entry);
    for (unsigned i = 0; i < Suspends.size(); ++i) {
      SuspendPoint& sp = Suspends[i];
      if (sp.IfFalse != ret) {
        switchInst->addCase(ConstantInt::get(M->getContext(), APInt(32, i)), sp.IfFalse);
      }
    }
  }

  void SplitOutDestroyFunction() {
    Function::BasicBlockListType &oldBlocks = F->getBasicBlockList();
    Function::BasicBlockListType &newBlocks = destroyFn->getBasicBlockList();

    oldBlocks.remove(*DeleteBlock);
    newBlocks.push_back(DeleteBlock);
    auto term = cast<UnreachableInst>(DeleteBlock->getTerminator());
    term->eraseFromParent();
    ReturnInst::Create(M->getContext(), DeleteBlock);

    auto entry = &destroyFn->getEntryBlock();
    auto call = CallInst::Create(cleanupFn, frameInDestroy, "", entry);
    call->setCallingConv(CallingConv::Fast);

    BranchInst::Create(DeleteBlock, entry);

    ReplaceIntrinsicWith(*destroyFn, Intrinsic::coro_frame, vFrameInDestroy);
  }

  void SplitOutResumeFunction() {
    Function::BasicBlockListType &oldBlocks = F->getBasicBlockList();
    Function::BasicBlockListType &newBlocks = resumeFn->getBasicBlockList();

    for (auto it = F->begin(), end = F->end(); it != end;) {
      BasicBlock& B = *it++;
      if (getFramePtr(B) == frameInResume) {
        oldBlocks.remove(B);
        newBlocks.push_back(&B);
        if (auto succ = B.getSingleSuccessor()) {
          if (getFramePtr(*succ) == frameInCleanup) {
            // insert call to destroy followed by return
            B.getTerminator()->eraseFromParent();
            auto call = CallInst::Create(destroyFn, frameInResume, "", &B);
            call->setCallingConv(CallingConv::Fast);
            ReturnInst::Create(M->getContext(), &B);
          }
        }
      }
    }

    auto caseCount = Suspends.size();
    if (FinalSuspend) {
      --caseCount;
    }
    // TODO: add special case for caseCount == 1
    BasicBlock* unreach = CreateUnreachableBlock(*resumeFn);

    auto entry = &resumeFn->getEntryBlock();

    auto gep = GetElementPtrInst::Create(
      frameTy, frameInResume, { zeroConstant, twoConstant }, "", entry);
    auto index = new LoadInst(gep, "resume.index", entry);
    auto switchInst = SwitchInst::Create(index, unreach, caseCount, entry);
    for (unsigned i = 0; i < Suspends.size(); ++i) {
      if (i != FinalSuspendIndex) {
        switchInst->addCase(ConstantInt::get(M->getContext(), APInt(32, i)), Suspends[i].IfTrue);
      }
    }
    ReplaceIntrinsicWith(*resumeFn, Intrinsic::coro_frame, vFrameInResume);
  }

  void runTransformations() {
    RemoveNoOptAttribute(*F);
    RemoveFakeSuspends(*F);
    CreateAuxillaryFunctions();
    RampBlocks.clear();

    IntrinsicInst *coroDone = FindIntrinsic(*F, Intrinsic::coro_done);
    assert(coroDone && "missing @llvm.coro.done intrinsic");
    assert(dyn_cast<ConstantPointerNull>(coroDone->getArgOperand(0)) &&
           "expecting null argument in @llvm.coro.done intrinsic");

    BranchSuccessors done = getSuccessors(coroDone);
    ReturnBlock = done.IfTrue;
    BasicBlock *StartBlock = done.IfFalse;
    IntrinsicInst *FirstSuspendIntr =
        FindIntrinsic(*StartBlock, Intrinsic::coro_suspend);

    if (FirstSuspendIntr) {
      assert(StartBlock->getSinglePredecessor() == coroDone->getParent() &&
        "FIXME: need to duplicate entry block. Not implemented yet");
    }
    else {
      auto BR = cast<BranchInst>(StartBlock->getTerminator());
      auto CoroSuspend = BR->getSuccessor(1);
      FirstSuspendIntr = FindIntrinsic(*CoroSuspend, Intrinsic::coro_suspend);
      assert(FirstSuspendIntr && "Missing first suspend instruction");
      RampBlocks.insert(CoroSuspend);

      auto invokeResume = BasicBlock::Create(M->getContext(), "invoke.resume", F);
      CallInst::Create(resumeFn, frameInRamp, "", invokeResume);
      BranchInst::Create(ReturnBlock, invokeResume);

      BR->setSuccessor(0, invokeResume);
      ComputeAllPredecessors(invokeResume, RampBlocks);
    }

    ComputeAllPredecessors(StartBlock, RampBlocks);
    ComputeAllSuccessors(ReturnBlock, RampBlocks);
    FindUsefulInstructions(*ReturnBlock, FirstSuspendIntr);
    CreateFrameStruct();

    FindDeleteBlocks();
    FindCleanupBlocks();
    ReplaceSharedUses();

    SplitOutCleanupFunction();
    SplitOutDestroyFunction();
    SplitOutResumeFunction();
    CleanupRampFunction(coroDone, StartBlock);

    ReplaceSuspends();

    // until coro-elide pass we need to make sure that
    // .cleanup function is not garbage collected
    InsertFakeSuspend(cleanupFn, &*inst_begin(F));
  }

  void CleanupRampFunction(IntrinsicInst* coroDone, BasicBlock* StartBlock) {
    // get rid of the coroDone and initialize resumeFn and destroyFn pointers
    BasicBlock *coroDoneBlock = coroDone->getParent();
    coroDoneBlock->getTerminator()->eraseFromParent();
    coroDone->eraseFromParent();
    auto insertionPoint = BranchInst::Create(StartBlock, coroDoneBlock);

    auto gep0 = GetElementPtrInst::Create(
      frameTy, frameInRamp, { zeroConstant, zeroConstant }, "", insertionPoint);
    new StoreInst(resumeFn, gep0, insertionPoint);

    auto gep1 = GetElementPtrInst::Create(
      frameTy, frameInRamp, { zeroConstant, oneConstant }, "", insertionPoint);
    new StoreInst(destroyFn, gep1, insertionPoint);
  }
};
// Hello - The first implementation, without getAnalysisUsage.
struct CoroSplit : public ModulePass {
  static char ID; // Pass identification, replacement for typeid
  CoroSplit() : ModulePass(ID) {}

  bool runOnModule(Module &M) override {
    CoroutineProcessor processor(M);
    bool changed = false;

    for (Function &F : M.getFunctionList()) {
      if (CoroutineCommon::FindIntrinsic(F, Intrinsic::coro_suspend)) {
        ++CoroutineCounter;
        processor.init(F);
        processor.runTransformations();
        changed = true;
      }
    }
    return changed;
  }
};
}

char CoroSplit::ID = 0;
INITIALIZE_PASS(CoroSplit, "coro-split",
  "Split coroutine into ramp/resume/destroy/cleanup functions", false, false)
Pass *llvm::createCoroSplitPass() { return new CoroSplit(); }
