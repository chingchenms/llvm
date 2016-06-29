//===--- CoroExtract.cpp - Pull code region into a new function -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the interface to tear out a code region, from a 
// coroutine to prevent code moving into the specialized coroutine regions that
// deal with allocation, deallocation and producing the return value.
//
//===----------------------------------------------------------------------===//

#include "CoroExtract.h"

#include <llvm/Transforms/Utils/Cloning.h>

#include <llvm/Transforms/Utils/CodeExtractor.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IRBuilder.h>

#define DEBUG_TYPE "coro-extract"

using namespace llvm;

struct RegionInfo {
  SmallPtrSet<BasicBlock*, 16> Blocks;
  SmallPtrSet<Value*, 4> Inputs;
  SmallPtrSet<Value*, 4> Outputs;
  BasicBlock* EntryBlock;

  void dump() {
    dbgs() << "inputs:\n";
    for (Value* V : Inputs)
      V->dump();
    dbgs() << "\noutputs:\n";
    for (Value* V : Outputs)
      V->dump();
    dbgs() << "\n";
  }
};

static RegionInfo findInputsOutputs(Function& F) {
  DominatorTree DT(F);
  RegionInfo Result;
  Result.EntryBlock = &F.getEntryBlock();

  for (BasicBlock& BB : F) {
    // not interested in blocks outside the region
    if (!DT.isReachableFromEntry(&BB))
      continue;

    Result.Blocks.insert(&BB);

    for (Instruction& I : BB) {
      // if any operand is defined outside the region, it is an input
      for (Use& U : I.operands()) {
        Value* User = U.get();
        if (isa<Argument>(User)) {
          Result.Inputs.insert(U.get());
          continue;
        }
        if (auto Instr = dyn_cast<Instruction>(U.get()))
          if (!DT.isReachableFromEntry(Instr->getParent()))
            Result.Inputs.insert(U.get());
      }

      // if there is a user outside the region, it should be an output
      for (User *U : I.users()) {
        BasicBlock* UserBB = cast<Instruction>(U)->getParent();
        if (!DT.isReachableFromEntry(UserBB)) {
          Result.Outputs.insert(&I);
          break;
        }
      }
    }
  }
  return Result;
}
void replaceAllUsesInside(Value *Old, Value *New,
  SmallPtrSetImpl<BasicBlock *> const &Blocks) {
  assert(New && "Value::replaceUsesOutsideBlock(<null>, BB) is invalid!");
  assert(New->getType() == Old->getType() &&
    "replaceUses of value with new value of different type!");

  Value::use_iterator UI = Old->use_begin(), E = Old->use_end();
  for (; UI != E;) {
    Use &U = *UI;
    ++UI;
    auto *Usr = dyn_cast<Instruction>(U.getUser());
    if (Usr && Blocks.count(Usr->getParent()) == 0)
      continue;
    U.set(New);
  }
}

void replaceAllUsesOutside(Value *Old, Value *New,
                           SmallPtrSetImpl<BasicBlock *> const &Blocks) {
  assert(New && "Value::replaceUsesOutsideBlock(<null>, BB) is invalid!");
  assert(New->getType() == Old->getType() &&
    "replaceUses of value with new value of different type!");

  Value::use_iterator UI = Old->use_begin(), E = Old->use_end();
  for (; UI != E;) {
    Use &U = *UI;
    ++UI;
    auto *Usr = dyn_cast<Instruction>(U.getUser());
    if (Usr && Blocks.count(Usr->getParent()) != 0)
      continue;
    U.set(New);
  }
}

// TODO: 
//   1) special case one ret value case
//   2) remove extra entry exit blocks

Function *llvm::CoroPartExtractor::createFunction(BasicBlock *Start,
  BasicBlock *End, Twine Suffix) {

  auto PreStart = Start->getSinglePredecessor();
  auto PreEnd = End->getSinglePredecessor();
  //if (!PreEnd) {
  //  Instruction* I = End->getFirstNonPHIOrDbgOrLifetime();
  //  End = End->
  //}

  assert(PreStart != nullptr && PreEnd != nullptr &&
         "region start and end should have single predecessors");

  Function &F = *Start->getParent();
  Module& M = *F.getParent();
  LLVMContext& C = F.getContext();

  auto OldEntryBB = &F.getEntryBlock();
  auto EntryBB = BasicBlock::Create(C, "EntryBB", &F, OldEntryBB);
  BranchInst::Create(Start, EntryBB);

  auto OldExitBB = End;
  auto ExitBB = BasicBlock::Create(C, "ExitBB", &F);
  auto RetValue = UndefValue::get(F.getReturnType());
  ReturnInst::Create(C, RetValue, ExitBB);

  End->replaceAllUsesWith(ExitBB);

  // compute inputs
  auto R = findInputsOutputs(F);

  SmallVector<Type*, 8> ArgTypes;
  SmallVector<Value*, 8> ArgValues;
  if (auto Size = R.Inputs.size()) {
    ArgTypes.reserve(Size);
    for (Value* V : R.Inputs) {
      ArgTypes.push_back(V->getType());
      ArgValues.push_back(V);
    }
  }

  ExitBB->replaceAllUsesWith(OldExitBB);
  //EntryBB->eraseFromParent();

  // compute outputs
  Type* RetType;
  if (auto Size = R.Outputs.size()) {
    SmallVector<Type*, 8> ElementTypes;
    ElementTypes.reserve(Size);
    for (Value* V : R.Outputs)
      ElementTypes.push_back(V->getType());
    RetType = StructType::get(C, ElementTypes);

    IRBuilder<> Builder(PreEnd->getTerminator());
    Value* Val = UndefValue::get(RetType);

    unsigned Index = 0;
    for (Value* V : R.Outputs) {
      Val = Builder.CreateInsertValue(Val, V, Index++);
    }
    Builder.CreateRet(Val);
  }
  else {
    RetType = Type::getVoidTy(C);
    ReturnInst::Create(C, nullptr, PreEnd->getTerminator());
  }
  PreEnd->getTerminator()->eraseFromParent();

  // Create a new function type...
  FunctionType *FTy = FunctionType::get(RetType, ArgTypes, /*isVarArg=*/false);

  // Create the new function...
  Function *NewF = Function::Create(
      FTy, GlobalValue::LinkageTypes::PrivateLinkage, F.getName() + Suffix, &M);

  for (BasicBlock* BB: R.Blocks) {
    BB->removeFromParent();
    BB->insertInto(NewF);
  }
  // make sure that the first block is first
  R.EntryBlock->removeFromParent();
  R.EntryBlock->insertInto(NewF, &NewF->getEntryBlock());

  IRBuilder<> Builder(PreStart->getTerminator());
  auto ReturnedValue = Builder.CreateCall(NewF, ArgValues, "");

  if (!R.Outputs.empty()) {
    unsigned Index = 0;
    for (Value* V : R.Outputs) {
      auto Val =
          Builder.CreateExtractValue(ReturnedValue, Index++, V->getName());
      replaceAllUsesOutside(V, Val, R.Blocks);
    }
  }
  if (auto Size = R.Inputs.size()) {
    assert(Size == NewF->getArgumentList().size());
    Function::arg_iterator AI = NewF->arg_begin();
    for (Value* V : R.Inputs) {
      Argument& Arg = *AI++;
      if (V->hasName())
        Arg.setName(V->getName());
      else
        Arg.setName("arg");
      replaceAllUsesInside(V, &Arg, R.Blocks);
    }
  }

  Builder.CreateBr(End);
  PreStart->getTerminator()->eraseFromParent();

  return NewF;
}
