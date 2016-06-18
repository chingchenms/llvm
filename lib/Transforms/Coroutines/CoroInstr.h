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
#include <llvm/ADT/StringSwitch.h>

// Metadata tuple in CoroInitInstr starts with a string identifying 
// the pass that produces the metadata. It can hold one of these values:

namespace llvm {

  using CoroAllocInst = IntrinsicInst;
  using CoroSizeInst = IntrinsicInst;
  using CoroBeginInst = IntrinsicInst;
  using CoroFreeInst = IntrinsicInst;

  using CoroSaveInst = IntrinsicInst;
  using CoroSizeInst = IntrinsicInst;

  /// This represents the llvm.coro.end instruction.
  class LLVM_LIBRARY_VISIBILITY CoroSuspendInst : public IntrinsicInst {
    enum { kSave, kFallthrough };
  public:
    CoroSaveInst *getCoroSave() const {
      return cast<CoroSaveInst>(getArgOperand(kSave));
    }
    bool isFinal() const {
      return cast<Constant>(getArgOperand(kFallthrough))->isOneValue();
    }

    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::coro_suspend;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };

  /// This represents the llvm.coro.end instruction.
  class LLVM_LIBRARY_VISIBILITY CoroFrameInst : public IntrinsicInst {
  public:
    static CoroFrameInst* Create(Instruction* InsertBefore);

    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::experimental_coro_frame;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };

  /// This represents the llvm.coro.end instruction.
  class LLVM_LIBRARY_VISIBILITY CoroEndInst : public IntrinsicInst {
    enum { kFrame, kFallthrough };
  public:
    bool isFallthrough() const {
      return cast<Constant>(getArgOperand(kFallthrough))->isOneValue(); 
    }
    static CoroEndInst *Create(Instruction *InsertBefore, Value *Addr);

    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::coro_end;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };


  // Coroutine transformation occurs in phases tracked by
  // CoroInitInstr
  //    P
  enum class Phase {
    // Straight out of the front end
    Fresh,
    // Before interprocedural pipeline starts
    PreIPO,
    // Before coroutine is split
    PreSplit,
    // After coroutine is split
    PostSplit
  };

  // Assumes that the last parameter of the provided intrinsic contains
  // coroutine metadata
  struct LLVM_LIBRARY_VISIBILITY CoroMeta {
    IntrinsicInst* Intrin;

    // Fields of metadata tuple
    enum Field { Tag, Func, Frame, Parts, Resumers, COUNT };

    // Updates fields of the metadata tuple.
    // Default value Phase::Fresh is used to indicate that no updates
    // to tag is requested. 
    void updateFields(std::initializer_list<std::pair<Field, Metadata *>>,
      Phase NewPhase = Phase::Fresh);

    Phase getPhase() const;
    void setPhase(Phase Ph);
    void setParts(ArrayRef<Metadata *> MDs);
    MDNode::op_range getParts();
  };

  /// This represents the llvm.coro.init instruction.
  class LLVM_LIBRARY_VISIBILITY CoroInitInst : public IntrinsicInst {
    enum { kMem, kAlloc, kAlign, kPromise, kMeta };
  public:

    /// Provides a helpful Coroutine Metadata manipulation wrapper
    CoroMeta meta() { return{ this }; }
    CoroMeta meta() const { return {const_cast<CoroInitInst *>(this)}; }

    CoroAllocInst *getAlloc() const {
      return dyn_cast<CoroAllocInst>(getArgOperand(kAlloc));
    }

    Value *getMem() const { return getArgOperand(kMem); }

    ConstantInt *getAlignment() const {
      return cast<ConstantInt>(getArgOperand(kAlign));
    }

    bool isUntouched() const { return meta().getPhase() == Phase::Fresh; }
    bool isPostSplit() const { return meta().getPhase() >= Phase::PostSplit; }
    bool isPreSplit() const { return !isPostSplit(); }

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