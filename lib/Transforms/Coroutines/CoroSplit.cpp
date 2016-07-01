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

#include "CoroutineCommon.h"
#include <llvm/Transforms/Coroutines.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/LegacyPassManagers.h>
#include <llvm/IR/LegacyPassManager.h>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Analysis/CallGraphSCCPass.h>

using namespace llvm;

#define DEBUG_TYPE "coro-split"

BasicBlock* createResumeEntryBlock(Function& F, CoroutineShape& Shape) {
  LLVMContext& C = F.getContext();
  auto NewEntry = BasicBlock::Create(C, "resume.entry", &F);
  auto UnreachBB = BasicBlock::Create(C, "UnreachBB", &F);
  auto SuspendDestBB = Shape.CoroReturn.back()->getParent();
  assert(&SuspendDestBB->front() == Shape.CoroReturn.back());

  IRBuilder<> Builder(NewEntry);
//  auto vFramePtr = CoroFrameInst::Create(Builder);
  auto FramePtr = Shape.FramePtr;
  auto FrameTy = Shape.FrameTy;
  auto GepIndex =
      Builder.CreateConstInBoundsGEP2_32(FrameTy, FramePtr, 0, 2, "index.addr");
  auto Index =
    Builder.CreateLoad(GepIndex, "index");
  auto Switch =
      Builder.CreateSwitch(Index, UnreachBB, Shape.CoroSuspend.size());

  uint8_t SuspendIndex = -1;
  for (auto S: Shape.CoroSuspend) {
    ++SuspendIndex;
    ConstantInt* IndexVal = Builder.getInt8(SuspendIndex);

    // replace CoroSave with a store to Index
    auto Save = S->getCoroSave();
    Builder.SetInsertPoint(Save);
    auto GepIndex =
      Builder.CreateConstInBoundsGEP2_32(FrameTy, FramePtr, 0, 2, "index.addr");
    Builder.CreateStore(IndexVal, GepIndex);
    Save->replaceAllUsesWith(ConstantTokenNone::get(C));
    Save->eraseFromParent();

    // make sure that CoroSuspend is at the beginning of the block
    // and add a branch to it from a switch

    auto SuspendBB = S->getParent();
    Builder.SetInsertPoint(S);
    Builder.CreateBr(SuspendDestBB);
    auto ResumeBB = SuspendBB->splitBasicBlock(
        S, "resume." + Twine::utohexstr(SuspendIndex));
    SuspendBB->getTerminator()->eraseFromParent();

    Switch->addCase(IndexVal, ResumeBB);
  }

  Builder.SetInsertPoint(UnreachBB);
  Builder.CreateUnreachable();

return NewEntry;
}

static void replaceWith(ArrayRef<Instruction *> Instrs, Value *NewValue,
  ValueToValueMapTy *VMap = nullptr) {
  for (Instruction* I : Instrs) {
    if (VMap) I = cast<Instruction>((*VMap)[I]);
    I->replaceAllUsesWith(NewValue);
    I->eraseFromParent();
  }
}

template <typename T>
static void replaceWith(T &C, Value *NewValue,
  ValueToValueMapTy *VMap = nullptr) {
  Instruction* TestB = *C.begin();
  Instruction* TestE = *C.begin();
  TestB; TestE;
  Instruction** B = reinterpret_cast<Instruction**>(C.begin());
  Instruction** E = reinterpret_cast<Instruction**>(C.end());
  ArrayRef<Instruction*> Ar(B, E);
  replaceWith(Ar, NewValue, VMap);
}

void replaceCoroEnd(IntrinsicInst* End, ValueToValueMapTy& VMap) {
  auto NewE = cast<IntrinsicInst>(VMap[End]);
  LLVMContext& C = NewE->getContext();
  auto Ret = ReturnInst::Create(C, nullptr, NewE);
  auto BB = NewE->getParent();
  BB->splitBasicBlock(NewE);
  Ret->getNextNode()->eraseFromParent();
}
void replaceCoroEnd(ArrayRef<CoroEndInst*> Ends, ValueToValueMapTy& VMap) {
  for (auto E : Ends)
    replaceCoroEnd(E, VMap);
}

static Function *createClone(Function &F, Twine Suffix, CoroutineShape &Shape,
  BasicBlock *ResumeEntry, int8_t FnIndex) {

  Module* M = F.getParent();
  auto FrameTy = Shape.FrameTy;
  auto FnPtrTy = cast<PointerType>(FrameTy->getElementType(0));
  auto FnTy = cast<FunctionType>(FnPtrTy->getElementType());

  Function *NewF = Function::Create(
    FnTy, GlobalValue::LinkageTypes::InternalLinkage, F.getName() + Suffix, M);
  NewF->addAttribute(1, Attribute::NonNull);
  NewF->addAttribute(1, Attribute::NoAlias);

  SmallVector<ReturnInst*, 4> Returns;

  ValueToValueMapTy VMap;

  // replace all args with undefs
  for (Argument& A : F.getArgumentList())
    VMap[&A] = UndefValue::get(A.getType());

  CloneFunctionInto(NewF, &F, VMap, true, Returns);

  // remap frame pointer
  Argument* NewFramePtr = &NewF->getArgumentList().front();
  Value* OldFramePtr = cast<Value>(VMap[Shape.FramePtr]);
  NewFramePtr->takeName(OldFramePtr);
  OldFramePtr->replaceAllUsesWith(NewFramePtr);

  auto Entry = cast<BasicBlock>(VMap[ResumeEntry]);
  auto SpillBB = cast<BasicBlock>(VMap[Shape.AllocaSpillBlock]);
  SpillBB->moveBefore(&NewF->getEntryBlock());
  SpillBB->getTerminator()->eraseFromParent();
  BranchInst::Create(Entry, SpillBB);
  Entry = SpillBB;

  IRBuilder<> Builder(&Entry->front());

  // remap vFrame
  auto NewVFrame = Builder.CreateBitCast(
      NewFramePtr, Type::getInt8PtrTy(Builder.getContext()), "vFrame");
  Value* OldVFrame = cast<Value>(VMap[Shape.CoroBegin.back()]);
  OldVFrame->replaceAllUsesWith(NewVFrame);

  auto NewValue = Builder.getInt8(FnIndex);
  replaceWith(Shape.CoroSuspend, NewValue, &VMap);

  replaceCoroEnd(Shape.CoroReturn.back(), VMap);
  replaceCoroEnd(Shape.CoroEnd, VMap);

  Builder.SetInsertPoint(Shape.FramePtr->getNextNode());
  auto G = Builder.CreateConstInBoundsGEP2_32(Shape.FrameTy, Shape.FramePtr, 0,
    FnIndex, "fn.addr");
  Builder.CreateStore(NewF, G);
  NewF->setCallingConv(CallingConv::Fast);

  return NewF;
}

static void preSplitCleanup(Function& F) {
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
  llvm::legacy::FunctionPassManager FPM(F.getParent());

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
ArrayRef<Instruction*> toArrayRef(T const& Container) {
  using ElementType = typename T::value_type;
  Instruction* Test = (ElementType)0;
  Test;
  ArrayRef<ElementType> AR(Container);
  return reinterpret_cast<ArrayRef<Instruction*>&>(AR);
}

void replaceAndRemove(ArrayRef<Instruction*> Instructions, Value* NewValue) {
  for (Instruction* I : Instructions) {
    I->replaceAllUsesWith(NewValue);
    I->eraseFromParent();
  }
}

static void replaceFrameSize(CoroutineShape& Shape) {
  if (Shape.CoroSize.empty())
    return;

  auto SizeIntrin = Shape.CoroSize.back();
  Module* M = SizeIntrin->getModule();
  const DataLayout &DL = M->getDataLayout();
  auto Size = DL.getTypeAllocSize(Shape.FrameTy);
  auto SizeConstant = ConstantInt::get(SizeIntrin->getType(), Size);

  replaceAndRemove(toArrayRef(Shape.CoroSize), SizeConstant);
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
  Shape.CoroBegin.back()->setInfo(BC);
}

static void splitCoroutine(Function &F, CallGraph &CG, CallGraphSCC &SCC) {
  preSplitCleanup(F);

  CoroutineShape Shape(F);

  buildCoroutineFrame(F, Shape);
  replaceFrameSize(Shape);
  replaceAndRemove(toArrayRef(Shape.CoroFrame), Shape.CoroBegin.back());

  auto ResumeEntry = createResumeEntryBlock(F, Shape);
  auto ResumeClone = createClone(F, ".Resume", Shape, ResumeEntry, 0);
  auto DestroyClone = createClone(F, ".Destroy", Shape, ResumeEntry, 1);

  //  replaceWith(Shape.CoroSuspend, Builder.getInt8(-1));

  postSplitCleanup(F);
  postSplitCleanup(*ResumeClone);
  postSplitCleanup(*DestroyClone);

  // TODO: create Cleanup

  updateCoroInfo(F, Shape, { ResumeClone, DestroyClone });

  CoroCommon::updateCallGraph(F, { ResumeClone, DestroyClone }, CG, SCC);
}

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
      F.removeFnAttr(Attribute::Coroutine); // now coroutine is a normal func
      return false; // restart not needed
    }
  }
  llvm_unreachable("Coroutine without defininig coro.begin");
}

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
    false);
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
