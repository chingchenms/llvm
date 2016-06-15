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
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "coro-early"

using namespace llvm;


void llvm::removeLifetimeIntrinsics(Function &F) {
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

Function *llvm::CoroPartExtractor::createFunction(StringRef Suffix,
                                                  BasicBlock *Start,
                                                  BasicBlock *End) {
  computeRegion(Start, End);
  return CodeExtractor(Blocks.getArrayRef()).extractCodeRegion();
}

void llvm::CoroPartExtractor::dump() {
  for (auto* BB : Blocks) {
    DEBUG(BB->dump());
  }
}

void llvm::CoroPartExtractor::computeRegion(BasicBlock *Start,
                                            BasicBlock *End) {
  // Collect all predecessors of End
  SmallVector<BasicBlock*, 4> WorkList({ End });
  SmallPtrSet<BasicBlock*, 4> PredSet;
  do {
    auto BB = WorkList.pop_back_val();
    PredSet.insert(BB);
    for (auto PBB : predecessors(BB))
      if (PredSet.count(PBB) == 0)
        WorkList.push_back(PBB);
  } while (!WorkList.empty());

  // Now collect all successors of Start from the
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

void llvm::initializeCoroutines(PassRegistry &registry) {
  initializeCoroEarlyPass(registry);
  initializeCoroElidePass(registry);
  initializeCoroCleanupPass(registry);
  initializeCoroSplitPass(registry);
}
