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
  auto BB = InsertBefore->getParent();
  auto M = BB->getParent()->getParent();
  auto Fn = Intrinsic::getDeclaration(M, Intrinsic::experimental_coro_frame);
  auto Call = CallInst::Create(Fn, "", InsertBefore);
  return cast<CoroFrameInst>(Call);
}

CoroEndInst * llvm::CoroEndInst::Create(Instruction * InsertBefore, Value * Addr)
{
  auto BB = InsertBefore->getParent();
  auto M = BB->getParent()->getParent();
  LLVMContext& C = M->getContext();
  auto BoolTy = Type::getInt1Ty(C);
  auto Fn = Intrinsic::getDeclaration(M, Intrinsic::coro_end);
  auto Call = CallInst::Create(Fn, { Addr, ConstantInt::get(BoolTy, 0) }, "",
    InsertBefore);
  return cast<CoroEndInst>(Call);
}

///////

Phase CoroMeta::getPhase() const {
  auto MD = getRawMeta();
  if (dyn_cast<MDString>(MD))
    return Phase::Fresh;

  auto N = dyn_cast<MDNode>(MD);
  assert(N && "invalid metadata on CoroInit instruction");

  // {!"tag", {elide-info}, {outline-info}}
  return StringSwitch<Phase>(cast<MDString>(N->getOperand(0))->getString())
    .Case(kCoroPreIPOTag, Phase::PreIPO)
    .Case(kCoroPreSplitTag, Phase::PreSplit)
    .Default(Phase::PostSplit);
}

void CoroMeta::setPhase(Phase NewPhase) {
  if (NewPhase == Phase::PreIPO) {
    assert(getPhase() == Phase::Fresh && "invalid phase transition");
    Function* Coroutine = Intrin->getParent()->getParent();
    LLVMContext& C = Intrin->getContext();
    auto Empty = MDString::get(C, "");
    Metadata *Args[4] = {MDString::get(C, kCoroPreIPOTag),
                         ValueAsMetadata::get(Coroutine), Empty, Empty};
    setMeta(MDNode::get(C, Args));
    return;
  }
  char const* Tag;
  switch (NewPhase) {
  case Phase::PreSplit: Tag = kCoroPreSplitTag; break;
  case Phase::PostSplit: Tag = kCoroPostSplitTag; break;
  default:
    llvm_unreachable("unexpected phase tag");
  }

  LLVMContext &C = Intrin->getContext();
  auto N = cast<MDNode>(getRawMeta());
  Metadata *Args[4] = { MDString::get(C, Tag), N->getOperand(1).get(),
    N->getOperand(2).get(), N->getOperand(3).get() };
  setMeta(MDNode::get(C, Args));
}

MDNode::op_range CoroMeta::getParts() {
  auto N = cast<MDNode>(getRawMeta());
  auto P = cast<MDNode>(N->getOperand(3));
  return P->operands();
}

void CoroMeta::setParts(ArrayRef<Metadata *> MDs) {
  assert(getPhase() == Phase::PreIPO && "can only outline in preIPO phase");
  auto N = cast<MDNode>(getRawMeta());

  LLVMContext &C = Intrin->getContext();
  Metadata *Args[4] = { N->getOperand(0).get(), N->getOperand(1).get(),
    N->getOperand(2).get(), MDNode::get(C, MDs) };
  setMeta(MDNode::get(C, Args));
}

void CoroMeta::setMeta(Metadata *MD) {
  unsigned N = Intrin->getNumArgOperands();
  Intrin->setArgOperand(N-1, MetadataAsValue::get(Intrin->getContext(), MD));
}

Metadata *CoroMeta::getRawMeta() const {
  unsigned N = Intrin->getNumArgOperands();
  return cast<MetadataAsValue>(Intrin->getArgOperand(N-1))->getMetadata();
}





