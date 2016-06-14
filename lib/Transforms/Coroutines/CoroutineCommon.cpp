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
#include "llvm/Transforms/Coroutines.h"

using namespace llvm;

void llvm::initializeCoroutines(PassRegistry &registry) {
  initializeCoroEarlyPass(registry);
  initializeCoroElidePass(registry);
  initializeCoroCleanupPass(registry);
  initializeCoroSplitPass(registry);
}
