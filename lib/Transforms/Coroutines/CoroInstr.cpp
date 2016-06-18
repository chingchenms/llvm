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

#include <array>

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

/////// Coroutine Metadata

#define kCoroPreIPOTag     "0 preIPO"
#define kCoroPreSplitTag   "1 preSplit"
#define kCoroPostSplitTag  "2 postSplit"

static void setMeta(IntrinsicInst* Intrin, Metadata *MD) {
  unsigned N = Intrin->getNumArgOperands();
  Intrin->setArgOperand(N - 1, MetadataAsValue::get(Intrin->getContext(), MD));
}

static Metadata *getRawMeta(IntrinsicInst* Intrin){
  unsigned N = Intrin->getNumArgOperands();
  return cast<MetadataAsValue>(Intrin->getArgOperand(N - 1))->getMetadata();
}

static MDString* phaseToTag(LLVMContext& C, Phase NewPhase) {
  const char* Tag = nullptr;
  switch (NewPhase) {
  case Phase::PreIPO: Tag = kCoroPreIPOTag; break;
  case Phase::PreSplit: Tag = kCoroPreSplitTag; break;
  case Phase::PostSplit: Tag = kCoroPostSplitTag; break;
  default:
    llvm_unreachable("unexpected phase tag");
  }
  return MDString::get(C, Tag);
}

void CoroMeta::updateFields(
    std::initializer_list<std::pair<Field, Value *>> Fs, Phase NewPhase) {

  auto MD = getRawMeta(Intrin);
  auto N = dyn_cast<MDNode>(MD);
  assert(N || isa<MDString>(MD) && "invalid coroutine metadata");

  std::array<Metadata*, Field::COUNT> Args;
  LLVMContext& C = Intrin->getContext();

  // we we had already a tuple, copy its operands
  if (N) {
    unsigned Count = N->getNumOperands();
    assert(Count == Field::COUNT &&
      "unexpected number of elements in a coroutine metadata");
    for (unsigned I = 0; I < Count; ++I)
      Args[I] = N->getOperand(I);
  }
  else { 
    // otherwise, update Tag and Coroutine fields
    // filling the rest with empty strings
    Args.fill(MDString::get(C, ""));
    Args[Field::Func] = ValueAsMetadata::get(Intrin->getFunction());
    NewPhase = Phase::PreIPO;
  }
  if (NewPhase != Phase::Fresh)
    Args[Field::Tag] = phaseToTag(C, Phase::PreIPO);

  // now go through all the pairs an update the fields
  for (auto const &P : Fs) {
    assert(P.first > Field::Tag &&
           "update tag field via dedicate NewPhase parameter");
    Args[P.first] = ValueAsMetadata::get(P.second);
  }

  // TODO: add ctor for ArrayRef to take std::array
  ArrayRef<Metadata*> AR(&*Args.begin(), Args.size());
  setMeta(Intrin, MDNode::get(C, AR));
}

Phase CoroMeta::getPhase() const {
  auto MD = getRawMeta(Intrin);
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
    Function* Coroutine = Intrin->getFunction();
    LLVMContext& C = Intrin->getContext();
    auto Empty = MDString::get(C, "");
    Metadata *Args[4] = {MDString::get(C, kCoroPreIPOTag),
                         ValueAsMetadata::get(Coroutine), Empty, Empty};
    setMeta(Intrin, MDNode::get(C, Args));
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
  auto N = cast<MDNode>(getRawMeta(Intrin));
  Metadata *Args[4] = { MDString::get(C, Tag), N->getOperand(1).get(),
    N->getOperand(2).get(), N->getOperand(3).get() };
  setMeta(Intrin, MDNode::get(C, Args));
}

MDNode::op_range CoroMeta::getParts() {
  auto N = cast<MDNode>(getRawMeta(Intrin));
  auto P = cast<MDNode>(N->getOperand(3));
  return P->operands();
}

void CoroMeta::setParts(ArrayRef<Metadata *> MDs) {
  assert(getPhase() == Phase::PreIPO && "can only outline in preIPO phase");
  auto N = cast<MDNode>(getRawMeta(Intrin));

  LLVMContext &C = Intrin->getContext();
  Metadata *Args[4] = { N->getOperand(0).get(), N->getOperand(1).get(),
    N->getOperand(2).get(), MDNode::get(C, MDs) };
  setMeta(Intrin, MDNode::get(C, Args));
}





