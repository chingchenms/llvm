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
#include <llvm/PassRegistry.h>

namespace llvm {

class Function;
class BasicBlock;
class Constant;

struct LLVM_LIBRARY_VISIBILITY CoroPartExtractor {
  Function *createFunction(BasicBlock *Start, BasicBlock *End);
private:
  void dump();
  SetVector<BasicBlock *> Blocks;
  void computeRegion(BasicBlock *Start, BasicBlock *End);
};

struct LLVM_LIBRARY_VISIBILITY CoroCommon {
  static void removeLifetimeIntrinsics(Function &F);
  static void constantFoldUsers(Constant* Value);
};

}

namespace llvm {
void initializeCoroEarlyPass(PassRegistry &Registry);
void initializeCoroElidePass(PassRegistry &Registry);
void initializeCoroCleanupPass(PassRegistry &registry);
void initializeCoroSplitPass(PassRegistry &registry);
}

#endif