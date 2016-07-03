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

//CoroFrameInst * llvm::CoroFrameInst::Create(IRBuilder<>& Builder)
//{
//  Module* M = Builder.GetInsertBlock()->getModule();
//  auto Fn = Intrinsic::getDeclaration(M, Intrinsic::experimental_coro_frame);
//  auto Call = Builder.CreateCall(Fn);
//  return cast<CoroFrameInst>(Call);
//}

