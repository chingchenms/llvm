//===- CoroSplit.cpp - Converts coroutine into a state machine-------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// TODO: explain what it is
//
//===----------------------------------------------------------------------===//

#include "CoroUtils.h"
#include <llvm/Transforms/Coroutines.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Analysis/CallGraphSCCPass.h>

using namespace llvm;

#define DEBUG_TYPE "coro-split"

static BasicBlock* createResumeEntryBlock(Function& F, CoroutineShape& Shape) {
  LLVMContext& C = F.getContext();
  auto NewEntry = BasicBlock::Create(C, "resume.entry", &F);
  auto UnreachBB = BasicBlock::Create(C, "UnreachBB", &F);

  IRBuilder<> Builder(NewEntry);
  auto FramePtr = Shape.FramePtr;
  auto FrameTy = Shape.FrameTy;
  auto GepIndex =
      Builder.CreateConstInBoundsGEP2_32(FrameTy, FramePtr, 0, 2, "index.addr");
  auto Index = Builder.CreateLoad(GepIndex, "index");
  auto Switch =
      Builder.CreateSwitch(Index, UnreachBB, Shape.CoroSuspends.size());
  Shape.ResumeSwitch = Switch;
 
#if CORO_USE_INDEX_FOR_DONE
  int SuspendIndex = -1;
#else
  int SuspendIndex = Shape.CoroSuspends.front()->isFinal() ? -2 : -1;
#endif
  for (auto S: Shape.CoroSuspends) {
    ++SuspendIndex;
    ConstantInt* IndexVal = Builder.getInt8(SuspendIndex);

    // replace CoroSave with a store to Index
    auto Save = S->getCoroSave();
    Builder.SetInsertPoint(Save);
    if (SuspendIndex == -1) {
      // final suspend point is represented by storing zero in ResumeFnAddr
      auto GepIndex = Builder.CreateConstInBoundsGEP2_32(FrameTy, FramePtr, 0,
        0, "ResumeFn.addr");
      auto NullPtr = ConstantPointerNull::get(cast<PointerType>(
          cast<PointerType>(GepIndex->getType())->getElementType()));
      Builder.CreateStore(NullPtr, GepIndex);
    }
    else {
      auto GepIndex = Builder.CreateConstInBoundsGEP2_32(FrameTy, FramePtr, 0,
                                                         2, "index.addr");
      Builder.CreateStore(IndexVal, GepIndex);
    }
    Save->replaceAllUsesWith(ConstantTokenNone::get(C));
    Save->eraseFromParent();

    // Split block before and after coro.suspend and add a jump from an entry
    // switch.
    auto SuspendBB = S->getParent();
    auto ResumeBB =
      SuspendBB->splitBasicBlock(S, SuspendIndex < 0
        ? Twine("resume.final")
        : "resume." + Twine(SuspendIndex));
    auto LandingBB = ResumeBB->splitBasicBlock(
        S->getNextNode(), SuspendBB->getName() + Twine(".landing"));
    Switch->addCase(IndexVal, ResumeBB);

    // Make SuspendBB byPass ResumeBB and jump straight to LandingBB.
    // Add a phi node, to provide -1 as the result of the coro.suspend we bypass
    // during suspend.
    cast<BranchInst>(SuspendBB->getTerminator())->setSuccessor(0, LandingBB);
    auto PN = PHINode::Create(Builder.getInt8Ty(), 2, "", &LandingBB->front());
    S->replaceAllUsesWith(PN);
    PN->addIncoming(Builder.getInt8(-1), SuspendBB);
    PN->addIncoming(S, ResumeBB);
  }

  Builder.SetInsertPoint(UnreachBB);
  Builder.CreateUnreachable();

  return NewEntry;
}

static void preSplitCleanup(Function& F) {
  removeUnreachableBlocks(F);
  llvm::legacy::FunctionPassManager FPM(F.getParent());

  FPM.add(createSCCPPass());
  FPM.add(createCFGSimplificationPass());
  FPM.add(createSROAPass());
  FPM.add(createEarlyCSEPass());

  FPM.doInitialization();
  FPM.run(F);
  FPM.doFinalization();
}

static void postSplitCleanup(Function& F) {
  removeUnreachableBlocks(F);
  llvm::legacy::FunctionPassManager FPM(F.getParent());

  FPM.add(createVerifierPass());
  FPM.add(createSCCPPass());
  FPM.add(createCFGSimplificationPass());
  //FPM.add(createSROAPass());
  //FPM.add(createEarlyCSEPass());
  //  FPM.add(createInstructionCombiningPass());
  //FPM.add(createCFGSimplificationPass());

  FPM.doInitialization();
  FPM.run(F);
  FPM.doFinalization();
}

template <typename T>
ArrayRef<Instruction *>
toArrayRef(T const &Container,
           typename std::enable_if<std::is_convertible<
               typename T::value_type, Instruction *>::value>::type * = 0) {
  ArrayRef<typename T::value_type> AR(Container);
  return reinterpret_cast<ArrayRef<Instruction *> &>(AR);
}

static void replaceAndRemove(ArrayRef<Instruction *> Instrs, Value *NewValue,
                             ValueToValueMapTy *VMap = nullptr) {
  for (Instruction* I : Instrs) {
    if (VMap) I = cast<Instruction>((*VMap)[I]);
    I->replaceAllUsesWith(NewValue);
    I->eraseFromParent();
  }
}

template <typename T>
static void replaceAndRemove(T const &C, Value *NewValue,
                             ValueToValueMapTy *VMap = nullptr) {
  replaceAndRemove(toArrayRef(C), NewValue, VMap);
}

static void replaceFinalCoroEnd(IntrinsicInst *End, ValueToValueMapTy &VMap) {
  auto NewE = cast<IntrinsicInst>(VMap[End]);
  ReturnInst::Create(NewE->getContext(), nullptr, NewE);

  // remove the rest of the block, by splitting it into an unreachable block
  auto BB = NewE->getParent();
  BB->splitBasicBlock(NewE);
  BB->getTerminator()->eraseFromParent();
}

static void replaceUnwindCoroEnds(ArrayRef<CoroEndInst *> Ends,
                                  ValueToValueMapTy &VMap) {
  for (auto E : Ends) {
    if (E->isFinal())
      continue;
    auto NewE = cast<CoroEndInst>(VMap[E]);
    auto BB = NewE->getParent();
    auto FirstNonPhi = BB->getFirstNonPHI();
    if (auto LP = dyn_cast<LandingPadInst>(FirstNonPhi)) {
      assert(LP->isCleanup());
      ResumeInst::Create(LP, NewE);
    }
    else if (auto CP = dyn_cast<CleanupPadInst>(FirstNonPhi)) {
      CleanupReturnInst::Create(CP, nullptr, NewE);
    }
    else {
      llvm_unreachable("coro.end not at the beginning of the EHpad");
    }
    // move coro-end and the rest of the instructions to a block that 
    // will be now unreachable and remove the useless branch
    BB->splitBasicBlock(NewE);
    BB->getTerminator()->eraseFromParent();
  }
}

static void handleFinalSuspend(IRBuilder<> &Builder, Value *FramePtr,
                               CoroutineShape &Shape, SwitchInst *Switch,
                               bool IsDestroy) {
#if CORO_USE_INDEX_FOR_DONE
  if (!IsDestroy)
    Switch->removeCase(Switch->case_begin());
#else
  BasicBlock* ResumeBB = Switch->case_begin().getCaseSuccessor();
  Switch->removeCase(Switch->case_begin());
  if (IsDestroy) {
    BasicBlock* OldSwitchBB = Switch->getParent();
    auto NewSwitchBB = OldSwitchBB->splitBasicBlock(Switch, "Switch");
    Builder.SetInsertPoint(OldSwitchBB->getTerminator());
    auto GepIndex = Builder.CreateConstInBoundsGEP2_32(Shape.FrameTy, FramePtr,
                                                       0, 0, "ResumeFn.addr");
    auto Load = Builder.CreateLoad(GepIndex);
    auto NullPtr = ConstantPointerNull::get(cast<PointerType>(Load->getType()));
    auto Cond = Builder.CreateICmpEQ(Load, NullPtr);
    Builder.CreateCondBr(Cond, ResumeBB, NewSwitchBB);
    OldSwitchBB->getTerminator()->eraseFromParent();
  }
#endif
}

struct CreateCloneResult {
  Function* const Fn;
  Value* const VFrame;
};

static CreateCloneResult createClone(Function &F, Twine Suffix,
                                     CoroutineShape &Shape,
                                     BasicBlock *ResumeEntry, int8_t FnIndex) {

  Module* M = F.getParent();
  auto FrameTy = Shape.FrameTy;
  auto FnPtrTy = cast<PointerType>(FrameTy->getElementType(0));
  auto FnTy = cast<FunctionType>(FnPtrTy->getElementType());

  Function *NewF = Function::Create(
    FnTy, GlobalValue::LinkageTypes::InternalLinkage, F.getName() + Suffix, M);
  NewF->addAttribute(1, Attribute::NonNull);
  NewF->addAttribute(1, Attribute::NoAlias);

  ValueToValueMapTy VMap;
  // replace all args with undefs
  for (Argument& A : F.getArgumentList())
    VMap[&A] = UndefValue::get(A.getType());

  SmallVector<ReturnInst*, 4> Returns;

  CloneFunctionInto(NewF, &F, VMap, true, Returns);
  NewF->getSubprogram()->replaceUnit(F.getSubprogram()->getUnit());

  LLVMContext& C = NewF->getContext();

  // Remove old returns
  for (ReturnInst* Return: Returns) {
    new UnreachableInst(C, Return);
    Return->eraseFromParent();
  }

  // Remove old return attributes.
  NewF->removeAttributes(
    AttributeSet::ReturnIndex,
    AttributeSet::get(NewF->getContext(), AttributeSet::ReturnIndex,
      AttributeFuncs::typeIncompatible(NewF->getReturnType())));

  // Make AllocaSpillBlock the new entry block
  auto SwitchBB = cast<BasicBlock>(VMap[ResumeEntry]);
  auto Entry = cast<BasicBlock>(VMap[Shape.AllocaSpillBlock]);
  Entry->moveBefore(&NewF->getEntryBlock());
  Entry->getTerminator()->eraseFromParent();
  BranchInst::Create(SwitchBB, Entry);
  Entry->setName("entry" + Suffix);

  // Clear all predecessors of the new entry block. 
  auto Switch = cast<SwitchInst>(VMap[Shape.ResumeSwitch]);
  Entry->replaceAllUsesWith(Switch->getDefaultDest());

  IRBuilder<> Builder(&NewF->getEntryBlock().front());

  // Remap frame pointer.
  Argument* NewFramePtr = &NewF->getArgumentList().front();
  Value* OldFramePtr = cast<Value>(VMap[Shape.FramePtr]);
  NewFramePtr->takeName(OldFramePtr);
  OldFramePtr->replaceAllUsesWith(NewFramePtr);

  // Remap vFrame pointer.
  auto NewVFrame = Builder.CreateBitCast(
    NewFramePtr, Type::getInt8PtrTy(Builder.getContext()), "vFrame");
  Value* OldVFrame = cast<Value>(VMap[Shape.CoroBegin]);
  OldVFrame->replaceAllUsesWith(NewVFrame);

  // In ResumeClone (FnIndex == 0), it is undefined behavior to resume from
  // final suspend point, thus, we remove its case from the switch.
  if (Shape.HasFinalSuspend) {
    auto Switch = cast<SwitchInst>(VMap[Shape.ResumeSwitch]);
    bool IsDestroy = FnIndex != 0;
    handleFinalSuspend(Builder, NewFramePtr, Shape, Switch, IsDestroy);
  }

  // Replace coro suspend with the appropriate resume index. 
  auto NewValue = Builder.getInt8(FnIndex);
  replaceAndRemove(Shape.CoroSuspends, NewValue, &VMap);

  // Remove coro.end intrinsics.
  replaceFinalCoroEnd(Shape.CoroEnds.front(), VMap);
  replaceUnwindCoroEnds(Shape.CoroEnds, VMap);

  // Store the address of this clone in the coroutine frame.
  Builder.SetInsertPoint(Shape.FramePtr->getNextNode());
  auto G = Builder.CreateConstInBoundsGEP2_32(Shape.FrameTy, Shape.FramePtr, 0,
    FnIndex, "fn.addr");
  Builder.CreateStore(NewF, G);
  NewF->setCallingConv(CallingConv::Fast);

  return {NewF, NewVFrame};
}

static Function *createCleanupClone(Function &F, Twine Suffix,
                                    CreateCloneResult const &Destroy) {
  ValueToValueMapTy VMap;
  Function *CleanupClone = CloneFunction(Destroy.Fn, VMap);
  CleanupClone->setName(F.getName() + Suffix);

  CoroCommon::replaceCoroFree(Destroy.VFrame, Destroy.VFrame);

  auto CleanupVFrame = cast<Value>(VMap[Destroy.VFrame]);
  CoroCommon::replaceCoroFree(CleanupVFrame, nullptr);
  return CleanupClone;
}

static void replaceFrameSize(Function* ResumeFn, CoroutineShape& Shape) {
  LLVMContext& C = ResumeFn->getContext();
  auto BC = ConstantFolder().CreateBitCast(ResumeFn, Type::getInt8PtrTy(C));
  for (auto CoroSize : Shape.CoroSizes)
    CoroSize->setArgOperand(0, BC);
#if 0
  auto SizeIntrin = Shape.CoroSizes.back();
  Module* M = SizeIntrin->getModule();
  const DataLayout &DL = M->getDataLayout();
  auto Size = DL.getTypeAllocSize(Shape.FrameTy);
  auto SizeConstant = ConstantInt::get(SizeIntrin->getType(), Size);

  replaceAndRemove(toArrayRef(Shape.CoroSizes), SizeConstant);
#endif
}

static void updateCoroInfo(Function& F, CoroutineShape &Shape,
                           std::initializer_list<Function *> Fns) {

  SmallVector<Constant*, 4> Args(Fns.begin(), Fns.end());
  assert(Args.size() > 0);
  Function* Part = *Fns.begin();
  Module* M = Part->getParent();
  auto ArrTy = ArrayType::get(Part->getType(), Args.size());

  auto ConstVal = ConstantArray::get(ArrTy, Args);
  auto GV = new GlobalVariable(*M, ConstVal->getType(), /*isConstant=*/true,
    GlobalVariable::PrivateLinkage, ConstVal,
    F.getName() + Twine(".parts"));

  // Update coro.begin instruction to refer to this constant
  LLVMContext &C = F.getContext();
  auto BC = ConstantFolder().CreateBitCast(GV, Type::getInt8PtrTy(C));
  Shape.CoroBegin->setInfo(BC);
}

static void handleNoSuspendCoroutine(CoroBeginInst *CoroBegin, Type *FrameTy) {
  auto AllocInst = CoroBegin->getAlloc();
  IRBuilder<> Builder(AllocInst);
  auto Frame = Builder.CreateAlloca(FrameTy);
  auto vFrame = Builder.CreateBitCast(Frame, AllocInst->getType());
  AllocInst->replaceAllUsesWith(vFrame);
  AllocInst->eraseFromParent();

  CoroCommon::replaceCoroFree(CoroBegin, nullptr);
  CoroBegin->replaceAllUsesWith(vFrame);
  CoroBegin->eraseFromParent();
}

// look for a very simple pattern
//    coro.save
//    no other calls
//    resume or destroy call
//    coro.suspend

static bool simplifySuspendPoint(CoroSuspendInst* Suspend) {
  auto Save = Suspend->getCoroSave();
  auto BB = Suspend->getParent();
  if (BB != Save->getParent())
    return false;

  CallSite SingleCallSite;

  // check that we have only one CallSite
  for (Instruction *I = Save->getNextNode(); I != Suspend;
    I = I->getNextNode()) {
    if (isa<CoroFrameInst>(I))
      continue;
    if (isa<CoroSubFnInst>(I))
      continue;
    if (CallSite CS = CallSite(I)) {
      if (SingleCallSite)
        return false;
      else
        SingleCallSite = CS;
    }
  }
  auto CallInstr = SingleCallSite.getInstruction();
  if (!CallInstr)
    return false;

  auto Callee = SingleCallSite.getCalledValue();

  if (isa<Function>(Callee))
    return false;

  Callee = Callee->stripPointerCasts();
  auto SubFn = dyn_cast<CoroSubFnInst>(Callee);
  if (!SubFn)
    return false;

  Suspend->replaceAllUsesWith(SubFn->getRawIndex());
  Suspend->eraseFromParent();
  Save->eraseFromParent();

  SubFn->replaceAllUsesWith(
      ConstantPointerNull::get(cast<PointerType>(SubFn->getType())));
  SubFn->eraseFromParent();

  CallInstr->eraseFromParent();

  return true;
}

static void simplifySuspendPoints(CoroutineShape& Shape) {
  auto& S = Shape.CoroSuspends;
  unsigned I = 0, N = S.size();
  if (N == 0)
    return;
  for (;;) {
    if (simplifySuspendPoint(S[I])) {
      if (--N == I)
        break;
      std::swap(S[I], S[N]);
      continue;
    }
    if (++I == N)
      break;
  }
  S.resize(N);
}

static void splitCoroutine(Function &F, CallGraph &CG, CallGraphSCC &SCC) {
  LowerDbgDeclare(F);
  CoroCommon::removeLifetimeIntrinsics(F);
  preSplitCleanup(F);

  // After split coroutine will be a normal function
  F.removeFnAttr(Attribute::Coroutine); 
  CoroutineShape Shape(F);

  simplifySuspendPoints(Shape);
  buildCoroutineFrame(F, Shape);

  // If there is no suspend points, no split required, just remove
  // the allocation and deallocation blocks, they are not needed
  if (Shape.CoroSuspends.empty()) {
    preSplitCleanup(F);
    handleNoSuspendCoroutine(Shape.CoroBegin, Shape.FrameTy);
    postSplitCleanup(F);
    CoroCommon::updateCallGraph(F, {}, CG, SCC);
    return;
  }

  auto ResumeEntry = createResumeEntryBlock(F, Shape);
  auto ResumeClone = createClone(F, ".resume", Shape, ResumeEntry, 0);
  auto DestroyClone = createClone(F, ".destroy", Shape, ResumeEntry, 1);

  // we no longer need coro.end in F
  for (CoroEndInst* CE : Shape.CoroEnds)
    CE->eraseFromParent();

  postSplitCleanup(F);
  postSplitCleanup(*ResumeClone.Fn);
  postSplitCleanup(*DestroyClone.Fn);

  auto CleanupClone =
      createCleanupClone(F, ".cleanup", DestroyClone);

  updateCoroInfo(F, Shape, { ResumeClone.Fn, DestroyClone.Fn, CleanupClone });
  replaceFrameSize(ResumeClone.Fn, Shape);
  CoroCommon::updateCallGraph(
      F, {ResumeClone.Fn, DestroyClone.Fn, CleanupClone}, CG, SCC);
}

#define kReadyForSplitStr "coro.ready.for.split"

static bool handleCoroutine(Function& F, CallGraph &CG, CallGraphSCC &SCC) {
  if (F.hasFnAttribute(kReadyForSplitStr)) {
    splitCoroutine(F, CG, SCC);
    return false; // no restart needed
  }

  F.addFnAttr(kReadyForSplitStr);
  return true;
}

#if 0
static bool handleCoroutine(Function& F, CallGraph &CG, CallGraphSCC &SCC) {
  for (auto& I : instructions(F)) {
    if (auto CB = dyn_cast<CoroBeginInst>(&I)) {
      auto Info = CB->getInfo();
      // this coro.begin belongs to inlined post-split coroutine we called
      if (Info.postSplit())
        continue;

      if (Info.needToOutline()) {
        outlineCoroutineParts(F, CG, SCC);
        return true; // restart needed
      }

      splitCoroutine(F, CG, SCC);
      return false; // restart not needed
    }
  }
  llvm_unreachable("Coroutine without defininig coro.begin");
}
#endif
//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

namespace {

  struct CoroSplit : public CallGraphSCCPass {
    static char ID; // Pass identification, replacement for typeid
    CoroSplit() : CallGraphSCCPass(ID) {}

    bool needToRestart;
    
    bool restartRequested() const override { return needToRestart; }

    bool runOnSCC(CallGraphSCC &SCC) override {
      CallGraph &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
      needToRestart = false;

      // find coroutines for processing
      SmallVector<Function*, 4> Coroutines;
      for (CallGraphNode *CGN : SCC)
        if (auto F = CGN->getFunction())
          if (F->hasFnAttribute(Attribute::Coroutine))
            Coroutines.push_back(F);

      if (Coroutines.empty())
        return false;

      for (Function* F : Coroutines)
        needToRestart |= handleCoroutine(*F, CG, SCC);

      return true;
    }

  };
}

char CoroSplit::ID = 0;
#if 1
INITIALIZE_PASS(
    CoroSplit, "coro-split",
    "Split coroutine into a set of functions driving its state machine", false,
    false)
#else
INITIALIZE_PASS_BEGIN(
    CoroSplit, "coro-split",
    "Split coroutine into a set of functions driving its state machine", false,
    false)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(
    CoroSplit, "coro-split",
    "Split coroutine into a set of functions driving its state machine", false,
    false)
#endif
Pass *llvm::createCoroSplitPass() { return new CoroSplit(); }
