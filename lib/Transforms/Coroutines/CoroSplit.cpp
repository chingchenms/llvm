//===- CoroSplit.cpp - Converts a coroutine into a state machine ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// This pass builds the coroutine frame and outlines resume and destroy parts
// of the coroutine into separate functions.
//===----------------------------------------------------------------------===//

#include "CoroInternal.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

using namespace llvm;

#define DEBUG_TYPE "coro-split"

// We present a coroutine to an LLVM as an ordinary function with suspension
// points marked up with intrinsics. We let the optimizer party on the coroutine
// as a single function for as long as possible. Shortly before the coroutine is
// eligible to be inlined into its callers, we split up the coroutine into parts
// corresponding to an initial, resume and destroy invocations of the coroutine,
// add them to the current SCC and restart the IPO pipeline to optimize the
// coroutine subfunctions we extracted before proceeding to the caller of the
// coroutine.

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
  for (Instruction *I : Instrs) {
    if (VMap)
      I = cast<Instruction>((*VMap)[I]);
    I->replaceAllUsesWith(NewValue);
    I->eraseFromParent();
  }
}

template <typename T>
static void replaceAndRemove(T const &C, Value *NewValue,
                             ValueToValueMapTy *VMap = nullptr) {
  replaceAndRemove(toArrayRef(C), NewValue, VMap);
}

static BasicBlock *createResumeEntryBlock(Function &F, coro::Shape &Shape) {
  LLVMContext &C = F.getContext();
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

  int SuspendIndex = Shape.CoroSuspends.front()->isFinal() ? -2 : -1;
  for (auto S : Shape.CoroSuspends) {
    ++SuspendIndex;
    ConstantInt *IndexVal = Builder.getInt8(SuspendIndex);

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
    } else {
      auto GepIndex = Builder.CreateConstInBoundsGEP2_32(FrameTy, FramePtr, 0,
                                                         2, "index.addr");
      Builder.CreateStore(IndexVal, GepIndex);
    }
    Save->replaceAllUsesWith(ConstantTokenNone::get(C));
    Save->eraseFromParent();

    // Split block before and after coro.suspend and add a jump from an entry
    // switch.
    auto SuspendBB = S->getParent();
    auto ResumeBB = SuspendBB->splitBasicBlock(
        S, SuspendIndex < 0 ? Twine("resume.final")
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
    } else if (auto CP = dyn_cast<CleanupPadInst>(FirstNonPhi)) {
      CleanupReturnInst::Create(CP, nullptr, NewE);
    } else {
      llvm_unreachable("coro.end not at the beginning of the EHpad");
    }
    // move coro-end and the rest of the instructions to a block that
    // will be now unreachable and remove the useless branch
    BB->splitBasicBlock(NewE);
    BB->getTerminator()->eraseFromParent();
  }
}

static void handleFinalSuspend(IRBuilder<> &Builder, Value *FramePtr,
                               coro::Shape &Shape, SwitchInst *Switch,
                               bool IsDestroy) {
  BasicBlock *ResumeBB = Switch->case_begin().getCaseSuccessor();
  Switch->removeCase(Switch->case_begin());
  if (IsDestroy) {
    BasicBlock *OldSwitchBB = Switch->getParent();
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
}

struct CreateCloneResult {
  Function *const Fn;
  Value *const VFrame;
};

static CreateCloneResult createClone(Function &F, Twine Suffix,
                                     coro::Shape &Shape,
                                     BasicBlock *ResumeEntry, int8_t FnIndex) {

  Module *M = F.getParent();
  auto FrameTy = Shape.FrameTy;
  auto FnPtrTy = cast<PointerType>(FrameTy->getElementType(0));
  auto FnTy = cast<FunctionType>(FnPtrTy->getElementType());

  Function *NewF =
      Function::Create(FnTy, GlobalValue::LinkageTypes::InternalLinkage,
                       F.getName() + Suffix, M);
  NewF->addAttribute(1, Attribute::NonNull);
  NewF->addAttribute(1, Attribute::NoAlias);

  ValueToValueMapTy VMap;
  // replace all args with undefs
  for (Argument &A : F.getArgumentList())
    VMap[&A] = UndefValue::get(A.getType());

  SmallVector<ReturnInst *, 4> Returns;

  CloneFunctionInto(NewF, &F, VMap, /*ModuleLevelChanges=*/true, Returns);

  // If we have debug info, update it. ModuleLevelChanges = true above, does
  // the heavy lifting, we just need to repoint subprogram at the same
  // DICompileUnit as the original function F.
  if (DISubprogram *SP = F.getSubprogram())
    NewF->getSubprogram()->replaceUnit(SP->getUnit());

  LLVMContext &C = NewF->getContext();

  // Remove old returns
  for (ReturnInst *Return : Returns) {
    new UnreachableInst(C, Return);
    Return->eraseFromParent();
  }

  // Remove old return attributes.
  NewF->removeAttributes(
      AttributeSet::ReturnIndex,
      AttributeSet::get(
          NewF->getContext(), AttributeSet::ReturnIndex,
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
  Argument *NewFramePtr = &NewF->getArgumentList().front();
  Value *OldFramePtr = cast<Value>(VMap[Shape.FramePtr]);
  NewFramePtr->takeName(OldFramePtr);
  OldFramePtr->replaceAllUsesWith(NewFramePtr);

  // Remap vFrame pointer.
  auto NewVFrame = Builder.CreateBitCast(
      NewFramePtr, Type::getInt8PtrTy(Builder.getContext()), "vFrame");
  Value *OldVFrame = cast<Value>(VMap[Shape.CoroBegin]);
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

static void removeCoroEnds(coro::Shape &Shape) {
  for (CoroEndInst *CE : Shape.CoroEnds)
    CE->eraseFromParent();
}

static void replaceFrameSize(coro::Shape &Shape) {
  if (Shape.CoroSizes.empty())
    return;

  // In the same function all coro.sizes should have the same result type.
  auto SizeIntrin = Shape.CoroSizes.back();
  Module *M = SizeIntrin->getModule();
  const DataLayout &DL = M->getDataLayout();
  auto Size = DL.getTypeAllocSize(Shape.FrameTy);
  auto SizeConstant = ConstantInt::get(SizeIntrin->getType(), Size);

  replaceAndRemove(Shape.CoroSizes, SizeConstant);
}

// Create a global constant array containing pointers to functions provided and
// set Info parameter of CoroBegin to point at this constant. Example:
//
//   @f.resumers = internal constant [2 x void(%f.frame*)*]
//                    [void(%f.frame*)* @f.resume, void(%f.frame*)* @f.destroy]
//   define void @f() {
//     ...
//     call i8* @llvm.coro.begin(i8* null, i32 0, i8* null,
//                    i8* bitcast([2 x void(%f.frame*)*] * @f.resumers to i8*))
//
// Assumes that all the functions have the same signature.
static void setCoroInfo(Function &F, CoroBeginInst *CoroBegin,
                        std::initializer_list<Function *> Fns) {

  SmallVector<Constant *, 4> Args(Fns.begin(), Fns.end());
  assert(Args.size() > 0);
  Function *Part = *Fns.begin();
  Module *M = Part->getParent();
  auto ArrTy = ArrayType::get(Part->getType(), Args.size());

  auto ConstVal = ConstantArray::get(ArrTy, Args);
  auto GV = new GlobalVariable(*M, ConstVal->getType(), /*isConstant=*/true,
                               GlobalVariable::PrivateLinkage, ConstVal,
                               F.getName() + Twine(".resumers"));

  // Update coro.begin instruction to refer to this constant
  LLVMContext &C = F.getContext();
  auto BC = ConstantExpr::getPointerCast(GV, Type::getInt8PtrTy(C));
  CoroBegin->getId()->setInfo(BC);
}

static void buildCoroutineFrame(Function &F, coro::Shape &Shape) {}

static void splitCoroutine(Function &F, CallGraph &CG, CallGraphSCC &SCC) {
  coro::Shape Shape(F);
  buildCoroutineFrame(F, Shape);

  auto *ResumeEntry = createResumeEntryBlock(F, Shape);
  auto ResumeClone = createClone(F, ".resume", Shape, ResumeEntry, 0);
  auto DestroyClone = createClone(F, ".destroy", Shape, ResumeEntry, 1);

  // we no longer need coro.end in F
  removeCoroEnds(Shape);

  replaceFrameSize(Shape);

  setCoroInfo(F, Shape.CoroBegin, {ResumeClone.Fn, DestroyClone.Fn});
  coro::updateCallGraph(F, {ResumeClone.Fn, DestroyClone.Fn}, CG, SCC);
}

// When we see the coroutine the first time, we insert an indirect call to a
// devirt trigger function and mark the coroutine that it is now ready for
// split.
static void prepareForSplit(Function &F, CallGraph &CG) {
  Module &M = *F.getParent();
#ifndef NDEBUG
  Function *DevirtFn = M.getFunction(CORO_DEVIRT_TRIGGER_FN);
  assert(DevirtFn && "coro.devirt.trigger function not found");
#endif

  F.addFnAttr(CORO_PRESPLIT_ATTR, PREPARED_FOR_SPLIT);

  // Insert an indirect call sequence that will be devirtualized by CoroElide
  // pass:
  //    %0 = call i8* @llvm.coro.subfn.addr(i8* null, i8 -1)
  //    %1 = bitcast i8* %0 to void(i8*)*
  //    call void %1(i8* null)
  coro::LowererBase Lowerer(M);
  Instruction *InsertPt = F.getEntryBlock().getTerminator();
  auto *Null = ConstantPointerNull::get(Type::getInt8PtrTy(F.getContext()));
  auto *DevirtFnAddr =
      Lowerer.makeSubFnCall(Null, CoroSubFnInst::RestartTrigger, InsertPt);
  auto *IndirectCall = CallInst::Create(DevirtFnAddr, Null, "", InsertPt);

  // Update CG graph with an indirect call we just added.
  CG[&F]->addCalledFunction(IndirectCall, CG.getCallsExternalNode());
}

// Make sure that there is a devirtualization trigger function that CoroSplit
// pass uses the force restart CGSCC pipeline. If devirt trigger function is not
// found, we will create one and add it to the current SCC.
static void createDevirtTriggerFunc(CallGraph &CG, CallGraphSCC &SCC) {
  Module &M = CG.getModule();
  if (M.getFunction(CORO_DEVIRT_TRIGGER_FN))
    return;

  LLVMContext &C = M.getContext();
  auto *FnTy = FunctionType::get(Type::getVoidTy(C), Type::getInt8PtrTy(C),
                                 /*IsVarArgs=*/false);
  Function *DevirtFn =
      Function::Create(FnTy, GlobalValue::LinkageTypes::PrivateLinkage,
                       CORO_DEVIRT_TRIGGER_FN, &M);
  DevirtFn->addFnAttr(Attribute::AlwaysInline);
  auto *Entry = BasicBlock::Create(C, "entry", DevirtFn);
  ReturnInst::Create(C, Entry);

  auto *Node = CG.getOrInsertFunction(DevirtFn);

  SmallVector<CallGraphNode *, 8> Nodes(SCC.begin(), SCC.end());
  Nodes.push_back(Node);
  SCC.initialize(Nodes);
}

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

namespace {

struct CoroSplit : public CallGraphSCCPass {
  static char ID; // Pass identification, replacement for typeid
  CoroSplit() : CallGraphSCCPass(ID) {}

  bool Run = false;

  // A coroutine is identified by the presence of coro.begin intrinsic, if
  // we don't have any, this pass has nothing to do.
  bool doInitialization(CallGraph &CG) override {
    Run = coro::declaresIntrinsics(CG.getModule(), {"llvm.coro.begin"});
    return CallGraphSCCPass::doInitialization(CG);
  }

  bool runOnSCC(CallGraphSCC &SCC) override {
    if (!Run)
      return false;

    // Find coroutines for processing.
    SmallVector<Function *, 4> Coroutines;
    for (CallGraphNode *CGN : SCC)
      if (auto *F = CGN->getFunction())
        if (F->hasFnAttribute(CORO_PRESPLIT_ATTR))
          Coroutines.push_back(F);

    if (Coroutines.empty())
      return false;

    CallGraph &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
    createDevirtTriggerFunc(CG, SCC);

    for (Function *F : Coroutines) {
      Attribute Attr = F->getFnAttribute(CORO_PRESPLIT_ATTR);
      StringRef Value = Attr.getValueAsString();
      DEBUG(dbgs() << "CoroSplit: Processing coroutine '" << F->getName()
                   << "' state: " << Value << "\n");
      if (Value == UNPREPARED_FOR_SPLIT) {
        prepareForSplit(*F, CG);
        continue;
      }
      F->removeFnAttr(CORO_PRESPLIT_ATTR);
      splitCoroutine(*F, CG, SCC);
    }
    return true;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    CallGraphSCCPass::getAnalysisUsage(AU);
  }
};
}

char CoroSplit::ID = 0;
INITIALIZE_PASS(
    CoroSplit, "coro-split",
    "Split coroutine into a set of functions driving its state machine", false,
    false)

Pass *llvm::createCoroSplitPass() { return new CoroSplit(); }
