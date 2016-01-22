//===- CoroElide.cpp - Coroutine Frame Allocation Elision Pass ------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements coro-elide pass that replaces dynamic allocation 
// of coroutine frame with alloca and replaces calls to @llvm.coro.resume and
// @llvm.coro.destroy with direct calls to coroutine sub-functions
// see ./Coroutines.rst for details
//
//===----------------------------------------------------------------------===//

#include "CoroutineCommon.h"
#include "llvm/Transforms/Coroutines.h"
#include "llvm/Analysis/CallGraphSCCPass.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Pass.h"
#include "llvm/PassSupport.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define DEBUG_TYPE "coro-elide"

STATISTIC(CoroHeapElideCounter, "Number of heap elision performed");

namespace {

struct CoroHeapElide : FunctionPass, CoroutineCommon {
  static char ID; // Pass identification, replacement for typeid
  bool moduleInitialized = false;

  CoroHeapElide() : FunctionPass(ID) {}

  // This function walks up from an operand to @llvm.coro.resume or
  // @llvm.coro.destroy to see if it hits a @llvm.coro.init
  // somewhere in the definition change
  static IntrinsicInst *FindDefiningCoroInit(Value *op) {
    for (;;) {
      if (IntrinsicInst *intrin = dyn_cast<IntrinsicInst>(op)) {
        if (intrin->getIntrinsicID() == Intrinsic::coro_init)
          return intrin;

        return nullptr;
      }

      if (isa<Argument>(op))
        return nullptr;

      if (auto gep = dyn_cast<GetElementPtrInst>(op)) {
        op = gep->getPointerOperand();
        // TODO: sanity testing
        continue;
      }
      if (auto bitcast = dyn_cast<BitCastInst>(op)) {
        op = bitcast->getOperand(0);
        // TODO: sanity testing
        continue;
      }
      return nullptr;
    }
  }

  // Database - builds up an information about all
  // coroutine calls from a given function

  struct Database {
    struct InlinedCoroutine {
      IntrinsicInst *CoroInit = nullptr;
      Function *ResumeFn = nullptr;
      StoreInst *CleanupFn = nullptr;
      unsigned StoreCount = 0;
      SmallVector<IntrinsicInst *, 4> Resumes;
      SmallVector<IntrinsicInst *, 4> Destroys;

      InlinedCoroutine(IntrinsicInst *intrin) : CoroInit(intrin) {}

      StructType *getFrameType() {
        Function *func = ResumeFn;
        FunctionType *ft =
            cast<FunctionType>(func->getType()->getElementType());
        assert(ft->getNumParams() == 1 &&
               "expected exactly one parameter in destroyFn");
        PointerType *argType = cast<PointerType>(ft->getParamType(0));
        return cast<StructType>(argType->getElementType());
      }
      StringRef getRampName() {
        Function *func = ResumeFn;
        return func->getName().drop_back(sizeof(".resume") - 1);
      }
    };
    SmallVector<InlinedCoroutine, 4> data;

    InlinedCoroutine &get(IntrinsicInst *coroInit) {
      for (auto &item : data)
        if (item.CoroInit == coroInit)
          return item;
      AddCoroInit(coroInit);
      return data.back();
    }

    void AddCoroInit(IntrinsicInst *coroInit) { data.emplace_back(coroInit); }

    void AddResumeOrDestroy(IntrinsicInst *coroInit, IntrinsicInst *I) {
      auto &item = get(coroInit);
      if (I->getIntrinsicID() == Intrinsic::coro_destroy)
        item.Destroys.push_back(I);
      else
        item.Resumes.push_back(I);
    }

    void AddStore(IntrinsicInst *coroInit, Function *Func) {
      InlinedCoroutine &I = get(coroInit);
      if (Func->getName().endswith(".resume")) {
        I.ResumeFn = Func;
      }
      ++I.StoreCount;
    }

    Database(Function &F) {
      // scan the function for a store that sets a destroy function
      // if we elided allocation, we need to replace that store
      // with a store of an address of a cleanup function instead
      for (Instruction &I : instructions(F)) {
        if (StoreInst *S = dyn_cast<StoreInst>(&I)) {
          if (Function *func = dyn_cast<Function>(S->getOperand(0))) {
            IntrinsicInst *coroInit = FindDefiningCoroInit(S->getOperand(1));
            if (coroInit == nullptr)
              continue;

            AddStore(coroInit, func);
          }
        } else if (IntrinsicInst *intrin = dyn_cast<IntrinsicInst>(&I)) {
          switch (intrin->getIntrinsicID()) {
          default:
            continue;
          case Intrinsic::coro_destroy:
          case Intrinsic::coro_resume: {
            IntrinsicInst *coroInit =
                FindDefiningCoroInit(intrin->getOperand(0));
            if (coroInit == nullptr)
              continue;
            AddResumeOrDestroy(coroInit, intrin);
            break;
          }
          case Intrinsic::coro_init:
            AddCoroInit(intrin);
            break;
          }
        }
      }
    }
  };

  /// Pass Manager itself does not invalidate any analysis info.
  //void getAnalysisUsage(AnalysisUsage &Info) const override {
  //  // CGPassManager walks SCC and it needs CallGraph.
  //  Info.setPreservesAll();
  //  Info.addRequired<CallGraphWrapperPass>();
  //}

  void ReplaceWithDirectCalls(//CallGraph &CG, CallGraphNode &CGN,
                              SmallVector<IntrinsicInst *, 4> &v,
                              Function *func) {
    FunctionType *ft = cast<FunctionType>(func->getType()->getElementType());
    PointerType *argType = cast<PointerType>(ft->getParamType(0));

    for (IntrinsicInst *intrin : v) {
      auto bitCast =
          new BitCastInst(intrin->getArgOperand(0), argType, "", intrin);
      auto call = CallInst::Create(func, bitCast, "", intrin);
      call->setCallingConv(CallingConv::Fast);
      //CGN.addCalledFunction(CallSite(call), CG[func]);
      intrin->eraseFromParent();
    }
  }

  SmallString<64> smallString;

  Function *getFunc(Module *M, Twine Name) {
    smallString.clear();
    auto value = M->getNamedValue(Name.toStringRef(smallString));
    assert(value && "coroutine auxillary function not found");
    return cast<Function>(value);
  }

  bool tryElide(Function &F) {
    bool changed = false;
    Database db(F);
    for (auto &item : db.data) {
      assert(item.ResumeFn &&
             "missing ResumeFn store after @llvm.coro.init");

      const bool noDestroys = item.Destroys.empty();
      const bool noResumes = item.Resumes.empty();

      if (noDestroys && noResumes)
        continue;

      StringRef rampName = item.getRampName();
      Module *M = F.getParent();
      Function *cleanupFn = getFunc(M, rampName + ".cleanup");

      // FIXME: check for escapes, moves,
      if (!noDestroys && rampName != F.getName()) {
        auto InsertPt = inst_begin(F);
        auto allocaFrame =
            new AllocaInst(item.getFrameType(), "elided.frame", &*InsertPt);

        auto vAllocaFrame = new BitCastInst(allocaFrame, bytePtrTy,
                                            "elided.vFrame", &*InsertPt);

        auto CoroElide = GetCoroElide(item.CoroInit);

        item.CoroInit->replaceAllUsesWith(vAllocaFrame);
        item.CoroInit->eraseFromParent();

        CoroElide->replaceAllUsesWith(vAllocaFrame);
        CoroElide->eraseFromParent();
      }

      // FIXME: remove obsolete comment 1/21/2016
      // FIXME: handle case when we are looking at the coroutine itself
      // that destroys itself (like in case with optional/expected)
      ReplaceWithDirectCalls(//CG, CGN, 
        item.Resumes, item.ResumeFn);
      ReplaceWithDirectCalls(//CG, CGN, 
        item.Destroys, cleanupFn);
      replaceAllCoroDone(F);

      changed = true;
    }
    return changed;
  }

  void replaceAllCoroDone(Function &F) {
    for (auto it = inst_begin(F), end = inst_end(F); it != end;) {
      Instruction &I = *it++;
      if (IntrinsicInst *intrin = dyn_cast<IntrinsicInst>(&I))
        if (intrin->getIntrinsicID() == Intrinsic::coro_done)
          ReplaceCoroDone(intrin);
    }
  }

#if 0
  bool runOnModule(Module& M) override {
    CoroutineCommon::PerModuleInit(M);

    bool changed = false;
    for (Function &F : M.getFunctionList()) {
      changed |= runOnFunction(F);
    }
    return changed;
  }
#endif
  bool doInitialization(Module &M) override {
    if (moduleInitialized)
      return false;
    CoroutineCommon::PerModuleInit(M);
    moduleInitialized = true;
    return false;
  }

  static bool hasResumeOrDestroy(Function &F) {
    for (auto& I : instructions(F)) {
      if (auto intrin = dyn_cast<IntrinsicInst>(&I)) {
        switch (intrin->getIntrinsicID()) {
        default:
          continue;
        case Intrinsic::coro_resume:
        case Intrinsic::coro_destroy:
          return true;
        }
      }
    }
    return false;
  }

  bool runOnFunction(Function &F) override {
    bool changed = false;

    if (hasResumeOrDestroy(F)) {
      if (tryElide(F)) {
        ++CoroHeapElideCounter;
        changed = true;
      }
    }
    return changed;
  }

#if 0
  bool runOnSCC(CallGraphSCC &SCC) override {
    bool changed = false;
    for (CallGraphNode *Node : SCC) {
      Function *F = Node->getFunction();
      if (!F) continue;
      doInitialization(*F->getParent());

      changed |= runOnFunction(*F);
    }
    return changed;
  }
#endif
  void RemoveAllAllocationRelatedThings(//CallGraphNode& CGN, 
    Value *alloc) {
    CallInst *NeedlessAllocateCall = cast<CallInst>(alloc);
    // TODO: clean up allocation code better
    //CGN.removeCallEdgeFor(NeedlessAllocateCall);
    NeedlessAllocateCall->eraseFromParent();
  }
};
}

char CoroHeapElide::ID = 0;
INITIALIZE_PASS_BEGIN(
    CoroHeapElide, "coro-elide",
    "Coroutine frame allocation elision and indirect calls replacement", false,
    false)
  //INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(CoroSplit)
INITIALIZE_PASS_END(
    CoroHeapElide, "coro-elide",
    "Coroutine frame allocation elision and indirect calls replacement", false,
    false)

Pass *llvm::createCoroHeapElidePass() { return new CoroHeapElide(); }
