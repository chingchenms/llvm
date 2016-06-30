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
  auto SuspendDestBB = Shape.CoroEndFinal.back()->getParent();
  assert(&SuspendDestBB->front() == Shape.CoroEndFinal.back());

  IRBuilder<> Builder(NewEntry);
  auto vFramePtr = CoroFrameInst::Create(Builder);
  auto FramePtr = Builder.CreateBitCast(vFramePtr, Shape.FramePtrTy);
  auto FrameTy = Shape.FramePtrTy->getElementType();
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

void replaceCoroEnd(ArrayRef<CoroEndInst*> Ends, ValueToValueMapTy& VMap) {
  for (auto E : Ends) {
    auto NewE = cast<CoroEndInst>(VMap[E]);
    LLVMContext& C = NewE->getContext();
    auto Ret = ReturnInst::Create(C, nullptr, NewE);
    auto BB = NewE->getParent();
    BB->splitBasicBlock(NewE);
    Ret->getNextNode()->eraseFromParent();
  }
}

static Function *createClone(Function &F, Twine Suffix, CoroutineShape &Shape,
  BasicBlock *ResumeEntry, int8_t index) {

  Module* M = F.getParent();
  auto FrameTy = cast<StructType>(Shape.FramePtrTy->getElementType());
  auto FnPtrTy = cast<PointerType>(FrameTy->getElementType(0));
  auto FnTy = cast<FunctionType>(FnPtrTy->getElementType());

  Function *NewF = Function::Create(
    FnTy, GlobalValue::LinkageTypes::InternalLinkage, F.getName() + Suffix, M);

  SmallVector<ReturnInst*, 4> Returns;

  ValueToValueMapTy VMap;
  // replace all args with undefs
  for (Argument& A : F.getArgumentList())
    VMap[&A] = UndefValue::get(A.getType());

  CloneFunctionInto(NewF, &F, VMap, true, Returns);


  auto Entry = cast<BasicBlock>(VMap[ResumeEntry]);
  Entry->moveBefore(&NewF->getEntryBlock());
  IRBuilder<> Builder(F.getContext());

  auto NewValue = Builder.getInt8(index);
  replaceWith(Shape.CoroSuspend, NewValue, &VMap);

  replaceCoroEnd(Shape.CoroEndFinal, VMap);
  replaceCoroEnd(Shape.CoroEndUnwind, VMap);

  return NewF;
}

static void SimplifyCFG(Function& F) {
  llvm::legacy::FunctionPassManager FPM(F.getParent());
  FPM.add(createCFGSimplificationPass());
  FPM.doInitialization();
  FPM.run(F);
  FPM.doFinalization();
}

static void splitCoroutine(Function &F, CoroBeginInst &CB) {
  CoroutineShape Shape(F);
  buildCoroutineFrame(F, Shape);
  auto ResumeEntry = createResumeEntryBlock(F, Shape);
  auto ResumeClone = createClone(F, ".Resume", Shape, ResumeEntry, 0);
  auto DestroyClone = createClone(F, ".Destroy", Shape, ResumeEntry, 1);

  SimplifyCFG(F);
  SimplifyCFG(*ResumeClone);
  SimplifyCFG(*DestroyClone);
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

      splitCoroutine(F, *CB);
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
