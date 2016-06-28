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
      for (Use& U : I.operands())
        if (!DT.isReachableFromEntry(U))
          Result.Inputs.insert(U.get());

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
    if (Usr && Blocks.count(Usr->getParent()))
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
    if (Usr && !Blocks.count(Usr->getParent()))
      continue;
    U.set(New);
  }
}

#if 0
static void betterComputeRegion(BasicBlock *Start,
  BasicBlock *End) {
  Function& F = *Start->getParent();
  LLVMContext& C = F.getContext();

  ValueToValueMapTy VMap;
  auto NewF = CloneFunction(&F, VMap, false);

  // make new entry and exit blocks
  auto OldEntryBB = &NewF->getEntryBlock();
  auto EntryBB = BasicBlock::Create(C, "", NewF, OldEntryBB);
  EntryBB->takeName(OldEntryBB);
  BranchInst::Create(cast<BasicBlock>(VMap[Start]), EntryBB);

  auto OldExitBB = cast<BasicBlock>(VMap[End]);
  auto ExitBB = BasicBlock::Create(C, "ExitBB", NewF);
  auto RetValue = UndefValue::get(F.getReturnType());
  ReturnInst::Create(C, RetValue, ExitBB);

  OldExitBB->replaceAllUsesWith(ExitBB);
   
  // compute inputs
  auto R = findInputsOutputs(*NewF);

  ExitBB->replaceAllUsesWith(OldExitBB);
  EntryBB->eraseFromParent();

  Type* retType;

  if (!R.Outputs.empty()) {
    SmallVector<Type*, 8> ElementTypes;
    ElementTypes.reserve(R.Outputs.size());
    for (Value* V : R.Outputs)
      ElementTypes.push_back(V->getType());
    retType = StructType::create(ElementTypes);

    IRBuilder<> Builder(ExitBB->getTerminator());
    Value* Val = UndefValue::get(retType);

    unsigned Index = 0;
    for (Value* V : R.Outputs) {
      Val = Builder.CreateInsertValue(Val, V, Index++);
    }
  }
}
#endif
Function *llvm::CoroPartExtractor::createFunction(BasicBlock *Start,
  BasicBlock *End, Twine Suffix) {
#if 1
  auto PreStart = Start->getSinglePredecessor();
  auto PreEnd = End->getSinglePredecessor();

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

  ExitBB->replaceAllUsesWith(OldExitBB);
  //EntryBB->eraseFromParent();

  Type* RetType;

  if (!R.Outputs.empty()) {
    SmallVector<Type*, 8> ElementTypes;
    ElementTypes.reserve(R.Outputs.size());
    for (Value* V : R.Outputs)
      ElementTypes.push_back(V->getType());
    RetType = StructType::get(C, ElementTypes); //, Suffix.getSingleStringRef());

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
  FunctionType *FTy = FunctionType::get(RetType,{});

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
  auto ReturnedValue = Builder.CreateCall(NewF, {}, "");

  if (!R.Outputs.empty()) {
    unsigned Index = 0;
    for (Value* V : R.Outputs) {
      auto Val =
          Builder.CreateExtractValue(ReturnedValue, Index++, V->getName());
      replaceAllUsesOutside(V, Val, R.Blocks);
    }
  }

  Builder.CreateBr(End);
  PreStart->getTerminator()->eraseFromParent();

  return NewF;
}

#else
  betterComputeRegion(Start, End);
  computeRegion(Start, End);
  auto F = CodeExtractor(Blocks.getArrayRef()).extractCodeRegion();
  assert(F && "failed to extract coroutine part");
  F->addFnAttr(Attribute::AlwaysInline);
  return F;
#endif

#if 0
//void llvm::CoroPartExtractor::dump() {
//  for (auto *BB : Blocks)
//    BB->dump();
//}

void llvm::CoroPartExtractor::computeRegion(BasicBlock *Start,
  BasicBlock *End) {
  SmallVector<BasicBlock*, 4> WorkList({ End });
  SmallPtrSet<BasicBlock*, 4> PredSet;

  // Collect all predecessors of the End block
  do {
    auto BB = WorkList.pop_back_val();
    PredSet.insert(BB);
    for (auto PBB : predecessors(BB))
      if (PredSet.count(PBB) == 0)
        WorkList.push_back(PBB);
  } while (!WorkList.empty());

  // Now collect all successors of the Start block from the
  // set of predecessors of End
  Blocks.clear();
  WorkList.push_back(Start);
  do {
    auto BB = WorkList.pop_back_val();
    if (PredSet.count(BB) == 0)
      continue;

    Blocks.insert(BB);
    for (auto SBB : successors(BB))
      if (Blocks.count(SBB) == 0)
        WorkList.push_back(SBB);
  } while (!WorkList.empty());

  Blocks.remove(End);
}
#endif