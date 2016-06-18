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

  using CoroFrameInst = IntrinsicInst;
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

#define kCoroPreIPOTag     "0 preIPO"
#define kCoroPreSplitTag   "1 preSplit"
#define kCoroPostSplitTag  "2 postSplit"


  /// This represents the llvm.coro.init instruction.
  class LLVM_LIBRARY_VISIBILITY CoroInitInst : public IntrinsicInst {
    enum { kMem, kAlloc, kAlign, kPromise, kMeta };
  public:

    Phase getPhase() const {
      auto MD = getRawMeta();
      if (dyn_cast<MDString>(MD))
        return Phase::Fresh;

      auto N = dyn_cast<MDNode>(MD);
      assert(N && "invalid metadata on CoroInit instruction");

      // {!"tag", {elide-info}, {outline-info}}
      return StringSwitch<Phase>(cast<MDString>(N->getOperand(0))->getString())
        .Case(kCoroPreIPOTag, Phase::PreIPO)
        .Case(kCoroPreSplitTag, Phase::PreSplit)
        .Default(Phase::PostSplit);
    }

    void setPhase(Phase Ph) {
      if (Ph == Phase::PreIPO) {
        assert(getPhase() == Phase::Fresh && "invalid phase transition");
        LLVMContext& C = getContext();
        auto Empty = MDString::get(C, "");
        Metadata *Args[4] = {
            MDString::get(C, kCoroPreIPOTag), ValueAsMetadata::get(getCoroutine()), Empty, Empty};
        setMeta(MDNode::get(C, Args));
        return;
      }
    }

    CoroAllocInst *getAlloc() const {
      return dyn_cast<CoroAllocInst>(getArgOperand(kAlloc));
    }

    Value *getMem() const { return getArgOperand(kMem); }

    ConstantInt *getAlignment() const {
      return cast<ConstantInt>(getArgOperand(kAlign));
    }

#if 0
    /// isUntouched() returns true if no coroutine passes
    /// transformed coroutine yet.
    bool isUntouched() const {
      // If it is a string, it means it is straight out of the frontend
      return isa<MDString>(getRawMeta());
    }
    MDNode *getParts() const {
      auto MD = getRawMeta();
      if (auto N = dyn_cast<MDNode>(MD))
        if (N->getNumOperands() > 1)
          if (cast<MDString>(N->getOperand(0))->getString() ==
              kCoroEarlyTagStr)
            return N;
      return nullptr;
    }

    MDNode *getResumers() const {
      auto MD = getRawMeta();
      if (auto N = dyn_cast<MDNode>(MD))
        if (N->getNumOperands() > 1)
          if (cast<MDString>(N->getOperand(0))->getString() ==
            kCoroSplitTagStr)
            return N;
      return nullptr;
    }
#endif

    Function *getCoroutine() const {
      switch (getPhase()) {
      case Phase::Fresh:
        return const_cast<Function*>(cast<Function>(getParent()->getParent()));
      case Phase::PostSplit:
        return nullptr; // no longer a coroutine
      default:
        auto MD = getRawMeta();
        auto N = dyn_cast<MDNode>(MD);
        return cast<Function>(
            cast<ValueAsMetadata>(N->getOperand(1))->getValue());
      }
    }

    MDNode::op_range getParts() {
      auto N = cast<MDNode>(getRawMeta());
      auto P = cast<MDNode>(N->getOperand(3));
      return P->operands();
    }

    void setMeta(Metadata *MD) {
      setArgOperand(kMeta, MetadataAsValue::get(getContext(), MD));
    }

    void setParts(ArrayRef<Metadata *> MDs) {
      assert(getPhase() == Phase::PreIPO && "can only outline in preIPO phase");
      auto N = cast<MDNode>(getRawMeta());

      LLVMContext &C = getContext();
      Metadata *Args[4] = {N->getOperand(0).get(), N->getOperand(1).get(),
                           N->getOperand(2).get(), MDNode::get(C, MDs)};
      setMeta(MDNode::get(C, Args));
    }

    Metadata *getRawMeta() const {
      return cast<MetadataAsValue>(getArgOperand(kMeta))->getMetadata();
    }

    bool isUntouched() const { return getPhase() == Phase::Fresh; }
    bool isPostSplit() const { return getPhase() >= Phase::PostSplit; }
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