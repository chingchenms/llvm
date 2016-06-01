//===- CoroutineCommon.cpp - utilities for coroutine passes ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file provides implementation of common utilities used to implement
/// coroutine passes.
///
//===----------------------------------------------------------------------===//

#include "CoroutineCommon.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Coroutines.h"

#include "llvm/IR/Module.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/InstIterator.h"

using namespace llvm;

void CoroutineCommon::PerModuleInit(Module &M) {
  this->M = &M;
  bytePtrTy = PointerType::get(IntegerType::get(M.getContext(), 8), 0);
  voidTy = Type::getVoidTy(M.getContext());
  int32Ty = IntegerType::get(M.getContext(), 32);
  boolTy = IntegerType::get(M.getContext(), 1);
  zeroConstant = ConstantInt::get(M.getContext(), APInt(32, 0));
  oneConstant = ConstantInt::get(M.getContext(), APInt(32, 1));
  twoConstant = ConstantInt::get(M.getContext(), APInt(32, 2));
  auto anyResumeFnTy = FunctionType::get(voidTy, bytePtrTy, /*isVarArg=*/false);
  auto awaitSuspendFnTy =
      FunctionType::get(voidTy, {bytePtrTy, bytePtrTy}, /*isVarArg=*/false);

  anyResumeFnPtrTy = PointerType::get(anyResumeFnTy, 0);
  awaitSuspendFnPtrTy = PointerType::get(awaitSuspendFnTy, 0);

  anyFrameTy = StructType::create({anyResumeFnPtrTy, anyResumeFnPtrTy, int32Ty, int32Ty},
                                  "any.frame");
  anyFramePtrTy = PointerType::get(anyFrameTy, 0);
}

IntrinsicInst *CoroutineCommon::FindIntrinsic(BasicBlock &B,
                                              Intrinsic::ID intrinID) {
  for (Instruction &I : B)
    if (IntrinsicInst *intrin = dyn_cast<IntrinsicInst>(&I))
      if (intrin->getIntrinsicID() == intrinID)
        return intrin;

  return nullptr;
}

IntrinsicInst *CoroutineCommon::FindIntrinsic(Function &F,
                                              Intrinsic::ID intrinID) {
  for (BasicBlock &B : F)
    if (IntrinsicInst *intrin = FindIntrinsic(B, intrinID))
      return intrin;

  return nullptr;
}

IntrinsicInst *CoroutineCommon::GetCoroElide(IntrinsicInst *CoroInit) {
  auto PN = dyn_cast<PHINode>(CoroInit->getArgOperand(0));
  if (!PN)
    return nullptr;

  for (auto& Inc : PN->incoming_values())
    if (auto II = dyn_cast<IntrinsicInst>(Inc))
      if (II->getIntrinsicID() == Intrinsic::experimental_coro_elide)
        return II;

  llvm_unreachable("expecting one of the inputs to @llvm.coro.init to be from @llvm.coro.elide");
}

void CoroutineCommon::ComputeDefChainNotIn(Instruction *source,
  BlockSet const &Blocks,
  InstrSetVector &result) {
  if (result.count(source))
    return;

  SmallVector<Instruction *, 16> workList;
  workList.push_back(source);

  do {
    source = workList.pop_back_val();
    result.insert(source);

    for (Use &u : source->operands()) {
      Instruction *instr = dyn_cast<Instruction>(u.get());
      if (instr == nullptr)
        continue;

      if (Blocks.count(instr->getParent()) == 0)
        if (result.count(instr) == 0)
          workList.push_back(instr);
    }
  } while (!workList.empty());
}

void CoroutineCommon::ReplaceCoroPromise(IntrinsicInst *intrin, bool from) {

  Value *Operand = intrin->getArgOperand(0);
  auto PromisePtr = cast<PointerType>(
    from ? Operand->getType() : intrin->getFunctionType()->getReturnType());
  auto PromiseType = PromisePtr->getElementType();

  // TODO: move into Coroutine Common
  auto SampleStruct = StructType::create({ anyResumeFnPtrTy, anyResumeFnPtrTy,
    int32Ty, 
    //int32Ty, 
    PromiseType }, "");
  const DataLayout &DL = M->getDataLayout();
  const auto Offset = DL.getStructLayout(SampleStruct)->getElementOffset(3);

  Value* Replacement = nullptr;

  if (from) {
    auto Index = ConstantInt::get(int32Ty, -Offset);
    auto BCI = new BitCastInst(Operand, bytePtrTy, "", intrin);
    auto Gep = GetElementPtrInst::CreateInBounds(BCI, { Index }, "", intrin);
    Replacement = Gep;
  }
  else {
    auto Index = ConstantInt::get(int32Ty, Offset);
    auto Gep = GetElementPtrInst::CreateInBounds(Operand, { Index }, "", intrin);
    auto BCI = new BitCastInst(Gep, PromisePtr, "", intrin);
    Replacement = BCI;
  }

  intrin->replaceAllUsesWith(Replacement);
  intrin->eraseFromParent();
}

void CoroutineCommon::ComputeDefChain(Instruction *source,
                                      BlockSet const &Blocks,
                                      InstrSetVector &result) {
  if (result.count(source))
    return;

  SmallVector<Instruction *, 16> workList;
  workList.push_back(source);

  do {
    source = workList.pop_back_val();
    result.insert(source);

    for (Use &u : source->operands()) {
      Instruction *instr = dyn_cast<Instruction>(u.get());
      if (instr == nullptr)
        continue;

      if (Blocks.count(instr->getParent()))
        if (result.count(instr) == 0)
          workList.push_back(instr);
    }
  } while (!workList.empty());
}

void llvm::CoroutineCommon::MoveInReverseOrder(InstrSetVector const &Instrs,
                                               Instruction *InsertBefore) {
  for (auto it = Instrs.rbegin(), end = Instrs.rend(); it != end; ++it) {
    Instruction *I = *it;
    if (I != InsertBefore)
      I->moveBefore(InsertBefore);
  }
}

void CoroutineCommon::ReplaceIntrinsicWith(Function &func, Intrinsic::ID id, Value *framePtr) {
  for (auto it = inst_begin(func), end = inst_end(func); it != end;) {
    Instruction& I = *it++;
    if (IntrinsicInst* intrin = dyn_cast<IntrinsicInst>(&I))
      if (intrin->getIntrinsicID() == id) {
        intrin->replaceAllUsesWith(framePtr);
        intrin->eraseFromParent();
      }
  }
}

void CoroutineCommon::ReplaceCoroDone(IntrinsicInst *intrin) {
  Value *rawFrame = intrin->getArgOperand(0);

  // this could be a coroutine start marker
  // it that is the case, keep it
  if (dyn_cast<ConstantPointerNull>(rawFrame))
    return;

  auto frame = new BitCastInst(rawFrame, anyFramePtrTy, "", intrin);
#if 0
  auto gepIndex = GetElementPtrInst::Create(
    anyFrameTy, frame, { zeroConstant, zeroConstant }, "", intrin);
  auto index = new LoadInst(gepIndex, "", intrin); // FIXME: alignment
  auto cmp = new ICmpInst(intrin, ICmpInst::ICMP_EQ,
    ConstantPointerNull::get(anyResumeFnPtrTy), index);
#else
  auto gepIndex = GetElementPtrInst::Create(
    anyFrameTy, frame, { zeroConstant, twoConstant }, "", intrin);
  auto index = new LoadInst(gepIndex, "", intrin); // FIXME: alignment
  auto cmp = new ICmpInst(intrin, ICmpInst::ICMP_EQ,
    ConstantInt::get(int32Ty, 0), index);
#endif
  intrin->replaceAllUsesWith(cmp);
  intrin->eraseFromParent();
}


void CoroutineCommon::ReplaceWithIndirectCall(IntrinsicInst *intrin,
                                              ConstantInt *index, bool Erase) {
  Value *rawFrame = intrin->getArgOperand(0);
  auto frame = new BitCastInst(rawFrame, anyFramePtrTy, "", intrin);
  auto gepIndex = GetElementPtrInst::Create(anyFrameTy, frame,
                                            {zeroConstant, index}, "", intrin);
  auto fnAddr = new LoadInst(gepIndex, "", intrin); // FIXME: alignment
  auto call = CallInst::Create(fnAddr, rawFrame, "", intrin);
  call->setCallingConv(CallingConv::Fast);
  intrin->replaceAllUsesWith(call);
  if (Erase) {
    intrin->eraseFromParent();
  }
}

bool llvm::CoroutineCommon::simplifyAndConstantFoldTerminators(Function & F) {
  int maxRepeat = 3;
  bool repeat;
  bool changed = false;
  do {
    repeat = false;
    for (auto& BB : F) changed |= SimplifyInstructionsInBlock(&BB);
    for (auto& BB : F) repeat |= ConstantFoldTerminator(&BB);
    changed |= repeat;
  } while (repeat && --maxRepeat > 0);
  return changed;
}


void llvm::initializeCoroutines(PassRegistry &registry) {
  initializeCoroHeapElidePass(registry);
  initializeCoroCleanupPass(registry);
  initializeCoroInlinePass(registry);
}
