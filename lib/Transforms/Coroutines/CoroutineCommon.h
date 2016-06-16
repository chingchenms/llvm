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

#include "llvm/Transforms/Coroutines.h"
#include <llvm/IR/IntrinsicInst.h>
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringSwitch.h"
#include <llvm/PassRegistry.h>

namespace llvm {

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

#define EMULATE_INTRINSICS 1

#if EMULATE_INTRINSICS // emulate intrinsics via functions for faster turnaround

  class LLVM_LIBRARY_VISIBILITY CoroIntrinsic : public CallInst {
  public:
    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const CallInst *I) {
      return I->getCalledFunction() != nullptr;
    }
    static inline bool classof(const Value *V) {
      return isa<CallInst>(V) && classof(cast<CallInst>(V));
    }
    int getIntrinsicID() const {
      return StringSwitch<int>(getCalledFunction()->getName())
        .Case("CoroAlloc", Intrinsic::experimental_coro_elide)
        .Case("CoroInit", Intrinsic::coro_init)
        .Case("CoroStart", Intrinsic::coro_fork)
        .Case("CoroFree", Intrinsic::experimental_coro_delete)
        .Case("CoroEnd", Intrinsic::coro_end)
        .Default(0);

    }
  };

  class LLVM_LIBRARY_VISIBILITY CoroEndInst : public CoroIntrinsic {
  public:
    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const CoroIntrinsic *I) {
      return I->getIntrinsicID() == Intrinsic::coro_end;
    }
    static inline bool classof(const Value *V) {
      return isa<CoroIntrinsic>(V) && classof(cast<CoroIntrinsic>(V));
    }
  };

  class LLVM_LIBRARY_VISIBILITY CoroAllocInst : public CoroIntrinsic {
  public:
    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const CoroIntrinsic *I) {
      return I->getIntrinsicID() == Intrinsic::experimental_coro_elide;
    }
    static inline bool classof(const Value *V) {
      return isa<CoroIntrinsic>(V) && classof(cast<CoroIntrinsic>(V));
    }
  };

  class LLVM_LIBRARY_VISIBILITY CoroForkInst : public CoroIntrinsic {
  public:
    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const CoroIntrinsic *I) {
      return I->getIntrinsicID() == Intrinsic::coro_fork;
    }
    static inline bool classof(const Value *V) {
      return isa<CoroIntrinsic>(V) && classof(cast<CoroIntrinsic>(V));
    }
  };

  class LLVM_LIBRARY_VISIBILITY CoroFreeInst : public CoroIntrinsic {
  public:
    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const CoroIntrinsic *I) {
      return I->getIntrinsicID() == Intrinsic::experimental_coro_delete;
    }
    static inline bool classof(const Value *V) {
      return isa<CoroIntrinsic>(V) && classof(cast<CoroIntrinsic>(V));
    }
  };

  class LLVM_LIBRARY_VISIBILITY CoroInitInst : public CoroIntrinsic {
    enum { kMem, kElide, kAlign, kPromise, kMeta };
  public:
    Value *getMem() const {
      return getArgOperand(kMem);
    }
    CoroAllocInst *getAlloc() const {
      return dyn_cast<CoroAllocInst>(getArgOperand(kElide));
    }
    bool isPostSplit() const { return false; }

    Function *getCoroutine() const {
      return const_cast<Function *>(getParent()->getParent());
    }

    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const CoroIntrinsic *I) {
      return I->getIntrinsicID() == Intrinsic::coro_init;
    }
    static inline bool classof(const Value *V) {
      return isa<CoroIntrinsic>(V) && classof(cast<CoroIntrinsic>(V));
    }
  };

#else
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
#endif

void initializeCoroEarlyPass(PassRegistry &Registry);
void initializeCoroElidePass(PassRegistry &Registry);
void initializeCoroCleanupPass(PassRegistry &registry);
void initializeCoroSplitPass(PassRegistry &registry);
}

#endif