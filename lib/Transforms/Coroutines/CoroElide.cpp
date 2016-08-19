//===- CoroElide.cpp - Coroutine Frame Allocation Elision Pass ------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// This pass replaces dynamic allocation of coroutine frame with alloca and
// replaces calls to llvm.coro.resume and llvm.coro.destroy with direct calls
// to coroutine sub-functions.
//===----------------------------------------------------------------------===//

#include "CoroInternal.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Pass.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define DEBUG_TYPE "coro-elide"

namespace {
// Created on demand if CoroElide pass has work to do.
struct Lowerer : coro::LowererBase {
  SmallVector<CoroIdInst *, 4> CoroIds;
  SmallVector<CoroBeginInst *, 1> CoroBegins;
  SmallVector<CoroAllocInst *, 1> CoroAllocs;
  SmallVector<CoroSubFnInst *, 4> ResumeAddr;
  SmallVector<CoroSubFnInst *, 4> DestroyAddr;
  SmallVector<CoroFreeInst *, 1> CoroFrees;

  Lowerer(Module &M) : LowererBase(M) {}

  void elideHeapAllocations(Function *F, Type *FrameTy, AAResults &AA);
  bool processCoroId(CoroIdInst *, AAResults &AA);
};
} // end anonymous namespace

// Go through the list of coro.subfn.addr intrinsics and replace them with the
// provided constant.
static void replaceWithConstant(Constant *Value,
                                SmallVectorImpl<CoroSubFnInst *> &Users) {
  if (Users.empty())
    return;

  // See if we need to bitcast the constant to match the type of the intrinsic
  // being replaced. Note: All coro.subfn.addr intrinsics return the same type,
  // so we only need to examine the type of the first one in the list.
  Type *IntrTy = Users.front()->getType();
  Type *ValueTy = Value->getType();
  if (ValueTy != IntrTy) {
    // May need to tweak the function type to match the type expected at the
    // use site.
    assert(ValueTy->isPointerTy() && IntrTy->isPointerTy());
    Value = ConstantExpr::getBitCast(Value, IntrTy);
  }

  // Now the value type matches the type of the intrinsic. Replace them all!
  for (CoroSubFnInst *I : Users)
    replaceAndRecursivelySimplify(I, Value);
}

// See if any operand of the call instruction references the coroutine frame.
static bool operandReferences(CallInst *CI, AllocaInst *Frame, AAResults &AA) {
  for (Value *Op : CI->operand_values())
    if (AA.alias(Op, Frame) != NoAlias)
      return true;
  return false;
}

// Look for any tail calls referencing the coroutine frame and remove tail
// attribute from them, since now coroutine frame resides on the stack and tail
// call implies that the function does not references anything on the stack.
static void removeTailCallAttribute(AllocaInst *Frame, AAResults &AA) {
  Function &F = *Frame->getFunction();
  MemoryLocation Mem(Frame);
  for (Instruction &I : instructions(F))
    if (auto *Call = dyn_cast<CallInst>(&I))
      if (Call->isTailCall() && operandReferences(Call, Frame, AA)) {
        // FIXME: If we ever hit this check. Evaluate whether it is more
        // appropriate to retain musttail and allow the code to compile.
        if (Call->isMustTailCall())
          report_fatal_error("Call referring to the coroutine frame cannot be "
                             "marked as musttail");
        Call->setTailCall(false);
      }
}

// Given a resume function @f.resume(%f.frame* %frame), returns %f.frame type.
static Type *getFrameType(Function *Resume) {
  auto *ArgType = Resume->getArgumentList().front().getType();
  return cast<PointerType>(ArgType)->getElementType();
}

// Finds first non alloca instruction in the entry block of a function.
static Instruction *getFirstNonAllocaInTheEntryBlock(Function *F) {
  for (Instruction &I : F->getEntryBlock())
    if (!isa<AllocaInst>(&I))
      return &I;
  llvm_unreachable("no terminator in the entry block");
}

// To elide heap allocations we need to suppress code blocks guarded by
// llvm.coro.alloc and llvm.coro.free instructions.
void Lowerer::elideHeapAllocations(Function *F, Type *FrameTy, AAResults &AA) {
  LLVMContext &C = FrameTy->getContext();
  auto *InsertPt =
      getFirstNonAllocaInTheEntryBlock(CoroIds.front()->getFunction());

  // Replacing llvm.coro.alloc with false will suppress dynamic
  // allocation as it is expected for the frontend to generate the code that
  // looks like:
  //   id = coro.id(...)
  //   mem = coro.alloc(id) ? malloc(coro.size()) : 0;
  //   coro.begin(id, mem)
  auto *False = ConstantInt::getFalse(C);
  for (auto *CA : CoroAllocs) {
    CA->replaceAllUsesWith(False);
    CA->eraseFromParent();
  }

  // To suppress deallocation code, we replace all llvm.coro.free intrinsics
  // associated with this coro.begin with null constant.
  auto *NullPtr = ConstantPointerNull::get(Type::getInt8PtrTy(C));
  for (auto *CF : CoroFrees) {
    CF->replaceAllUsesWith(NullPtr);
    CF->eraseFromParent();
  }

  // FIXME: Design how to transmit alignment information for every alloca that
  // is spilled into the coroutine frame and recreate the alignment information
  // here. Possibly we will need to do a mini SROA here and break the coroutine
  // frame into individual AllocaInst recreating the original alignment.
  auto *Frame = new AllocaInst(FrameTy, "", InsertPt);
  auto *FrameVoidPtr =
      new BitCastInst(Frame, Type::getInt8PtrTy(C), "vFrame", InsertPt);

  for (auto *CB : CoroBegins) {
    CB->replaceAllUsesWith(FrameVoidPtr);
    CB->eraseFromParent();
  }

  // Since now coroutine frame lives on the stack we need to make sure that
  // any tail call referencing it, must be made non-tail call.
  removeTailCallAttribute(Frame, AA);
}

bool Lowerer::processCoroId(CoroIdInst *CoroId, AAResults &AA) {
  CoroBegins.clear();
  CoroAllocs.clear();
  ResumeAddr.clear();
  DestroyAddr.clear();

  // Collect all coro.begin and coro.allocs associated with this coro.id.
  for (User *U : CoroId->users()) {
    if (auto *CB = dyn_cast<CoroBeginInst>(U))
      CoroBegins.push_back(CB);
    else if (auto *CA = dyn_cast<CoroAllocInst>(U))
      CoroAllocs.push_back(CA);
  }

  // Collect all coro.subfn.addrs associated with coro.begin.
  // Note, we only devirtualize the calls if their coro.subfn.addr refers to
  // coro.begin directly. If we run into cases where this check is too
  // conservative, we can consider relaxing the check.
  for (CoroBeginInst *CB : CoroBegins) {
    for (User *U : CB->users())
      if (auto *II = dyn_cast<CoroSubFnInst>(U))
        switch (II->getIndex()) {
        case CoroSubFnInst::ResumeIndex:
          ResumeAddr.push_back(II);
          break;
        case CoroSubFnInst::DestroyIndex:
          DestroyAddr.push_back(II);
          break;
        default:
          llvm_unreachable("unexpected coro.subfn.addr constant");
        }
  }

  // If we don't have anything to devirtualize, replace coro.alloc with 'true',
  // since we won't be able to heap elide anything.
  if (DestroyAddr.empty() && ResumeAddr.empty()) {
    if (CoroAllocs.empty() && CoroBegins.empty())
      return false;

    // If it is the coroutine itself, don't touch it.
    if (CoroId->getCoroutine() == CoroId->getFunction())
      return false;

    auto *True = ConstantInt::getTrue(Context);
    for (CoroAllocInst* CA : CoroAllocs) {
      CA->replaceAllUsesWith(True);
      CA->eraseFromParent();
    }

    for (CoroBeginInst* CB : CoroBegins) {
      CB->replaceAllUsesWith(CB->getMem());
      CB->eraseFromParent();
    }
    return true;
  }


  // PostSplit coro.id refers to an array of subfunctions in its Info
  // argument.
  ConstantArray *Resumers = CoroId->getInfo().Resumers;
  assert(Resumers && "PostSplit coro.id Info argument must refer to an array"
                     "of coroutine subfunctions");
  auto *ResumeAddrConstant =
      ConstantExpr::getExtractValue(Resumers, CoroSubFnInst::ResumeIndex);

  replaceWithConstant(ResumeAddrConstant, ResumeAddr);

  if (DestroyAddr.empty())
    return true;

  auto *DestroyAddrConstant =
      ConstantExpr::getExtractValue(Resumers, CoroSubFnInst::DestroyIndex);

  replaceWithConstant(DestroyAddrConstant, DestroyAddr);

  // If there is a coro.alloc that llvm.coro.id refers to, we have the ability
  // to suppress dynamic allocation.
  if (!CoroAllocs.empty()) {
    // FIXME: The check above is overly lax. It only checks for whether we have
    // an ability to elide heap allocations, not whether it is safe to do so.
    // We need to do something like:
    // If for every exit from the function where coro.begin is
    // live, there is a coro.free or coro.destroy dominating that exit block,
    // then it is safe to elide heap allocation, since the lifetime of coroutine
    // is fully enclosed in its caller.
    auto *FrameTy = getFrameType(cast<Function>(ResumeAddrConstant));
    elideHeapAllocations(CoroId->getFunction(), FrameTy, AA);
  }
  return true;
}

// See if there are any coro.subfn.addr instructions referring to coro.devirt
// trigger, if so, replace them with a direct call to devirt trigger function.
static bool replaceDevirtTrigger(Function &F) {
  SmallVector<CoroSubFnInst *, 1> DevirtAddr;
  for (auto &I : instructions(F))
    if (auto *SubFn = dyn_cast<CoroSubFnInst>(&I))
      if (SubFn->getIndex() == CoroSubFnInst::RestartTrigger)
        DevirtAddr.push_back(SubFn);

  if (DevirtAddr.empty())
    return false;

  Module &M = *F.getParent();
  Function *DevirtFn = M.getFunction(CORO_DEVIRT_TRIGGER_FN);
  assert(DevirtFn && "coro.devirt.fn not found");
  replaceWithConstant(DevirtFn, DevirtAddr);

  return true;
}

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

namespace {
struct CoroElide : FunctionPass {
  static char ID;
  CoroElide() : FunctionPass(ID) {}

  std::unique_ptr<Lowerer> L;

  bool doInitialization(Module &M) override {
    if (coro::declaresIntrinsics(M, {"llvm.coro.id"}))
      L = llvm::make_unique<Lowerer>(M);
    return false;
  }

  bool runOnFunction(Function &F) override {
    if (!L)
      return false;

    bool Changed = false;

    if (F.hasFnAttribute(CORO_PRESPLIT_ATTR))
      Changed = replaceDevirtTrigger(F);

    L->CoroIds.clear();
    L->CoroFrees.clear();

    // Collect all PostSplit coro.ids and all coro.free.
    for (auto &I : instructions(F))
      if (auto *CF = dyn_cast<CoroFreeInst>(&I))
        L->CoroFrees.push_back(CF);
      else if (auto *CII = dyn_cast<CoroIdInst>(&I))
        if (CII->getInfo().isPostSplit())
          L->CoroIds.push_back(CII);

    // If we did not find any coro.id, there is nothing to do.
    if (L->CoroIds.empty())
      return Changed;

    AAResults &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
    for (auto *CII : L->CoroIds)
      Changed |= L->processCoroId(CII, AA);

    return Changed;
  }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AAResultsWrapperPass>();
    AU.setPreservesCFG();
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
