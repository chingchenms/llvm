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

#include "llvm/Support/Compiler.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/IntrinsicInst.h"

namespace llvm {
class Module;
class Type;
class PointerType;
class IntegerType;
class ConstantInt;
class PointerType;
class IntrinsicInst;
class StructType;
class Function;
class BasicBlock;
class AllocaInst;
class PassRegistry;

struct LLVM_LIBRARY_VISIBILITY CoroutineCommon {
  Module *M = nullptr;
  Type *voidTy = nullptr;
  PointerType *bytePtrTy = nullptr;
  PointerType *anyResumeFnPtrTy = nullptr;
  IntegerType *int32Ty = nullptr;
  IntegerType *boolTy = nullptr;
  ConstantInt *zeroConstant = nullptr;
  ConstantInt *oneConstant = nullptr;
  ConstantInt *twoConstant = nullptr;
  PointerType *awaitSuspendFnPtrTy = nullptr;

  StructType *anyFrameTy = nullptr;
  PointerType *anyFramePtrTy = nullptr;

  using BlockSet = SmallPtrSet<BasicBlock *, 16>;
  using InstrSetVector = SmallSetVector<Instruction *, 32>;

  void PerModuleInit(Module &M);

  static IntrinsicInst *FindIntrinsic(BasicBlock &B, Intrinsic::ID intrinID);

  static IntrinsicInst *FindIntrinsic(Function &F, Intrinsic::ID intrinID);

  static IntrinsicInst *GetCoroElide(IntrinsicInst *CoroInit);

  static bool isCoroutine(Function& F);

  static void ComputeAllSuccessors(BasicBlock *B, SmallPtrSetImpl<BasicBlock*> &result);

  static void ComputeAllPredecessors(BasicBlock *B, BlockSet &result);

  static void ComputeDefChainNotIn(Instruction *instr, BlockSet const &source,
                                   InstrSetVector &result);

  static void ComputeDefChain(Instruction *instr, BlockSet const &source,
                              InstrSetVector &result);

  static void MoveInReverseOrder(InstrSetVector const &Instrs,
                                 Instruction *InsertBefore);

  static void ReplaceIntrinsicWith(Function &func, Intrinsic::ID id, Value *framePtr);

  void ReplaceCoroDone(IntrinsicInst *intrin);

  void ReplaceCoroPromise(IntrinsicInst *intrin, bool from = false);

  struct BranchSuccessors {
    BasicBlock *IfFalse;
    BasicBlock *IfTrue;

    BranchSuccessors() : IfFalse(), IfTrue() {}
    BranchSuccessors(IntrinsicInst *I);
    void reset(IntrinsicInst *I);
  };

  static BranchSuccessors getSuccessors(IntrinsicInst *I) { return {I}; }

  struct SuspendPoint : BranchSuccessors {
    IntrinsicInst *SuspendInst;
    SuspendPoint() : BranchSuccessors(), SuspendInst() {}
    SuspendPoint(IntrinsicInst *I) : BranchSuccessors(I), SuspendInst(I) {}
    SuspendPoint(BasicBlock *B);

    explicit operator bool() const { return SuspendInst; }
    void clear() {
      SuspendInst = nullptr;
      IfFalse = IfTrue = nullptr;
    }
  };

  static void ComputeRampBlocks(Function &F, BlockSet &RampBlocks);

  static void ComputeSharedAllocas(Function &F,
                                   SmallSetVector<AllocaInst *, 16> &result);

  void ReplaceWithIndirectCall(IntrinsicInst *intrin, ConstantInt *index, bool Erase = true);

  //IntrinsicInst *asFakeSuspend(Instruction *inst);
  //bool isFakeSuspend(Instruction *inst) { return asFakeSuspend(inst); }

  //void InsertFakeSuspend(Value *value, Instruction *InsertBefore);
  //void RemoveNoOptAttribute(Function & F);
  //void RemoveFakeSuspends(Function &F);

  static bool simplifyAndConstantFoldTerminators(Function& F);
};

/// TODO: move to llvm/InitializePasses.h?
/// initializeCoroutines - Initialize all passes linked into the Coroutines
/// library.
void initializeCoroEarlyPass(PassRegistry &Registry);
void initializeCoroEarly2Pass(PassRegistry &Registry);
void initializeCoroHeapElidePass(PassRegistry &Registry);
void initializeCoroHeapElide2Pass(PassRegistry &Registry);
void initializeCoroCleanupPass(PassRegistry &registry);
void initializeCoroSplitPass(PassRegistry &registry);
void initializeCoroSplit2Pass(PassRegistry &registry);
void initializeCoroSplit3Pass(PassRegistry &registry);
void initializeCoroPassManagerPass(PassRegistry &registry);
void initializeCoroModuleEarlyPass(PassRegistry &registry);
void initializeCoroInlinePass(PassRegistry &registry);
}

#endif