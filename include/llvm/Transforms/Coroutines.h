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

/// TODO: move to llvm/InitializePasses.h?
/// initializeCoroutines - Initialize all passes linked into the Coroutines library.
void initializeCoroutines(PassRegistry&);

} // End llvm namespace

#endif
