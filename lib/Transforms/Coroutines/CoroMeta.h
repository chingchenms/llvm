//===- CoroMeta.h - Coroutine Intrinsic Instruction Wrappers ---*- C++ -*-===//
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
// metadata.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_LIB_TRANSFORMS_COROUTINES_COROMETA_H
#define LLVM_LIB_TRANSFORMS_COROUTINES_COROMETA_H

#include <llvm/Support/Compiler.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/Metadata.h>
#include <initializer_list>

namespace llvm {

  class IntrinsicInst;
  class Value;
  class Metadata;

  struct LLVM_LIBRARY_VISIBILITY CoroInfo {
    Type* FrameType;
    Function* Resume;
    Function* Destroy;
    Function* Cleanup;

    void dump();
  };

  // Coroutine transformation occurs in phases tracked by CoroInitInstr
  enum class Phase {
    // Straight out of the front end
    Fresh,
    // Before interprocedural pipeline starts
    NotReadyForSplit,
    // Before coroutine is split
    ReadyForSplit,
    // After coroutine is split
    PostSplit
  };

  // Assumes that the last parameter of the provided intrinsic contains
  // coroutine metadata
  struct LLVM_LIBRARY_VISIBILITY CoroMeta {
    IntrinsicInst* Intrin;

    // Fields of metadata tuple
    enum Field { Tag, Func, Frame, Parts, Resumers, COUNT };

    // Helper to struct to update a particular field in CoroMeta tuple
    struct Setter {
      Field FieldNo;
      Metadata* NewValue;
      
      Setter(Field F, ArrayRef<Metadata*> MDs, LLVMContext& C);
      Setter(Field F, ArrayRef<Value*> Values);
      Setter(Phase V, LLVMContext& C);
      Setter(Type* T);
    };

    // Updates fields of the metadata tuple.
    // Default value Phase::Fresh is used to indicate that no updates
    // to tag is requested. 
    void update(std::initializer_list<Setter>);

    Phase getPhase() const;
    void setPhase(Phase NewPhase);

    MDNode::op_range getParts();
    void setParts(ArrayRef<Metadata *> MDs);

    CoroInfo getCoroInfo();
    Type* getFrameType();

  };

} // end namespace llvm

#endif