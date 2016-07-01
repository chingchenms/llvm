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
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/GlobalVariable.h> // TODO: move to .cpp

// Metadata tuple in CoroInitInstr starts with a string identifying 
// the pass that produces the metadata. It can hold one of these values:

namespace llvm {

  /// This represents the llvm.coro.free instruction.
  class LLVM_LIBRARY_VISIBILITY CoroSubFnInst : public IntrinsicInst {
    enum { kFrame, kIndex };
  public:
    Value *getFrame() const { return getArgOperand(kFrame); }
    int getIndex() const {
      auto C = cast<ConstantInt>(getArgOperand(kIndex)); 
      return C->getValue().getSExtValue();
    }

    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::coro_subfn_addr;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };

  /// This represents the llvm.coro.free instruction.
  class LLVM_LIBRARY_VISIBILITY CoroSizeInst : public IntrinsicInst {
  public:
    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::coro_size;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };

  /// This represents the llvm.coro.free instruction.
  class LLVM_LIBRARY_VISIBILITY CoroSaveInst : public IntrinsicInst {
    enum { kFinal };
  public:
    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::coro_save;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };

  /// This represents the llvm.coro.free instruction.
  class LLVM_LIBRARY_VISIBILITY CoroAllocInst : public IntrinsicInst {
  public:
    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::coro_alloc;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };

  /// This represents the llvm.coro.free instruction.
  class LLVM_LIBRARY_VISIBILITY CoroReturnInst : public IntrinsicInst {
    enum { kEnd };
  public:
    Value *getEnd() const { return getArgOperand(kEnd); }
    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::coro_return;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };

  /// This represents the llvm.coro.free instruction.
  class LLVM_LIBRARY_VISIBILITY CoroFreeInst : public IntrinsicInst {
    enum { kFrame };
  public:
    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::coro_free;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };

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
    static CoroFrameInst* Create(IRBuilder<>& Builder);
//    static CoroFrameInst* Create(Instruction* InsertBefore);
//    static CoroFrameInst* Create(BasicBlock* InsertAfter);

    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::coro_frame;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };

  /// This represents the llvm.coro.end instruction.
  class LLVM_LIBRARY_VISIBILITY CoroEndInst : public IntrinsicInst {
    enum { kFrame };//, kUnwind  };
  public:
    //bool isFallthrough() const { return !isUnwind(); }
    //bool isUnwind() const {
    //  return cast<Constant>(getArgOperand(kUnwind))->isOneValue();
    //}
    //void setUnwind(bool Value);
    Value *getFrameArg() const { return getArgOperand(kFrame); }
    static CoroEndInst *Create(Instruction *InsertBefore,
                               Value *FreeAddr = nullptr);

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

    AllocaInst *getPromise() const { 
      Value* Arg = getArgOperand(kPromise);
      return isa<ConstantPointerNull>(Arg)
                 ? nullptr
                 : cast<AllocaInst>(Arg->stripPointerCasts());
    }
    void clearPromise() {
      setArgOperand(kPromise, 
        ConstantPointerNull::get(Type::getInt8PtrTy(getContext())));
    }

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
    Value* getRawInfo() const {
      return getArgOperand(kInfo)->stripPointerCasts();
    }
    void setInfo(Value* C) {
      setArgOperand(kInfo, C);
    }

    struct Info {
      ConstantStruct* OutlinedParts = nullptr;
      ConstantArray* Resumers = nullptr;

      bool needToOutline() const {
        return (Resumers == nullptr) && (OutlinedParts == nullptr);
      }
      bool needToSplit() const { return OutlinedParts != nullptr; }
      bool postSplit() const { return Resumers != nullptr; }
      bool isPreSplit() const { return !postSplit(); }
    };

    Info getInfo() const {
      Info Result;
      auto GV = dyn_cast<GlobalVariable>(getRawInfo());
      if (!GV)
        return Result;

      assert(GV->isConstant() && GV->hasDefinitiveInitializer());
      Constant* Initializer = GV->getInitializer();
      if ((Result.OutlinedParts = dyn_cast<ConstantStruct>(Initializer)))
        return Result;

      Result.Resumers = cast<ConstantArray>(Initializer);
      return Result;
    }

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