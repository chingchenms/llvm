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
#include "llvm/Support/Debug.h"

#include <array>

using namespace llvm;

#define DEBUG_TYPE "coro-meta"

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

static MDTuple* valuesToMDTuple(ArrayRef<Value*> Values) {
  assert(!Values.empty() && "expect at least one value");
  LLVMContext& C = Values.front()->getContext();

  SmallVector<Metadata*, 16> MDs;
  MDs.reserve(Values.size());
  for (Value* V : Values)
    MDs.push_back(ValueAsMetadata::get(V));
  return MDNode::get(C, MDs);
}

CoroMeta::Setter::Setter(Type *T)
    : FieldNo(Field::Frame),
      NewValue(
          ValueAsMetadata::get(ConstantPointerNull::get(T->getPointerTo()))) {}

CoroMeta::Setter::Setter(Phase NewPhase, LLVMContext &C)
    : FieldNo(Field::Tag), NewValue(phaseToTag(C, NewPhase)) {}

CoroMeta::Setter::Setter(Field F, ArrayRef<Value *> Values)
    : FieldNo(F), NewValue(valuesToMDTuple(Values)) {}

CoroMeta::Setter::Setter(Field F, ArrayRef<Metadata *> Values, LLVMContext& C)
  : FieldNo(F), NewValue(MDNode::get(C, Values)) {}

// Updates fields of the metadata tuple.
// Default value Phase::Fresh is used to indicate that no updates
// to tag is requested. 

void CoroMeta::update(std::initializer_list<Setter> Updates) {
  auto MD = getRawMeta(Intrin);
  auto N = dyn_cast<MDNode>(MD);
  assert(N || isa<MDString>(MD) && "invalid coroutine metadata");

  std::array<Metadata*, Field::COUNT> Args;
  LLVMContext& C = Intrin->getContext();

  // If we already had a tuple, copy its operands to Args
  if (N) {
    unsigned Count = N->getNumOperands();
    assert(Count == Field::COUNT &&
      "unexpected number of elements in a coroutine metadata");
    for (unsigned I = 0; I < Count; ++I)
      Args[I] = N->getOperand(I);
  }
  else { 
    // otherwise, fill it with empty strings
    Args.fill(MDString::get(C, ""));
  }

  DEBUG(dbgs() << "--- metadata update for " << Intrin->getFunction()->getName()
               << "-----\n");
  // now go through all the pairs an update the fields
  for (auto const &U : Updates) {
    DEBUG(Args[U.FieldNo]->print(dbgs(), Intrin->getModule(), true));
    DEBUG(dbgs() << " => ");
    DEBUG(U.NewValue->print(dbgs(), Intrin->getModule(), true));
    DEBUG(dbgs() << "\n");

    Args[U.FieldNo] = U.NewValue;
  }
  DEBUG(dbgs() << "------------------------------\n");

  // TODO: add ctor for ArrayRef to take std::array
  ArrayRef<Metadata*> AR(&*Args.begin(), Args.size());
  auto UpdatedMD = MDNode::get(C, AR);
  setMeta(Intrin, UpdatedMD);
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
  update({{NewPhase, Intrin->getContext()}});
}

MDNode::op_range CoroMeta::getParts() {
  auto N = cast<MDNode>(getRawMeta(Intrin));
  auto P = cast<MDNode>(N->getOperand(3));
  return P->operands();
}

void CoroMeta::setParts(ArrayRef<Metadata *> MDs) {
  assert(getPhase() == Phase::PreIPO && "can only outline in preIPO phase");
  update({{Field::Parts, MDs, Intrin->getContext()}});
}





