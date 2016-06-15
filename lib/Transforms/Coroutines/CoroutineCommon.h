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

#include <llvm/IR/IntrinsicInst.h>
#include <llvm/PassRegistry.h>

namespace llvm {

/// This represents the llvm.coro.elide instruction.
class LLVM_LIBRARY_VISIBILITY CoroElideInst : public IntrinsicInst {
public:
  // Methods to support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const IntrinsicInst *I) {
    return I->getIntrinsicID() == Intrinsic::experimental_coro_elide;
  }
  static inline bool classof(const Value *V) {
    return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
  }
};

/// This represents the llvm.coro.init instruction.
class LLVM_LIBRARY_VISIBILITY CoroInitInst : public IntrinsicInst {
  enum { kMem, kElide, kAlign, kPromise, kMeta };
public:
  CoroElideInst *getElide() const {
    return dyn_cast<CoroElideInst>(getArgOperand(kElide));
  }

  Value *getMem() const { return getArgOperand(kMem); }

  ConstantInt *getAlignment() const {
    return cast<ConstantInt>(getArgOperand(kAlign));
  }

  Function *getCoroutine() const {
    auto MD = cast<MetadataAsValue>(getArgOperand(kMeta))->getMetadata();
    auto MV = dyn_cast<ValueAsMetadata>(MD);
    if (!MV)
      return nullptr;

    auto F = dyn_cast<Function>(MV->getValue());
    assert(getParent()->getParent() == F &&
           "coro.init metadata does not point at enclosing function");
    return F;
  }

  bool isPostSplit() const { return getCoroutine() == nullptr; }

  // Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const IntrinsicInst *I) {
    return I->getIntrinsicID() == Intrinsic::coro_init;
  }
  static inline bool classof(const Value *V) {
    return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
  }
};

void initializeCoroEarlyPass(PassRegistry &Registry);
void initializeCoroElidePass(PassRegistry &Registry);
void initializeCoroCleanupPass(PassRegistry &registry);
void initializeCoroSplitPass(PassRegistry &registry);
}

#endif