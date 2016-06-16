//===- CoroInstr.h - Coroutine Intrinsic Instruction Wrappers ---*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
///
// This file defines classes that make it really easy to deal with coroutine
// intrinsic functions with the isa/dyncast family of functions.  In particular, 
// this allows you to do things like:
//
//     if (CoroInitInst *CI = dyn_cast<CoroInitInst>(Inst))
//        ... CI->getAlignment() ... CI->getPromise() ...
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TRANSFORMS_COROUTINES_COROINSTR_H
#define LLVM_LIB_TRANSFORMS_COROUTINES_COROINSTR_H

#include <llvm/IR/IntrinsicInst.h>

namespace llvm {

  using CoroAllocInst = IntrinsicInst;
  using CoroSizeInst = IntrinsicInst;
  using CoroBeginInst = IntrinsicInst;
  using CoroFreeInst = IntrinsicInst;

  using CoroFrameInst = IntrinsicInst;
  using CoroSuspendInst = IntrinsicInst;
  using CoroSaveInst = IntrinsicInst;

  /// This represents the llvm.coro.end instruction.
  class LLVM_LIBRARY_VISIBILITY CoroEndInst : public IntrinsicInst {
    enum { kFrame, kFallthrough };
  public:
    bool isFallthrough() const {
      return cast<Constant>(getArgOperand(kFallthrough))->isOneValue(); 
    }

    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::coro_end;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };

  /// This represents the llvm.coro.init instruction.
  class LLVM_LIBRARY_VISIBILITY CoroInitInst : public IntrinsicInst {
    enum { kMem, kAlloc, kAlign, kPromise, kMeta };
  public:
    CoroAllocInst *getAlloc() const {
      return dyn_cast<CoroAllocInst>(getArgOperand(kAlloc));
    }

    Value *getMem() const { return getArgOperand(kMem); }

    ConstantInt *getAlignment() const {
      return cast<ConstantInt>(getArgOperand(kAlign));
    }

    bool isPreSplit() const { return true; }
    bool isPostSplit() const { return !isPreSplit(); }

    Function *getCoroutine() const {
      return isPostSplit() ? nullptr
                           : const_cast<Function *>(getParent()->getParent());
    }

#if 0
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
#endif
    // Methods for support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::coro_init;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };
}

#endif