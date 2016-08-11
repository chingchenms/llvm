//===- CoroElide.cpp - Coroutine Frame Allocation Elision Pass ------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements coro-elide pass that replaces dynamic allocation
// of coroutine frame with alloca and replaces calls to @llvm.coro.resume and
// @llvm.coro.destroy with direct calls to coroutine sub-functions
// see ./Coroutines.rst for details
//
//===----------------------------------------------------------------------===//

#include "CoroUtils.h"
#include "CoroInstr.h"
#include "llvm/Transforms/Coroutines.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
using namespace llvm;

#define DEBUG_TYPE "coro-elide"

namespace {

  // TODO: paste explanation
  struct CoroElide : FunctionPass {
    static char ID;
    CoroElide() : FunctionPass(ID) {}
    bool runOnFunction(Function &F) override;
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.addRequired<AAResultsWrapperPass>();
    }
  };

}

char CoroElide::ID = 0;
INITIALIZE_PASS_BEGIN(
    CoroElide, "coro-elide",
    "Coroutine frame allocation elision and indirect calls replacement", false,
    false)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_END(
    CoroElide, "coro-elide",
    "Coroutine frame allocation elision and indirect calls replacement", false,
    false)

Pass *llvm::createCoroElidePass() { return new CoroElide(); }

static void replaceWithConstant(Constant *Value,
                                SmallVectorImpl<CoroSubFnInst *> &Users) {
  if (Users.empty())
    return;

  // All intrinsics in Users list should have the same type
  auto IntrTy = Users.front()->getType();
  auto ValueTy = Value->getType();
  if (ValueTy != IntrTy) {
    assert(ValueTy->isPointerTy() && IntrTy->isPointerTy());
    Value = ConstantExpr::getBitCast(Value, IntrTy);
  }

  for (CoroSubFnInst *I : Users) {
    I->replaceAllUsesWith(Value);
    I->eraseFromParent();
  }

  // do constant propagation
  CoroUtils::constantFoldUsers(Value);
}

static bool operandReferences(CallInst* CI, AllocaInst* Frame, AAResults& AA) {
  for (Value *Op : CI->operand_values())
    if (AA.alias(Op, Frame) != NoAlias)
      return true;
  return false;
}

static void removeTailCalls(AllocaInst* Frame, AAResults& AA) {
  Function& F = *Frame->getFunction();
  for (Instruction& I : instructions(F))
    if (auto Call = dyn_cast<CallInst>(&I))
      if (Call->isTailCall() && operandReferences(Call, Frame, AA))
        Call->setTailCall(false);
}

// Given a resume function @f.resume(%f.frame* %frame),
// returns %f.frame type.
static Type* getFrameType(Function* Resume) {
  auto ArgType = Resume->getArgumentList().front().getType();
  return cast<PointerType>(ArgType)->getElementType();
}

// Finds first non alloca instruction in the entry block of a function.
static Instruction* getFirstNonAllocaInTheEntryBlock(Function* F) {
  for (Instruction& I : F->getEntryBlock())
    if (!isa<AllocaInst>(&I))
      return &I;
  llvm_unreachable("no terminator in the entry block");
}

static void elideHeapAllocations(CoroBeginInst *CoroBegin, Type *FrameTy,
                                 CoroAllocInst *AllocInst, AAResults &AA) {
  LLVMContext& C = CoroBegin->getContext();
  auto InsertPt = getFirstNonAllocaInTheEntryBlock(CoroBegin->getFunction());

  auto Frame = new AllocaInst(FrameTy, "", InsertPt);
  auto vFrame =
      new BitCastInst(Frame, Type::getInt8PtrTy(C), "vFrame", InsertPt);

  auto *False = ConstantInt::get(Type::getInt1Ty(C), 0);
  AllocInst->replaceAllUsesWith(False);
  AllocInst->eraseFromParent();

  CoroUtils::replaceCoroFree(CoroBegin, nullptr);
  CoroBegin->replaceAllUsesWith(vFrame);
  CoroBegin->eraseFromParent();

  // Since now coroutine frame lives on the stack we need to make sure that
  // any tail call referencing it, must be made non-tail call.
  removeTailCalls(Frame, AA);
}

static bool replaceIndirectCalls(CoroBeginInst *CoroBegin, AAResults& AA) {
  SmallVector<CoroSubFnInst*, 8> ResumeAddr;
  SmallVector<CoroSubFnInst*, 8> DestroyAddr;

  for (auto U : CoroBegin->users())
    if (auto II = dyn_cast<CoroSubFnInst>(U))
      switch (II->getIndex()) {
      case 0: ResumeAddr.push_back(II); break;
      case 1: DestroyAddr.push_back(II); break;
      default:
        llvm_unreachable("unexpected coro.subfn.addr constant");
      }

  if (ResumeAddr.empty() && DestroyAddr.empty())
    return false;

  ConstantArray* Resumers = CoroBegin->getInfo().Resumers;

  auto ResumeAddrConstant = ConstantFolder().CreateExtractValue(Resumers, 0);
  auto DestroyAddrConstant = ConstantFolder().CreateExtractValue(Resumers, 1);
  auto CleanupAddrConstant = ConstantFolder().CreateExtractValue(Resumers, 2);

  replaceWithConstant(ResumeAddrConstant, ResumeAddr);

  if (DestroyAddr.empty())
    return true;

  auto *AllocInst = CoroBegin->getAlloc();

  replaceWithConstant(AllocInst ? CleanupAddrConstant : DestroyAddrConstant,
    DestroyAddr);

  if (AllocInst) {
    auto FrameTy = getFrameType(cast<Function>(ResumeAddrConstant));
    elideHeapAllocations(CoroBegin, FrameTy, AllocInst, AA);
  }

  return true;
}

static bool replaceDevirtTrigger(Function& F) {
  SmallVector<CoroSubFnInst*, 1> DevirtAddr;
  for (auto& I : instructions(F))
    if (auto SubFn = dyn_cast<CoroSubFnInst>(&I))
      if (SubFn->getIndex() == -1)
        DevirtAddr.push_back(SubFn);

  if (DevirtAddr.empty())
    return false;

  Module& M = *F.getParent();
  Function* DevirtFn = M.getFunction(CORO_DEVIRT_TRIGGER_FN);
  assert(DevirtFn && "coro.devirt.fn not found");
  replaceWithConstant(DevirtFn, DevirtAddr);

  return true;
}

bool CoroElide::runOnFunction(Function &F) {
  bool changed = false;

  if (F.hasFnAttribute(CORO_ATTR_STR))
    changed = replaceDevirtTrigger(F);

  // Collect all coro inits
  SmallVector<CoroBeginInst*, 4> CoroBegins;
  for (auto& I : instructions(F))
    if (auto CB = dyn_cast<CoroBeginInst>(&I))
      if (CB->getInfo().postSplit())
        CoroBegins.push_back(CB);

  if (CoroBegins.empty())
    return changed;

  AAResults& AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
  for (auto CB : CoroBegins)
    changed |= replaceIndirectCalls(CB, AA);

  return changed;
}
