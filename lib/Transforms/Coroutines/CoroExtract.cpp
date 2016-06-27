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

#include <llvm/Transforms/Utils/CodeExtractor.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/CFG.h>

using namespace llvm;

#if 1
Function *llvm::CoroPartExtractor::createFunction(BasicBlock *Start,
  BasicBlock *End) {
  computeRegion(Start, End);
  auto F = CodeExtractor(Blocks.getArrayRef()).extractCodeRegion();
  assert(F && "failed to extract coroutine part");
  F->addFnAttr(Attribute::AlwaysInline);
  return F;
}

void llvm::CoroPartExtractor::dump() {
  for (auto *BB : Blocks)
    BB->dump();
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
#endif