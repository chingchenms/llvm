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

class PassManagerBuilder;
class PassRegistry;

/// addCoroutinePassesToExtensionPoints - Adds all coroutine passes
/// to an approriate extensions points. If VerifyEach is set to true,
/// a verifierPass will get inserted after every coroutine pass.
void addCoroutinePassesToExtensionPoints(PassManagerBuilder &Builder,
                                         bool VerifyEach = false);

/// TODO: move to llvm/InitializePasses.h?
/// initializeCoroutines - Initialize all passes linked into the Coroutines library.
void initializeCoroutines(PassRegistry&);

} // End llvm namespace

#endif
