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

#include "llvm/IR/Module.h"
#include "llvm/IR/CallSite.h"
#include "llvm/Support/Casting.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define DEBUG_TYPE "coro-early"

namespace {
  struct CoroEarly : public FunctionPass, CoroutineCommon {
    static char ID; // Pass identification, replacement for typeid
    CoroEarly() : FunctionPass(ID) {}

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
char CoroEarly::ID = 0;
INITIALIZE_PASS(CoroEarly, "coro-early",
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

    bool runOnCoroutine(Function& F) {
      RemoveNoOptAttribute(F);
      for (auto it = inst_begin(F), end = inst_end(F); it != end;) {
        Instruction& I = *it++;
        CallSite CS(cast<Value>(&I));
        if (!CS)
          continue;
        const Function *Callee = CS.getCalledFunction();
        if (!Callee)
          continue; // indirect call, nothing to do
        if (Callee->isDeclaration())
          continue;

        // only inline void and bool returning functions
        const Type *RetType = Callee->getReturnType();
        if (RetType == voidTy) ReplaceIfEmpty(I, *Callee);
        else if (RetType == boolTy) ReplaceIfConstant(I, *Callee);
      }
      return false;
    }

    bool runOnModule(Module &M) override {
      CoroutineCommon::PerModuleInit(M);

      bool changed = false;
      for (Function &F : M.getFunctionList())
        if (isCoroutine(F))
          changed |= runOnCoroutine(F);
      return changed;
    }
  };
}
char CoroPreSplit::ID = 0;
static RegisterPass<CoroPreSplit> Y6("CoroPreSplit", "inline little things");
namespace llvm {
  Pass *createCoroPreSplit() { return new CoroPreSplit(); }
}
