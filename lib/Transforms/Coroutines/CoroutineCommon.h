//===- CoroutineCommon.h - utilities for coroutine passes-------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file provides internal interfaces used to implement coroutine passes.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TRANSFORMS_COROUTINES_COROUTINECOMMON_H
#define LLVM_LIB_TRANSFORMS_COROUTINES_COROUTINECOMMON_H

#include "CoroInstr.h"

#include <llvm/Transforms/Coroutines.h>
#include <llvm/ADT/SetVector.h>
#include <llvm/ADT/TinyPtrVector.h>
#include <llvm/PassRegistry.h>

namespace llvm {

class Function;
class BasicBlock;
class Constant;

namespace CoroCommon {
  void removeLifetimeIntrinsics(Function &F);
  void constantFoldUsers(Constant* Value);
  BasicBlock *splitBlockIfNotFirst(Instruction *I, const Twine &Name = "");
};

/// Holds all structural Coroutine Intrinsics for a particular function.
struct LLVM_LIBRARY_VISIBILITY CoroutineShape {
  TinyPtrVector<CoroAllocInst*> CoroAlloc;
  TinyPtrVector<CoroBeginInst*> CoroBegin;
  TinyPtrVector<CoroEndInst*> CoroEndFinal;
  TinyPtrVector<CoroEndInst*> CoroEndUnwind;

  SmallVector<CoroSizeInst*, 2> CoroSize;
  SmallVector<CoroFreeInst*, 2> CoroFree;
  SmallVector<CoroFrameInst*, 4> CoroFrame;
  SmallVector<CoroSuspendInst*, 4> CoroSuspend;

  TinyPtrVector<ReturnInst*> Return;

  template <class F> void reflect(F&& f);

  void dump();
  CoroutineShape() = default;
  void buildFrom(Function &F);

private:
  void clear();
};

template <class F> void CoroutineShape::reflect(F&& f) {
  f(CoroAlloc, "CoroAlloc");
  f(CoroBegin, "CoroBegin");
  f(CoroEndFinal, "CoroEndFinal");
  f(CoroEndUnwind, "CoroEndUnwind");

  f(CoroSize, "CoroSize");
  f(CoroFree, "CoroFree");
  f(CoroFrame, "CoroFrame");
  f(CoroSuspend, "CoroSuspend");

  f(Return, "Return");
}

void buildCoroutineFrame(Function& F, CoroutineShape& Shape);

void initializeCoroEarlyPass(PassRegistry &Registry);
void initializeCoroOutlinePass(PassRegistry &Registry);
void initializeCoroElidePass(PassRegistry &Registry);
void initializeCoroCleanupPass(PassRegistry &registry);
void initializeCoroSplitPass(PassRegistry &registry);

//===----------------------------------------------------------------------===//
//
// Split up coroutine into several functions driving its state machine
//
Pass *createCoroSplitPass();

//===----------------------------------------------------------------------===//
//
// Analyze coroutine use sites and perform heap allocation elision
//
Pass *createCoroElidePass();

//===----------------------------------------------------------------------===//
//
// Clean up all remaining coroutine related intrinsics from the code
//
Pass *createCoroCleanupPass();

//===----------------------------------------------------------------------===//
//
// Lower coroutine intrinsics that are not used by later passes
//
Pass *createCoroEarlyPass();

Pass *createCoroOutlinePass();
Pass *createCoroInlinePass();


}

#endif