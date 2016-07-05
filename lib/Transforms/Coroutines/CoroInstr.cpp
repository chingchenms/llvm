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

CoroSubFnInst *llvm::CoroSubFnInst::Create(IRBuilder<> &Builder,
                                           Value *FramePtr, uint8_t Index) {
  Module* M = Builder.GetInsertBlock()->getModule();
  auto Fn = Intrinsic::getDeclaration(M, Intrinsic::coro_subfn_addr);
  SmallVector<Value*, 2> Args{ FramePtr, Builder.getInt8(Index) };
  auto Call = Builder.CreateCall(Fn, Args);
  return cast<CoroSubFnInst>(Call);
}

