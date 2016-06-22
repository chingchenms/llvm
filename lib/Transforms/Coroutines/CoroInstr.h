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
#include <llvm/IR/GlobalVariable.h> // TODO: move to .cpp

// Metadata tuple in CoroInitInstr starts with a string identifying 
// the pass that produces the metadata. It can hold one of these values:

#define coro_begin coro_start
#define coro_init coro_unused_please_remove

namespace llvm {

  using CoroAllocInst = IntrinsicInst;
  using CoroSizeInst = IntrinsicInst;
  using CoroFreeInst = IntrinsicInst;

  using CoroSaveInst = IntrinsicInst;

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

  /// This represents the llvm.coro.init instruction.
  class LLVM_LIBRARY_VISIBILITY CoroBeginInst : public IntrinsicInst {
    enum { kMem, kAlloc, kAlign, kPromise, kInfo };
  public:

    CoroAllocInst *getAlloc() const {
      return dyn_cast<CoroAllocInst>(getArgOperand(kAlloc));
    }

    Value *getMem() const { return getArgOperand(kMem); }

    ConstantInt *getAlignment() const {
      return cast<ConstantInt>(getArgOperand(kAlign));
    }
    void setAlignment(unsigned Align) {
      auto * C = ConstantInt::get(Type::getInt32Ty(getContext()), Align);
      setArgOperand(kAlign, C);
    }

    // fresh - i8* null
    // outined - {Init, Return, Susp1, Susp2, ...}
    // postsplit - [resume, destroy, cleanup]
    Value* getInfo() const {
      return getArgOperand(kInfo)->stripPointerCasts();
    }
    void setInfo(Value* C) {
      setArgOperand(kInfo, C);
    }

    bool isUnprocessed() const { 
      auto V = getInfo();
      return isa<ConstantPointerNull>(V); 
    }
    bool isReadyForSplit() const {
      if (isUnprocessed())
        return false;
      auto V = getInfo();V;
      return true;
    }

    ConstantStruct* getOutlinedParts() const {
      auto GV = cast<GlobalVariable>(getInfo());
      assert(GV->isConstant() && GV->hasDefinitiveInitializer());
      auto init = GV->getInitializer();
      return cast<ConstantStruct>(init);
    }

    ConstantArray* getResumers() const {
      auto GV = cast<GlobalVariable>(getInfo());
      assert(GV->isConstant() && GV->hasDefinitiveInitializer());
      auto init = GV->getInitializer();
      return cast<ConstantArray>(init);
    }

    bool isPostSplit() const {
      auto GV = dyn_cast<GlobalVariable>(getInfo());
      if (!GV)
        return false;

      assert(GV->isConstant() && GV->hasDefinitiveInitializer());
      auto init = GV->getInitializer();
      bool isArray = isa<ConstantArray>(init);
      return isArray;
    }

    //bool isUntouched() const { return meta().getPhase() == Phase::Fresh; }
    //bool isPostSplit() const { return meta().getPhase() >= Phase::PostSplit; }
    bool isPreSplit() const { return true; } // FIXME:

    // Methods for support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::coro_begin;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };
}

#endif