//===- CoroSplit2.cpp - Manager for Coroutine Passes -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// wait for it
//
//===----------------------------------------------------------------------===//

#include "CoroutineCommon.h"
#include "llvm/Transforms/Coroutines.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/IPO/InlinerPass.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Type.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define DEBUG_TYPE "coro-split2"

namespace {
struct CoroutineInfo : CoroutineCommon {
  Function *ThisFunction;

  IntrinsicInst *CoroInit;
  IntrinsicInst *CoroDelete;

  struct Suspend {
    IntrinsicInst* Intrin;
    BasicBlock* IfTrue = nullptr;
    BasicBlock* IfFalse = nullptr;

    Suspend(IntrinsicInst* I) : Intrin(I) {}
  };

  SmallVector<Suspend, 8> Suspends;
  SmallVector<AllocaInst *, 8> AllAllocas;
  SmallSetVector<AllocaInst *, 8> SharedAllocas;
  BasicBlock *ReturnBlock;
  BasicBlock *DeleteBlock;

  BlockSet RampBlocks;
  BlockSet CleanupBlocks;

  Function *resumeFn = nullptr;
  Function *destroyFn = nullptr;
  Function *cleanupFn = nullptr;

  SmallString<16> smallString;
  StructType *frameTy = nullptr;
  PointerType *framePtrTy = nullptr;
  FunctionType *resumeFnTy = nullptr;
  PointerType *resumeFnPtrTy = nullptr;

  void init(Function &F) {
    this->ThisFunction = &F;
    smallString.clear();
    frameTy = StructType::create(
        M->getContext(), (F.getName() + ".frame").toStringRef(smallString));
    framePtrTy = PointerType::get(frameTy, 0);
    resumeFnTy = FunctionType::get(voidTy, framePtrTy, false);
    resumeFnPtrTy = PointerType::get(resumeFnTy, 0);
  }

  Value *frameInDestroy;
  Value *frameInCleanup;
  Value *frameInResume;
  Instruction *frameInRamp;

  Value *vFrameInDestroy;
  Value *vFrameInCleanup;
  Value *vFrameInResume;
  Instruction *vFrameInRamp;

  Function *CreateAuxillaryFunction(Twine suffix, Value *&frame,
                                    Value *&vFrame) {
    auto func = Function::Create(resumeFnTy, GlobalValue::InternalLinkage,
                                 ThisFunction->getName() + suffix, M);
    func->setCallingConv(CallingConv::Fast);
    frame = &*func->arg_begin();
    frame->setName("frame.ptr");

    auto entry = BasicBlock::Create(M->getContext(), "entry", func);
    vFrame = new BitCastInst(frame, bytePtrTy, "frame.void.ptr", entry);
    return func;
  }

  void CreateAuxillaryFunctions() {
    resumeFn =
        CreateAuxillaryFunction(".resume", frameInResume, vFrameInResume);
    cleanupFn =
        CreateAuxillaryFunction(".cleanup", frameInCleanup, vFrameInCleanup);
    destroyFn =
        CreateAuxillaryFunction(".destroy", frameInDestroy, vFrameInDestroy);

    auto CoroInit = FindIntrinsic(*ThisFunction, Intrinsic::coro_init);
    assert(CoroInit && "Call to @llvm.coro.init is missing");
    vFrameInRamp = CoroInit;
    frameInRamp = new BitCastInst(vFrameInRamp, framePtrTy, "frame",
                                  CoroInit->getNextNode());
  }

#if 0
  void AnalyzeFunction2(Function &F) {
    init(F);

    CoroInit = nullptr;
    CoroDelete = nullptr;
    ReturnBlock = nullptr;
    Suspends.clear();
    AllAllocas.clear();

    for (auto &I : instructions(F)) {
      I.dump();
      if (auto AI = dyn_cast<AllocaInst>(&I)) {
        AllAllocas.emplace_back(AI);
        continue;
      }
      if (auto II = dyn_cast<IntrinsicInst>(&I)) {
        switch (II->getIntrinsicID()) {
        default:
          continue;
        case Intrinsic::coro_suspend:
          //Suspends.emplace_back(II);
          continue;
        case Intrinsic::coro_init:
          assert(!CoroDelete && "more than one @llvm.coro.init in a coroutine");
          CoroInit = II;
          continue;
        case Intrinsic::coro_delete:
          assert(!CoroDelete && "more than one @llvm.coro.delete in a coroutine");
          CoroDelete = II;
          continue;
        }
      }
    }

    assert(CoroInit && "missing @llvm.coro.init");
    assert(CoroDelete && "missing @llvm.coro.delete");
    assert(Suspends.size() != 0 && "cannot handle no suspend coroutines yet");

    DeleteBlock = CoroDelete->getParent();
    assert(isa<ReturnInst>(DeleteBlock->getTerminator()) && "expecting delete block to end with return");
  }
#endif
  void ReplaceSuccessors(BasicBlock *B, BasicBlock *oldTarget,
                         BasicBlock *newTarget) {
    // FIXME: can't handle anything but the branch instructions
    auto BI = cast<BranchInst>(B->getTerminator());
    unsigned ReplacedCount = 0;
    for (unsigned i = 0; i < BI->getNumSuccessors(); ++i) {
      if (BI->getSuccessor(i) == oldTarget) {
        BI->setSuccessor(i, newTarget);
        ++ReplacedCount;
      }
    }
    assert(ReplacedCount > 0 && "failed to replace any successors");
  }

  BasicBlock *CloneBlock(BasicBlock *BB, BasicBlock *PrevBlock) {
    ValueToValueMapTy VMap;
    auto NewBB = CloneBasicBlock(BB, VMap, ".clone", ThisFunction);
    ReplaceSuccessors(PrevBlock, BB, NewBB);
    for (Instruction &I : *NewBB)
      RemapInstruction(&I, VMap,
                       RF_NoModuleLevelChanges | RF_IgnoreMissingEntries);
    return NewBB;
  }

  BasicBlock *MoveInstructionWithItsUsers(Twine Name, Instruction* Instr) {   
    SmallSet<Instruction*, 8> InstructionsToMove;
    SmallVector<Instruction*, 8> Worklist;
    Worklist.push_back(Instr);

    while (!Worklist.empty()) {
      Instruction* I = Worklist.pop_back_val();
      if (isa<AllocaInst>(I))
        continue;

      InstructionsToMove.insert(I);
      for (Use& Operand : I->operands()) {
        if (auto Op = dyn_cast<Instruction>(&Operand)) {
          if (InstructionsToMove.count(Op) == 0)
            Worklist.push_back(Op);
          continue;
        }
        assert(!isa<Argument>(&Operand) && "Cannot move");
      }
    }

    if (isa<TerminatorInst>(Instr)) {
      // if we are removing a terminator, put a ret instruction
      // instead
      ReturnInst::Create(M->getContext(), Instr->getParent());
    }

    Worklist.clear();
    for (auto& I : instructions(ThisFunction))
      if (InstructionsToMove.count(&I) != 0)
        Worklist.push_back(&I);

    InstructionsToMove.clear();

    BasicBlock* PreviousOwner = Instr->getParent();
    auto NewBlock = BasicBlock::Create(M->getContext(), Name, ThisFunction);
    auto InsertionPt = ReturnInst::Create(M->getContext(), NewBlock);
    for (auto I: Worklist) {
      I->removeFromParent();
      I->insertBefore(InsertionPt);
    }
    InsertionPt->eraseFromParent();

    // check that all users are in the new block
    for (auto& I : *NewBlock)
      for (auto U : I.users())
        if (auto I = dyn_cast<Instruction>(U))
          assert(I->getParent() == NewBlock && "all users must be in the same block");

    return NewBlock;
  }

  // replace all uses of allocas with gep from frame struct
  void ReplaceSharedUses() {
    APInt fieldNo(32, 2); // Fields start with after 2
    for (AllocaInst *AI : SharedAllocas) {
      smallString = AI->getName();
      if (smallString == "__promise") {
        assert(fieldNo == 2 && "promise shall be the first field");
      }
      AI->setName(""); // FIXME: use TakeName
      auto index = ConstantInt::get(M->getContext(), ++fieldNo);

      while (!AI->use_empty()) {
        Use &U = *AI->use_begin();
        User *user = U.getUser();
        Value *frame = frameInRamp;
        auto gep =
          GetElementPtrInst::Create(frameTy, frame, { zeroConstant, index },
            smallString, cast<Instruction>(user));
        U.set(gep);
      }
      AI->eraseFromParent();
    }

    // we may end up replacing allocas with gep before frame is defined
    // move definition of frame to the beginning of function
    InstrSetVector coroFrameUses;
    ComputeDefChain(frameInRamp, RampBlocks, coroFrameUses);
    MoveInReverseOrder(coroFrameUses, &*inst_begin(ThisFunction));
  }

  void CreateFrameStruct() {
    SmallVector<Type *, 8> typeArray;

    typeArray.clear();
    typeArray.push_back(resumeFnPtrTy); // 0 res-type
    typeArray.push_back(resumeFnPtrTy); // 1 dtor-type
    typeArray.push_back(int32Ty);       // 2 index

    for (AllocaInst *AI : SharedAllocas) {
      typeArray.push_back(AI->getType()->getElementType());
    }
    frameTy->setBody(typeArray);
  }

  void AnalyzeFunction(Function& F) {
    ReturnInst* Return = nullptr;
    CoroInit = nullptr;
    CoroDelete = nullptr;

    for (auto &BB : F) {
      auto *Term = BB.getTerminator();
      if (auto RI = dyn_cast<ReturnInst>(Term)) {
        assert(!Return && "cannot handle multiple returns yet");
        Return = RI;
      }

      for (auto &I : BB) {
        if (auto AI = dyn_cast<AllocaInst>(&I)) {
          AllAllocas.emplace_back(AI);
          continue;
        }
        if (auto II = dyn_cast<IntrinsicInst>(&I)) {
          switch (II->getIntrinsicID()) {
          default:
            continue;
          case Intrinsic::coro_suspend:
            Suspends.emplace_back(II);
            continue;
          case Intrinsic::coro_init:
            assert(!CoroDelete && "more than one @llvm.coro.init in a coroutine");
            CoroInit = II;
            continue;
          case Intrinsic::coro_delete:
            assert(!CoroDelete && "more than one @llvm.coro.delete in a coroutine");
            CoroDelete = II;
            continue;
          }
        }
      }
    }
    assert(CoroInit && "missing @llvm.coro.init");
    assert(CoroDelete && "missing @llvm.coro.delete");
    assert(Return && "missing ret instruction");
    assert(Suspends.size() != 0 && "cannot handle no suspend coroutines yet");

    // part 1, split out suspends and fill out the branches
    for (auto &S : Suspends) {
      auto CI = cast<ConstantInt>(S.Intrin->getOperand(2));
      APInt Val = CI->getValue();

      if (S.Intrin->getNumUses() == 0) {
        // split up the block
        auto next = S.Intrin->getParent()->splitBasicBlock(
            S.Intrin->getNextNode(), Twine("resume.") + Twine(Val.getZExtValue()));
        S.IfFalse = S.IfTrue = next;
        continue;
      }
      assert(S.Intrin->getNumUses() == 1 && "unexpected number of uses");
      auto BR = cast<BranchInst>(S.Intrin->user_back());
      if (BR->getNumSuccessors() == 1) {
        S.IfFalse = S.IfTrue = BR->getSuccessor(0);
      }
      else {
        S.IfTrue = BR->getSuccessor(0);
        S.IfFalse = BR->getSuccessor(1);
      }
      // if it is final suspend, correct the branch to point only at
      // the cleanup
      if (Val == 0) {
        S.IfTrue = nullptr;
      }
    }

    auto InitBlock = CoroInit->getParent();

    // part 2, split out return instruction into its own block
    auto ReturnBlock = MoveInstructionWithItsUsers("ramp.return", Return);
    RampBlocks.clear();
    RampBlocks.insert(InitBlock);
    RampBlocks.insert(ReturnBlock);

    // part 3, figure out allocas 
    for (auto AI: AllAllocas) {
      for (User* U : AI->users())
        if (auto I = dyn_cast<Instruction>(U))
          if (I->getParent() != InitBlock && I->getParent() != ReturnBlock) {
            SharedAllocas.insert(AI);
            break;
          }
    }
    CreateFrameStruct();
    ReplaceSharedUses();
    // part 4, replace allocas 


    // part 3, duplicate the delete block and extract cleanup part from it
    
  }

  bool runOnCoroutine(Function &F) {
    init(F);

    RemoveNoOptAttribute(F);
    RemoveFakeSuspends(F);
    CreateAuxillaryFunctions();
    AnalyzeFunction(F);

#if 0
    // find the first suspend
    BasicBlock *PrevBlock = CoroDone.SuspendInst->getParent();
    BasicBlock *Candidate = CoroDone.IfFalse;
    DestroyCallBlock = nullptr;

    for (;;) {
      while (Candidate->getSinglePredecessor() == 0) {
        Candidate = CloneBlock(Candidate, PrevBlock);
      }
      RampBlocks.insert(Candidate);
      auto T = Candidate->getTerminator();
      if (SuspendPoint SP{Candidate}) {
        auto T = Candidate->getTerminator();
        BranchInst::Create(ReturnBlock, T);
        // FIXME: check that it is not final suspend
        FirstResume = SP.IfTrue;
        T->eraseFromParent();
        break;
      }
      PrevBlock = Candidate;
      if (auto Next = Candidate->getSingleSuccessor()) {
        Candidate = Next;
        continue;
      }
      auto BI = dyn_cast<BranchInst>(T);
      assert(BI && "Cannot handle any terminator but BranchInst yet");
      if (TryReplaceWithDestroyCall(*BI, 0))
        Candidate = BI->getSuccessor(1);
      else if (TryReplaceWithDestroyCall(*BI, 1))
        Candidate = BI->getSuccessor(0);
      else
        llvm_unreachable("cannot handle branches where one success does not "
                         "lead to cleanup");
    }
#endif
    return true;
  }
};
}

namespace {
#if 1
// incubating new coro-split path

struct CoroSplit2 : public ModulePass, CoroutineInfo {
  static char ID; // Pass identification, replacement for typeid
  CoroSplit2() : ModulePass(ID) {}

  SmallVector<Function *, 8> Coroutines;

  bool runOnModule(Module &M) override {
    CoroutineCommon::PerModuleInit(M);

    bool changed = false;
    for (Function &F : M.getFunctionList())
      if (isCoroutine(F)) {
        changed |= runOnCoroutine(F);
      }
    return changed;
  }
};
#endif
#if 0
  struct CoroSplit2 : public ModulePass, CoroutineCommon {
    static char ID; // Pass identification, replacement for typeid
    CoroSplit2() : ModulePass(ID) {}

    bool runOnModule(Module &M) override {
      CoroutineCommon::PerModuleInit(M);

      bool changed = false;
      for (Function &F : M.getFunctionList())
        if (isCoroutine(F))
          changed |= runOnCoroutine(F);
      return changed;
    }
  };
#endif
#if 0
  class CoroSplit2 : public Inliner {
  public:
    CoroSplit2() : Inliner(ID) {
      initializeSimpleInlinerPass(*PassRegistry::getPassRegistry());
    }
    static char ID; // Pass identification, replacement for typeid

    InlineCost getInlineCost(CallSite CS) override {
      return InlineCost::getNever();
    }

    bool runOnSCC(CallGraphSCC &SCC) override {
      return Inliner::runOnSCC(SCC);
    }
    //void getAnalysisUsage(AnalysisUsage &AU) const override;
  };
#endif
}
char CoroSplit2::ID = 0;
namespace llvm {
INITIALIZE_PASS_BEGIN(
    CoroSplit2, "coro-split2",
    "Split coroutine into ramp/resume/destroy/cleanup functions v2", false,
    false)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(
    CoroSplit2, "coro-split2",
    "Split coroutine into ramp/resume/destroy/cleanup functions v2", false,
    false)

Pass *createCoroSplit2() { return new CoroSplit2(); }
}
