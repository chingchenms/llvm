//===-- CoroInstr.cpp - Coroutine Intrinsic Instruction Wrappers ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements all of the non-inline methods for the coroutine
// instruction wrappers.
//
//===----------------------------------------------------------------------===//

#include "CoroInstr.h"
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

CoroFrameInst* CoroFrameInst::Create(Instruction* InsertBefore) {
  auto M = InsertBefore->getModule();
  auto Fn = Intrinsic::getDeclaration(M, Intrinsic::experimental_coro_frame);
  auto Call = CallInst::Create(Fn, "", InsertBefore);
  return cast<CoroFrameInst>(Call);
}

CoroEndInst *llvm::CoroEndInst::Create(Instruction *InsertBefore, Value *Addr) {
  auto BB = InsertBefore->getParent();
  auto M = BB->getParent()->getParent();
  LLVMContext& C = M->getContext();
  auto BoolTy = Type::getInt1Ty(C);
  auto Fn = Intrinsic::getDeclaration(M, Intrinsic::coro_end);
  auto Call = CallInst::Create(Fn, { Addr, ConstantInt::get(BoolTy, 0) }, "",
    InsertBefore);
  return cast<CoroEndInst>(Call);
}

//CoroResumeAddrInst *CoroResumeAddrInst::Create(Value *FramePtr,
//                                               Instruction *InsertBefore) {
//  auto Fn = Intrinsic::getDeclaration(InsertBefore->getModule(), ID);
//  auto Call = CallInst::Create(Fn, {FramePtr}, "", InsertBefore);
//  return cast<CoroResumeAddrInst>(Call);
//}
