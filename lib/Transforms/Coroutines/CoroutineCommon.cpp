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
#include "llvm/Transforms/Utils/CodeExtractor.h"
#include "llvm/Transforms/Coroutines.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/Debug.h"
#include "llvm/IR/Module.h"

#define DEBUG_TYPE "coro-early"

using namespace llvm;


void CoroCommon::removeLifetimeIntrinsics(Function &F) {
  for (auto it = inst_begin(F), end = inst_end(F); it != end;) {
    Instruction& I = *it++;
    if (auto II = dyn_cast<IntrinsicInst>(&I))
      switch (II->getIntrinsicID()) {
      default:
        continue;
      case Intrinsic::lifetime_start:
      case Intrinsic::lifetime_end:
        II->eraseFromParent();
        break;
      }
  }
}

Function *llvm::CoroPartExtractor::createFunction(BasicBlock *Start,
                                                  BasicBlock *End) {
  computeRegion(Start, End);
  auto F = CodeExtractor(Blocks.getArrayRef()).extractCodeRegion();
  assert(F && "failed to extract coroutine part");
  F->addFnAttr(Attribute::NoInline);
  return F;
}

void llvm::CoroPartExtractor::dump() {
  for (auto* BB : Blocks) {
    DEBUG(BB->dump());
  }
}

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

void CoroCommon::constantFoldUsers(Constant* Value) {
  SmallPtrSet<Instruction*, 16> WorkList;
  for (User *U : Value->users())
    WorkList.insert(cast<Instruction>(U));

  if (WorkList.empty())
    return;

  Instruction *FirstInstr = *WorkList.begin();
  Function* F = FirstInstr->getParent()->getParent();
  const DataLayout &DL = F->getParent()->getDataLayout();

  do {
    Instruction *I = *WorkList.begin();
    WorkList.erase(I); // Get an element from the worklist...

    if (!I->use_empty())                 // Don't muck with dead instructions...
      if (Constant *C = ConstantFoldInstruction(I, DL)) {
        // Add all of the users of this instruction to the worklist, they might
        // be constant propagatable now...
        for (User *U : I->users())
          WorkList.insert(cast<Instruction>(U));

        // Replace all of the uses of a variable with uses of the constant.
        I->replaceAllUsesWith(C);

        // Remove the dead instruction.
        WorkList.erase(I);
        I->eraseFromParent();
      }
  } while (!WorkList.empty());
}

void llvm::initializeCoroutines(PassRegistry &registry) {
  initializeCoroEarlyPass(registry);
  initializeCoroElidePass(registry);
  initializeCoroCleanupPass(registry);
  initializeCoroSplitPass(registry);
}
