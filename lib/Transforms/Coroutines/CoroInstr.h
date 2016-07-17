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

namespace llvm {

  class CoroBeginInst;

  /// This represents the llvm.coro.subfn instruction.
  class LLVM_LIBRARY_VISIBILITY CoroSubFnInst : public IntrinsicInst {
    enum { kFrame, kIndex };
  public:
    Value *getFrame() const { return getArgOperand(kFrame); }
    ConstantInt* getRawIndex() const {
      return cast<ConstantInt>(getArgOperand(kIndex));
    }
    int getIndex() const { return getRawIndex()->getValue().getSExtValue(); }

    static CoroSubFnInst *Create(IRBuilder<> &Builder, Value *FramePtr,
                                 uint8_t Index);

    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::coro_subfn_addr;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };

  /// This represents the llvm.coro.size instruction.
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

  /// This represents the llvm.coro.alloc instruction.
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

  class LLVM_LIBRARY_VISIBILITY CoroSuspendInst;

  /// This represents the llvm.coro.save instruction.
  class LLVM_LIBRARY_VISIBILITY CoroSaveInst : public IntrinsicInst {
  public:
    static CoroSaveInst *Create(CoroBeginInst*, CoroSuspendInst *);

    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::coro_save;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };

  /// This represents the llvm.coro.suspend instruction.
  class LLVM_LIBRARY_VISIBILITY CoroSuspendInst : public IntrinsicInst {
    enum { kSave, kFinal };
  public:
    CoroSaveInst *getCoroSave() const {
      if (auto SI = dyn_cast<CoroSaveInst>(getArgOperand(kSave)))
        return SI;
      assert(isa<ConstantTokenNone>(getArgOperand(kSave)));
      return nullptr;
    }
    bool isFinal() const {
      return cast<Constant>(getArgOperand(kFinal))->isOneValue();
    }

    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::coro_suspend;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };

  /// This represents the llvm.coro.frame instruction.
  class LLVM_LIBRARY_VISIBILITY CoroFrameInst : public IntrinsicInst {
  public:
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
    enum { kFrame, kUnwind  };
  public:
    bool isFinal() const { return !isUnwind(); }
    bool isUnwind() const {
      return cast<Constant>(getArgOperand(kUnwind))->isOneValue();
    }

    // Methods to support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::coro_end;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };

  /// This represents the llvm.coro.begin instruction.
  class LLVM_LIBRARY_VISIBILITY CoroBeginInst : public IntrinsicInst {
    enum { kMem, kAlign, kPromise, kInfo };
  public:

    CoroAllocInst *getAlloc() const {
      if (auto PN = dyn_cast<PHINode>(getMem()))
        for (Value *V : PN->incoming_values())
          if (auto CA = dyn_cast<CoroAllocInst>(V))
            return CA;
      return nullptr;
    }

    Value *getMem() const { return getArgOperand(kMem); }

    AllocaInst *getPromise() const { 
      Value* Arg = getArgOperand(kPromise);
      return isa<ConstantPointerNull>(Arg)
                 ? nullptr
                 : cast<AllocaInst>(Arg->stripPointerCasts());
    }

    void clearPromise() {
      Value* Arg = getArgOperand(kPromise);
      setArgOperand(kPromise, 
        ConstantPointerNull::get(Type::getInt8PtrTy(getContext())));
      if (isa<AllocaInst>(Arg))
        return;
      assert((isa<BitCastInst>(Arg) || isa<GetElementPtrInst>(Arg)) &&
             "unexpected instruction designating the promise");
      auto Inst = cast<Instruction>(Arg);
      if (Inst->use_empty())
        Inst->eraseFromParent();
      else
        Inst->moveBefore(getNextNode());
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