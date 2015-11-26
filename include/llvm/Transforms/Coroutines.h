//===-- Coroutines.h - Coroutines Transformations ---------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This header file defines prototypes for accessor functions that expose passes
// to handle coroutines.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_COROUTINES_H
#define LLVM_TRANSFORMS_COROUTINES_H

namespace llvm {

class Pass;
class PassRegistry;

//===----------------------------------------------------------------------===//
//
// Split up coroutine into several functions
//
Pass *createCoroSplitPass();

//===----------------------------------------------------------------------===//
//
// Analyze coroutine use sites and perform heap allocation elision
//
Pass *createCoroHeapElidePass();

//===----------------------------------------------------------------------===//
//
// Clean up all remaining coroutine related intrinsics from the code
//
Pass *createCoroCleanupPass();

Pass *createCoroEarlyPass();
Pass *createCoroModuleEarlyPass();
Pass *createCoroScalarLatePass();
Pass *createCoroLastPass();
Pass *createCoroOnOpt0();

Pass *createCoroPreSplit();

/// TODO: move to llvm/InitializePasses.h?
/// initializeCoroutines - Initialize all passes linked into the Coroutines library.
void initializeCoroutines(PassRegistry&);

} // End llvm namespace

#endif
