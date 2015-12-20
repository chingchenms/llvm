//===- CoroEarly.cpp - Coroutine Early Function Pass ----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// CoroEarly - FunctionPass ran at extension point EP_EarlyAsPossible
// see ./Coroutines.rst for details
//
//===----------------------------------------------------------------------===//

#include "CoroutineCommon.h"
#include "llvm/Transforms/Coroutines.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/CallSite.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define DEBUG_TYPE "coro-early"

namespace {
  struct AllocaInfo {
    SmallVector<BasicBlock *, 32> DefiningBlocks;
    SmallVector<BasicBlock *, 32> UsingBlocks;

    StoreInst *OnlyStore;
    BasicBlock *OnlyBlock;
    bool OnlyUsedInOneBlock;

    Value *AllocaPointerVal;

    void clear() {
      DefiningBlocks.clear();
      UsingBlocks.clear();
      OnlyStore = nullptr;
      OnlyBlock = nullptr;
      OnlyUsedInOneBlock = true;
      AllocaPointerVal = nullptr;
    }

    /// Scan the uses of the specified alloca, filling in the AllocaInfo used
    /// by the rest of the pass to reason about the uses of this alloca.
    void AnalyzeAlloca(AllocaInst *AI) {
      clear();

      // As we scan the uses of the alloca instruction, keep track of stores,
      // and decide whether all of the loads and stores to the alloca are within
      // the same basic block.
      for (auto UI = AI->user_begin(), E = AI->user_end(); UI != E;) {
        Instruction *User = cast<Instruction>(*UI++);

        if (StoreInst *SI = dyn_cast<StoreInst>(User)) {
          // Remember the basic blocks which define new values for the alloca
          DefiningBlocks.push_back(SI->getParent());
          AllocaPointerVal = SI->getOperand(0);
          OnlyStore = SI;
        }
        else {
          LoadInst *LI = cast<LoadInst>(User);
          // Otherwise it must be a load instruction, keep track of variable
          // reads.
          UsingBlocks.push_back(LI->getParent());
          AllocaPointerVal = LI;
        }

        if (OnlyUsedInOneBlock) {
          if (!OnlyBlock)
            OnlyBlock = User->getParent();
          else if (OnlyBlock != User->getParent())
            OnlyUsedInOneBlock = false;
        }
      }
    }
  };

  using SuspendMap = DenseMap<BasicBlock*, unsigned>;
}

static void insertCoroLoadAndStore(AllocaInst *AI, SuspendMap::iterator SI) {
  BasicBlock* ResumeEdge = SI->getFirst();

  // find the suspend instruction and its number
  BasicBlock* SuspendBlock = ResumeEdge->getSinglePredecessor();
  assert(SuspendBlock && "resume edge block should have a single predecessor");
  BranchInst* BR = cast<BranchInst>(SuspendBlock->getTerminator());
  assert(BR->getNumSuccessors() == 2 && "suspend block should end in cond branch");
  auto II = cast<IntrinsicInst>(BR->getOperand(0));
  assert(II->getIntrinsicID() == Intrinsic::coro_suspend);
  auto SuspendIndex = cast<ConstantInt>(II->getOperand(2));

  Module* M = ResumeEdge->getParent()->getParent();
  auto Int32Ty = IntegerType::get(M->getContext(), 32);
  auto Align = ConstantInt::get(Int32Ty, AI->getAlignment());
  auto Type = AI->getType()->getElementType();
  auto SpillIndex = ConstantInt::get(Int32Ty, SI->getSecond());

  auto StoreFn = Intrinsic::getDeclaration(M, Intrinsic::coro_save, Type);
  auto Load = new LoadInst(AI, "", false, II);
  CallInst::Create(StoreFn, {Load, SuspendIndex, SpillIndex, Align}, "", II);

  auto InsertPt = ResumeEdge->getTerminator();
  auto LoadFn = Intrinsic::getDeclaration(M, Intrinsic::coro_load, Type);
  auto Reload = CallInst::Create(LoadFn, { SuspendIndex, SpillIndex }, "", InsertPt);
  new StoreInst(Reload, AI, InsertPt);

  ++SI->getSecond();
}

static void processAlloca(
  AllocaInst *AI, AllocaInfo &Info,
  const SmallPtrSetImpl<BasicBlock *> &DefBlocks,
  SuspendMap &SuspendBlocks) {

  SmallPtrSet<BasicBlock *, 32> LiveInBlocks;

  // To determine liveness, we must iterate through the predecessors of blocks
  // where the def is live.  Blocks are added to the worklist if we need to
  // check their predecessors.  Start with all the using blocks.
  SmallVector<BasicBlock *, 64> LiveInBlockWorklist(Info.UsingBlocks.begin(),
    Info.UsingBlocks.end());

  // If any of the using blocks is also a definition block, check to see if the
  // definition occurs before or after the use.  If it happens before the use,
  // the value isn't really live-in.
  for (unsigned i = 0, e = LiveInBlockWorklist.size(); i != e; ++i) {
    BasicBlock *BB = LiveInBlockWorklist[i];
    if (!DefBlocks.count(BB))
      continue;

    // Okay, this is a block that both uses and defines the value.  If the first
    // reference to the alloca is a def (store), then we know it isn't live-in.
    for (BasicBlock::iterator I = BB->begin();; ++I) {
      if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
        if (SI->getOperand(1) != AI)
          continue;

        // We found a store to the alloca before a load.  The alloca is not
        // actually live-in here.
        LiveInBlockWorklist[i] = LiveInBlockWorklist.back();
        LiveInBlockWorklist.pop_back();
        --i, --e;
        break;
      }

      if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
        if (LI->getOperand(0) != AI)
          continue;

        // Okay, we found a load before a store to the alloca.  It is actually
        // live into this block.
        break;
      }
    }
  }

  // Now that we have a set of blocks where the phi is live-in, recursively add
  // their predecessors until we find the full region the value is live.
  while (!LiveInBlockWorklist.empty()) {
    BasicBlock *BB = LiveInBlockWorklist.pop_back_val();

    // The block really is live in here, insert it into the set.  If already in
    // the set, then it has already been processed.
    if (!LiveInBlocks.insert(BB).second)
      continue;

    // Since the value is live into BB, it is either defined in a predecessor or
    // live into it to.  Add the preds to the worklist unless they are a
    // defining block.
    for (pred_iterator PI = pred_begin(BB), E = pred_end(BB); PI != E; ++PI) {
      BasicBlock *P = *PI;

      auto Suspend = SuspendBlocks.find(P);
      if (Suspend != SuspendBlocks.end()) {
        insertCoroLoadAndStore(AI, Suspend);
      }

      // The value is not live into a predecessor if it defines the value.
      if (DefBlocks.count(P))
        continue;

      // Otherwise it is, add to the worklist.
      LiveInBlockWorklist.push_back(P);
    }
  }
}

static void removeLifetimeIntrinsicUsers(AllocaInst *AI) {
  // Knowing that this alloca is promotable, we know that it's safe to kill all
  // instructions except for load and store.

  for (auto UI = AI->user_begin(), UE = AI->user_end(); UI != UE;) {
    Instruction *I = cast<Instruction>(*UI);
    ++UI;
    if (isa<LoadInst>(I) || isa<StoreInst>(I))
      continue;

    if (!I->getType()->isVoidTy()) {
      // The only users of this bitcast/GEP instruction are lifetime intrinsics.
      // Follow the use/def chain to erase them now instead of leaving it for
      // dead code elimination later.
      for (auto UUI = I->user_begin(), UUE = I->user_end(); UUI != UUE;) {
        Instruction *Inst = cast<Instruction>(*UUI);
        ++UUI;
        Inst->eraseFromParent();
      }
    }
    I->eraseFromParent();
  }
}

namespace {
  struct CoroEarly : public FunctionPass, CoroutineCommon {
    static char ID; // Pass identification, replacement for typeid
    CoroEarly() : FunctionPass(ID) {}

    bool doInitialization(Module& M) override {
      CoroutineCommon::PerModuleInit(M);
      return false;
    }

    //static int getSuspendNo(IntrinsicInst* intrin) {
    //  assert(intrin->getIntrinsicID() == Intrinsic::coro_suspend);
    //  auto CI = cast<ConstantInt>(intrin->getArgOperand(2));
    //  return CI->getLimitedValue()
    //}

    void handleSuspend(IntrinsicInst *intrin, SuspendMap &SuspendBlocks) {
      // final suspend has no resume edge
      if (cast<ConstantInt>(intrin->getArgOperand(2))->isZero())
        return;

      auto SuspendBlock = intrin->getParent();
      assert(SuspendBlock->getNumUses() == 1 && "unexpected number of uses");
      auto BR = cast<BranchInst>(SuspendBlock->getTerminator());
      auto ResumeEdge = BasicBlock::Create(M->getContext(),
                                           SuspendBlock->getName() + ".resume",
                                           SuspendBlock->getParent());
      BranchInst::Create(BR->getSuccessor(0), ResumeEdge);
      BR->setSuccessor(0, ResumeEdge);
      SuspendBlocks.insert({ ResumeEdge, 0 });
    }

    bool runOnFunction(Function &F) override {
      if (!F.hasFnAttribute(Attribute::Coroutine))
        return false;

      // find allocas (and collect blocks with suspends)
      SmallVector<AllocaInst*, 16> Allocas;
      SuspendMap SuspendBlocks;

      for (auto it = inst_begin(F), end = inst_end(F); it != end;) {
        Instruction &I = *it++;
        if (auto intrin = dyn_cast<IntrinsicInst>(&I)) {
          if (intrin->getIntrinsicID() == Intrinsic::coro_suspend)
            handleSuspend(intrin, SuspendBlocks);
          continue;
        }
        if (auto AI = dyn_cast<AllocaInst>(&I))
          if (isAllocaPromotable(AI))
            Allocas.push_back(AI);
      }

      AllocaInfo Info;
      SmallPtrSet<BasicBlock *, 32> DefBlocks;

      for (auto AI : Allocas) {
        removeLifetimeIntrinsicUsers(AI);
        Info.AnalyzeAlloca(AI);

        if (AI->use_empty())
          continue;

          // Unique the set of defining blocks for efficient lookup.
        DefBlocks.insert(Info.DefiningBlocks.begin(), Info.DefiningBlocks.end());

        processAlloca(AI, Info, DefBlocks, SuspendBlocks);
      }
      return true;
    }

    // We don't modify the funciton much, so we preserve all analyses.
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.setPreservesAll();
    }
  };
}
char CoroEarly::ID = 0;
INITIALIZE_PASS(CoroEarly, "coro-early",
  "Pre-split coroutine transform", false, false)

#define DEBUG_TYPE "coro-early"

namespace {
  struct CoroEarly2 : public FunctionPass, CoroutineCommon {
    static char ID; // Pass identification, replacement for typeid
    CoroEarly2() : FunctionPass(ID) {}

    bool doInitialization(Module& M) override {
      CoroutineCommon::PerModuleInit(M);
      return false;
    }

    void ReplaceCoroDone(IntrinsicInst *intrin) {
      Value *rawFrame = intrin->getArgOperand(0);

      // this could be a coroutine start marker
      // it that is the case, keep it
      if (dyn_cast<ConstantPointerNull>(rawFrame))
        return;

      auto frame = new BitCastInst(rawFrame, anyFramePtrTy, "", intrin);
      auto gepIndex = GetElementPtrInst::Create(
        anyFrameTy, frame, { zeroConstant, zeroConstant }, "", intrin);
      auto index = new LoadInst(gepIndex, "", intrin); // FIXME: alignment
      auto cmp = new ICmpInst(intrin, ICmpInst::ICMP_EQ,
        ConstantPointerNull::get(anyResumeFnPtrTy), index);
      intrin->replaceAllUsesWith(cmp);
      intrin->eraseFromParent();
    }

    void MakeOptnoneUntilCoroSplit(Function& F) {
      if (F.hasFnAttribute(Attribute::OptimizeNone)) {
        // put a marker that the function was originally no opt
        InsertFakeSuspend(ConstantPointerNull::get(bytePtrTy), &*inst_begin(F));
      }
      else {
        // we need to preserve coroutine unchanged until coro-split pass
        F.addFnAttr(Attribute::OptimizeNone);
        F.addFnAttr(Attribute::NoInline);
      }
    }

    bool runOnFunction(Function &F) override {
      bool changed = false;
      bool isCoroutine = false;

      for (auto it = inst_begin(F), end = inst_end(F); it != end;) {
        Instruction &I = *it++;
        if (auto intrin = dyn_cast<IntrinsicInst>(&I)) {
          switch (intrin->getIntrinsicID()) {
          default:
            continue;
          case Intrinsic::coro_done:
            changed = true;
            ReplaceCoroDone(intrin);
            break;
          case Intrinsic::coro_suspend:
            if (!isCoroutine) {
              changed = true;
              isCoroutine = true;
              MakeOptnoneUntilCoroSplit(F);
            }
            break;
            // FIXME: figure out what to do with this two
          case Intrinsic::lifetime_start:
          case Intrinsic::lifetime_end:
            intrin->eraseFromParent();
            break;
          }
        }
      }
      return changed;
    }

    // We don't modify the funciton much, so we preserve all analyses.
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.setPreservesAll();
    }
  };
}
char CoroEarly2::ID = 0;
INITIALIZE_PASS(CoroEarly2, "coro-early2",
  "Pre-split coroutine transform", false, false)

  // pass incubator

namespace {
  struct CoroModuleEarly : public FunctionPass {
    static char ID; // Pass identification, replacement for typeid
    StringRef name;
    CoroModuleEarly() : FunctionPass(ID), name("CoroModuleEarly") {}

    bool doInitialization(Module&) override {
      //errs() << "init: " << name << "\n";
      return false;
    }

    bool doFinalization(Module&) override {
      //errs() << "fini: " << name << "\n";
      return false;
    }

    bool runOnFunction(Function &F) override {
      return false;
    }

    // We don't modify the program, so we preserve all analyses.
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.setPreservesAll();
    }
  };
}
char CoroModuleEarly::ID = 0;
static RegisterPass<CoroModuleEarly> Y2("CoroModuleEarly", "CoroModuleEarly Pass");

namespace {
  struct CoroScalarLate : public FunctionPass {
    static char ID; // Pass identification, replacement for typeid
    StringRef name;
    CoroScalarLate() : FunctionPass(ID), name("CoroScalarLate") {}

    bool doInitialization(Module&) override {
      //errs() << "init: " << name << "\n";
      return false;
    }

    bool doFinalization(Module&) override {
      //errs() << "fini: " << name << "\n";
      return false;
    }

    bool runOnFunction(Function &F) override {
      return false;
    }

    // We don't modify the program, so we preserve all analyses.
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.setPreservesAll();
    }
  };
}
char CoroScalarLate::ID = 0;
static RegisterPass<CoroScalarLate> Y3("CoroScalarLate", "CoroScalarLate Pass");

namespace {
  struct CoroLast : public FunctionPass {
    static char ID; // Pass identification, replacement for typeid
    StringRef name;
    CoroLast() : FunctionPass(ID), name("CoroLast") {}

    bool doInitialization(Module&) override {
      //errs() << "init: " << name << "\n";
      return false;
    }

    bool doFinalization(Module&) override {
      //errs() << "fini: " << name << "\n";
      return false;
    }

    bool runOnFunction(Function &F) override {
      return false;
    }

    // We don't modify the program, so we preserve all analyses.
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.setPreservesAll();
    }
  };
}
char CoroLast::ID = 0;
static RegisterPass<CoroLast> Y4("CoroLast", "CoroLast Pass");

namespace {
  struct CoroOpt0 : public FunctionPass {
    static char ID; // Pass identification, replacement for typeid
    StringRef name;
    CoroOpt0() : FunctionPass(ID), name("coro-opt0") {}

    bool doInitialization(Module&) override {
      //errs() << "init: " << name << "\n";
      return false;
    }

    bool doFinalization(Module&) override {
      //errs() << "fini: " << name << "\n";
      return false;
    }

    bool runOnFunction(Function &F) override {
      return false;
    }

    // We don't modify the program, so we preserve all analyses.
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.setPreservesAll();
    }
  };
}
char CoroOpt0::ID = 0;
static RegisterPass<CoroOpt0> Y5("CoroOpt0", "CoroOpt0 Pass");

Pass *llvm::createCoroEarlyPass() {
  //return new CoroLast();
  return new CoroEarly();
} // must be a function pass

Pass *llvm::createCoroModuleEarlyPass() {
    return createCoroSplitPass();
  //return new CoroModuleEarly();
}
Pass *llvm::createCoroScalarLatePass() {
  //  return createPrintModulePass(outs());
    return createCoroHeapElidePass(); 
  //return new CoroScalarLate();
}
Pass *llvm::createCoroLastPass() {
    return createCoroCleanupPass();
  //return new CoroLast();
}
Pass *llvm::createCoroOnOpt0() { return new CoroOpt0(); }

namespace {
  // inline little things in a coroutine, like a void or bool
  // function with only a ret instruction returning a constant

  struct CoroPreSplit : public ModulePass, CoroutineCommon {
    static char ID; // Pass identification, replacement for typeid
    CoroPreSplit() : ModulePass(ID) {}

    SmallPtrSet<Function*, 8> RampFunctions;

    void ReplaceIfEmpty(Instruction& I, const Function & F) {
      if (F.size() == 1)
        if (F.getEntryBlock().size() == 1)
          if (isa<ReturnInst>(&*inst_begin(F)))
            I.eraseFromParent();
    }

    void ReplaceIfConstant(Instruction& I, const Function& F) {
      if (F.size() == 1)
        if (F.getEntryBlock().size() == 1)
          if (auto RetInst = dyn_cast<ReturnInst>(&*inst_begin(F)))
            if (auto ConstVal = dyn_cast<ConstantInt>(RetInst->getReturnValue())) {
              I.replaceAllUsesWith(ConstVal);
              I.eraseFromParent();
            }
    }

    void ReplaceSuspendIfEmpty(IntrinsicInst& I) {
      Value *op = I.getArgOperand(1);
      while (const ConstantExpr *CE = dyn_cast<ConstantExpr>(op)) {
        if (!CE->isCast())
          break;
        // Look through the bitcast
        op = cast<ConstantExpr>(op)->getOperand(0);
      }
      Function* fn = cast<Function>(op);
      RampFunctions.insert(fn);
      if (fn->size() != 1)
        return;

      BasicBlock& B = fn->back();
      if (B.size() < 2)
        return;

      auto LastInstr = B.getTerminator()->getPrevNode();
      CallInst* Call = dyn_cast<CallInst>(LastInstr);
      Function* awaitSuspendFn = Call->getCalledFunction();
      if (!awaitSuspendFn)
        return;
      if (awaitSuspendFn->isDeclaration())
        return;

      if (awaitSuspendFn->size() == 1)
        if (awaitSuspendFn->getEntryBlock().size() == 1)
          if (isa<ReturnInst>(&*inst_begin(*awaitSuspendFn))) 
            I.setOperand(0, ConstantPointerNull::get(bytePtrTy));
    }

    bool runOnCoroutine(Function& F) {
      RemoveNoOptAttribute(F);

      Function* coroKill = 
        Intrinsic::getDeclaration(M, Intrinsic::coro_kill2, { int32Ty });
      coroKill->dump();

      Function* coroSave =
        Intrinsic::getDeclaration(M, Intrinsic::coro_save, { int32Ty });
      coroSave->dump();

      Function* coroLoad =
        Intrinsic::getDeclaration(M, Intrinsic::coro_load, { int32Ty });
      coroLoad->dump();

      for (auto it = inst_begin(F), end = inst_end(F); it != end;) {
        Instruction& I = *it++;

        CallSite CS(cast<Value>(&I));
        if (!CS)
          continue;
        const Function *Callee = CS.getCalledFunction();
        if (!Callee)
          continue; // indirect call, nothing to do
        if (Callee->isIntrinsic()) {
          if (Callee->getIntrinsicID() == Intrinsic::coro_suspend)
            ReplaceSuspendIfEmpty(*cast<IntrinsicInst>(&I));
          continue;
        }

        if (Callee->isDeclaration())
          continue;

        // only inline void and bool returning functions
        const Type *RetType = Callee->getReturnType();
        if (RetType == voidTy) ReplaceIfEmpty(I, *Callee);
        else if (RetType == boolTy) ReplaceIfConstant(I, *Callee);
      }
      return true;
    }

    void handleRampFunction(Function& F) {
      for (auto it = inst_begin(F), end = inst_end(F); it != end;) {
        Instruction& I = *it++;
        if (auto CS = CallSite(&I)) {
          InlineFunctionInfo IFI;
          InlineFunction(CS, IFI);
        }
      }
    }

    bool runOnModule(Module &M) override {
      CoroutineCommon::PerModuleInit(M);
      RampFunctions.clear();

      bool changed = false;
      for (Function &F : M.getFunctionList())
        if (isCoroutine(F))
          changed |= runOnCoroutine(F);

      for (Function* F : RampFunctions) {
        handleRampFunction(*F);
        changed = true;
      }
      return changed;
    }
  };
}
char CoroPreSplit::ID = 0;
static RegisterPass<CoroPreSplit> Y6("CoroPreSplit", "inline little things");
namespace llvm {
  Pass *createCoroPreSplit() { return new CoroPreSplit(); }
}
