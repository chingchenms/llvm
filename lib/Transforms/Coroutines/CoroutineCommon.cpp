//===- CoroutineCommon.cpp - utilities for coroutine passes ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file provides implementation of common utilities used to implement
/// coroutine passes.
///
//===----------------------------------------------------------------------===//

#include "CoroutineCommon.h"
#include "llvm/Transforms/Utils/CodeExtractor.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/Coroutines.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Module.h"

using namespace llvm;
using namespace llvm::legacy;

void CoroCommon::replaceCoroFree(Value* FramePtr, Value* Replacement) {
  SmallVector<CoroFreeInst*, 4> CoroFrees;
  for (User* U : FramePtr->users())
    if (auto CF = dyn_cast<CoroFreeInst>(U))
      CoroFrees.push_back(CF);

  if (CoroFrees.empty())
    return;

  if (nullptr == Replacement)
    Replacement = ConstantPointerNull::get(
        cast<PointerType>(CoroFrees.front()->getType()));

  for (CoroFreeInst* CF : CoroFrees) {
    CF->replaceAllUsesWith(Replacement);
    CF->eraseFromParent();
  }
}

void CoroCommon::removeLifetimeIntrinsics(Function &F) {
  for (auto it = inst_begin(F), end = inst_end(F); it != end;) {
    Instruction& I = *it++;
    if (auto II = dyn_cast<IntrinsicInst>(&I))
      switch (II->getIntrinsicID()) {
      default:
        continue;
      case Intrinsic::lifetime_start:
      case Intrinsic::lifetime_end:
        II->eraseFromParent();
        break;
      }
  }
}

BasicBlock *CoroCommon::splitBlockIfNotFirst(Instruction *I,
                                             const Twine &Name) {
  auto BB = I->getParent();
  if (&*BB->begin() == I) {
    if (BB->getSinglePredecessor()) {
      BB->setName(Name);
      return BB;
    }
  }
  return BB->splitBasicBlock(I, Name);
}

static void UpdateCGN(CallGraph &CG, CallGraphNode *Node) {
  Function *F = Node->getFunction();

  // Look for calls by this function.
  for (Instruction &I : instructions(F))
    if (CallSite CS = CallSite(cast<Value>(&I))) {
      const Function *Callee = CS.getCalledFunction();
      if (!Callee || !Intrinsic::isLeaf(Callee->getIntrinsicID()))
        // Indirect calls of intrinsics are not allowed so no need to check.
        // We can be more precise here by using TargetArg returned by
        // Intrinsic::isLeaf.
        Node->addCalledFunction(CS, CG.getCallsExternalNode());
      else if (!Callee->isIntrinsic())
        Node->addCalledFunction(CS, CG.getOrInsertFunction(Callee));
    }
}

void CoroCommon::updateCallGraph(Function &Caller, ArrayRef<Function *> Funcs,
  CallGraph &CG, CallGraphSCC &SCC) {
  auto CallerNode = CG[&Caller];
  CallerNode->removeAllCalledFunctions();
  UpdateCGN(CG, CallerNode);

  SmallVector<CallGraphNode*, 8> Nodes(SCC.begin(), SCC.end());

  for (Function* F : Funcs) {
    CallGraphNode* Callee = CG.getOrInsertFunction(F);
    Nodes.push_back(Callee);
    UpdateCGN(CG, Callee);
  }

  SCC.initialize(&*Nodes.begin(), &*Nodes.end());
}


void CoroCommon::constantFoldUsers(Constant* Value) {
  SmallPtrSet<Instruction*, 16> WorkList;
  for (User *U : Value->users())
    WorkList.insert(cast<Instruction>(U));

  if (WorkList.empty())
    return;

  Instruction *FirstInstr = *WorkList.begin();
  Function* F = FirstInstr->getParent()->getParent();
  const DataLayout &DL = F->getParent()->getDataLayout();

  do {
    Instruction *I = *WorkList.begin();
    WorkList.erase(I); // Get an element from the worklist...

    if (!I->use_empty())                 // Don't muck with dead instructions...
      if (Constant *C = ConstantFoldInstruction(I, DL)) {
        // Add all of the users of this instruction to the worklist, they might
        // be constant propagatable now...
        for (User *U : I->users())
          WorkList.insert(cast<Instruction>(U));

        // Replace all of the uses of a variable with uses of the constant.
        I->replaceAllUsesWith(C);

        // Remove the dead instruction.
        WorkList.erase(I);
        I->eraseFromParent();
      }
  } while (!WorkList.empty());
}

struct ClearVisitor {
  template <typename Container> void operator()(Container &C, StringRef) {
    C.clear();
  }
  template <typename Instr> void operator()(Instr *&I, StringRef) {
    I = nullptr;
  }
  void operator()(bool &Value, StringRef) {
    Value = false;
  }
};

void llvm::CoroutineShape::clear() { reflect(ClearVisitor{}); }

struct DumpVisitor {
  template <typename Container>
  void operator()(Container const &C, StringRef Name) {
    if (C.empty())
      return;
    dbgs() << Name << ":\n";
    for (auto *Val : C) {
      dbgs() << "    ";
      Val->dump();
    }
  }
  template <typename Instr> void operator()(Instr *I, StringRef Name) {
    dbgs() << Name << ":";
    I->dump();
  }
  void operator()(bool Value, StringRef Name) {
    dbgs() << Name << ":" << (Value ? "true" : "false");
  }
};

void llvm::CoroutineShape::dump() { reflect(DumpVisitor{}); }

void llvm::CoroutineShape::buildFrom(Function &F) {
  clear();
  for (Instruction& I : instructions(F)) {
    if (auto RI = dyn_cast<ReturnInst>(&I))
      Return.push_back(RI);
    else if (auto II = dyn_cast<IntrinsicInst>(&I)) {
      switch (II->getIntrinsicID()) {
      default:
        continue;
      case Intrinsic::coro_size:
        CoroSize.push_back(cast<CoroSizeInst>(II));
        break;
      case Intrinsic::coro_frame:
        CoroFrame.push_back(cast<CoroFrameInst>(II));
        break;
      case Intrinsic::coro_alloc:
        CoroAlloc.push_back(cast<CoroAllocInst>(II));
        break;
      case Intrinsic::coro_suspend:
        CoroSuspend.push_back(cast<CoroSuspendInst>(II));
        if (CoroSuspend.back()->isFinal()) {
          HasFinalSuspend = true;
          if (CoroSuspend.size() > 1) {
            assert(!CoroSuspend.front()->isFinal() &&
                   "Only one suspend point can be marked as final");
            std::swap(CoroSuspend.front(), CoroSuspend.back());
          }
        }
        break;
      case Intrinsic::coro_begin: {
        auto CB = cast<CoroBeginInst>(II);
        CB->addAttribute(0, Attribute::NonNull);
        CB->addAttribute(0, Attribute::NoAlias);
        if (CB->getInfo().isPreSplit())
          CoroBegin.push_back(CB);
        break;
      }
      case Intrinsic::coro_free:
        CoroFree.push_back(cast<CoroFreeInst>(II));
        break;
      case Intrinsic::coro_end:
        CoroEnd.push_back(cast<CoroEndInst>(II));
        if (CoroEnd.back()->isFinal()) {
          if (CoroEnd.size() > 1) {
            assert(!CoroEnd.front()->isFinal() &&
              "Only one suspend point can be marked as final");
            std::swap(CoroEnd.front(), CoroEnd.back());
          }
        }
        break;
      }
    }
  }
  assert(CoroBegin.size() == 1 &&
    "coroutine should have exactly one defining @llvm.coro.begin");
  assert(CoroAlloc.size() == 1 &&
    "coroutine should have exactly one @llvm.coro.alloc");
}

void llvm::initializeCoroutines(PassRegistry &registry) {
//  initializeCoroOutlinePass(registry);
  initializeCoroEarlyPass(registry);
  initializeCoroElidePass(registry);
  initializeCoroCleanupPass(registry);
  initializeCoroSplitPass(registry);
}

static bool g_VerifyEach = true;

static void addPass(legacy::PassManagerBase &PM, Pass *P) {
  // Add the pass to the pass manager...
  PM.add(P);

  // If we are verifying all of the intermediate steps, add the verifier...
  if (g_VerifyEach)
    PM.add(createVerifierPass());
}

static void addCoroutineOpt0Passes(const PassManagerBuilder &Builder,
                                   PassManagerBase &PM) {
  // addPass(PM, createCoroEarlyPass());
  // addPass(PM, createCoroSplitPass());
  // addPass(PM, createCoroLatePass());
}

static void addCoroutineEarlyPasses(const PassManagerBuilder &Builder,
                                    PassManagerBase &PM) {
  addPass(PM, createCoroEarlyPass());
  // addPass(PM, createCoroSplitPass());
  // addPass(PM, createCoroLatePass());
}

static void addCoroutineModuleEarlyPasses(const PassManagerBuilder &Builder,
                                          PassManagerBase &PM) {
  // addPass(PM, createCoroOutlinePass());
  // addPass(PM, createCoroSplitPass());
  // addPass(PM, createCoroLatePass());
}

static void addCoroutineScalarOptimizerPasses(const PassManagerBuilder &Builder,
  PassManagerBase &PM) {
  addPass(PM, createCoroElidePass());
}

static void addCoroutineSCCPasses(const PassManagerBuilder &Builder,
                                  PassManagerBase &PM) {
  if (Builder.OptLevel > 0) {
    //addPass(PM, createCoroInlinePass());
    addPass(PM, createCoroSplitPass());
  }
}

static void addCoroutineOptimizerLastPasses(const PassManagerBuilder &Builder,
  PassManagerBase &PM) {
  addPass(PM, createCoroCleanupPass());
}

void llvm::addCoroutinePassesToExtensionPoints(PassManagerBuilder &Builder,
                                               bool VerifyEach) {
  g_VerifyEach = VerifyEach;

  Builder.addExtension(PassManagerBuilder::EP_EarlyAsPossible,
    addCoroutineEarlyPasses);
  Builder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                       addCoroutineOpt0Passes);
  Builder.addExtension(PassManagerBuilder::EP_ModuleOptimizerEarly,
    addCoroutineModuleEarlyPasses);
  Builder.addExtension(PassManagerBuilder::EP_CGSCCOptimizerLate,
    addCoroutineSCCPasses);
  Builder.addExtension(PassManagerBuilder::EP_ScalarOptimizerLate,
    addCoroutineScalarOptimizerPasses);
  Builder.addExtension(PassManagerBuilder::EP_OptimizerLast,
    addCoroutineOptimizerLastPasses);
}
