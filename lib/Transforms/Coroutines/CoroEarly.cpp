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
#include "llvm/IR/Module.h"
#include "llvm/IR/CFG.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/IR/InstIterator.h"

using namespace llvm;

#define DEBUG_TYPE "coro-early"

namespace {
  /// Holds all structural Coroutine Intrinsics for a particular function
  struct CoroutineShape {
    TinyPtrVector<CoroInitInst*> CoroInit;
    TinyPtrVector<CoroAllocInst*> CoroAlloc;
    TinyPtrVector<CoroBeginInst*> CoroBegin;
    TinyPtrVector<CoroEndInst*> CoroEndFinal;
    TinyPtrVector<CoroEndInst*> CoroEndUnwind;

    SmallVector<CoroSizeInst*, 2> CoroSize;
    SmallVector<CoroFreeInst*, 2> CoroFree;
    SmallVector<CoroFrameInst*, 4> CoroFrame;
    SmallVector<CoroSuspendInst*, 4> CoroSuspend;

    TinyPtrVector<ReturnInst*> Return;

    explicit CoroutineShape(Function &F);
    explicit CoroutineShape(CoroInitInst *CoroInit)
        : CoroutineShape(*CoroInit->getParent()->getParent()) {}
  };
}

CoroutineShape::CoroutineShape(Function &F) {
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
      case Intrinsic::coro_alloc:
        CoroAlloc.push_back(cast<CoroAllocInst>(II));
        break;
      case Intrinsic::coro_init: {
        auto CI = cast<CoroInitInst>(II);
        if (CI->isPreSplit())
          CoroInit.push_back(CI);
        break;
      }
      case Intrinsic::coro_begin:
        CoroBegin.push_back(cast<CoroBeginInst>(II));
        break;
      case Intrinsic::coro_free:
        CoroFree.push_back(cast<CoroFreeInst>(II));
        break;
      case Intrinsic::coro_end:
        auto CE = cast<CoroEndInst>(II);
        if (CE->isFallthrough())
          CoroEndFinal.push_back(CE);
        else
          CoroEndUnwind.push_back(CE);
        break;
      }
    }
  }
  assert(CoroInit.size() != 1 &&
         "coroutine should have exactly one defining @llvm.coro.init");
  assert(CoroBegin.size() != 1 &&
         "coroutine should have exactly one @llvm.coro.begin");
  assert(CoroAlloc.size() != 1 &&
         "coroutine should have exactly one @llvm.coro.alloc");
  assert(CoroEndFinal.size() != 1 &&
    "coroutine should have exactly one @llvm.coro.end(falthrough = true)");
}

static BasicBlock* splitBlockIfNotFirst(Instruction* I, StringRef Name) {
  auto BB = I->getParent();
  if (&*BB->begin() == I) {
    BB->setName(Name);
    return BB;
  }

  return BB->splitBasicBlock(I, Name);
}

#if 0
static void outlineAllocPart(CoroutineShape& S, CoroPartExtractor& E) {
  auto Start = splitBlockIfNotFirst(S.CoroAlloc, "AllocPart");
  auto End = splitBlockIfNotFirst(S.CoroInit, "InitBB");
  E.createFunction(Start, End);
}

static void outlineFreePart(CoroutineShape& S, CoroPartExtractor& E) {
  auto Start = splitBlockIfNotFirst(S.CoroFree, "FreePart");
  auto End = splitBlockIfNotFirst(S.CoroEnd, "EndBB");
  E.createFunction(Start, End);
}

static void outlinePrepPart(CoroutineShape const& S, CoroPartExtractor& E) {
  auto Start = splitBlockIfNotFirst(S.CoroInit->getNextNode(), "PrepPart");
  auto End = splitBlockIfNotFirst(S.CoroFork, "ForkBB");
  E.createFunction(Start, End);
}

static void outlineReturnPart(CoroutineShape const& S, CoroPartExtractor& E) {
  Function* F = S.CoroInit->getParent()->getParent();

  // coroutine should have a single return instruction
  ReturnInst* RetInstr = nullptr;
  for (BasicBlock& B : *F) {
    if (auto RI = dyn_cast<ReturnInst>(B.getTerminator())) {
      assert(!RetInstr && "multiple ReturnInst in the coroutine");
      RetInstr = RI;
      break;
    }
  }
  assert(RetInstr && "Coroutine must have a return block");

  auto Start = splitBlockIfNotFirst(S.CoroEnd, "RetPart");
  auto End = splitBlockIfNotFirst(RetInstr, "RetBB");

  E.createFunction(Start, End);
}

static void outlineCoroutineParts(CoroutineShape& S) {
  Function& F = *S.CoroInit->getParent()->getParent();
  CoroCommon::removeLifetimeIntrinsics(F); // for now
  DEBUG(dbgs() << "Processing Coroutine: " << F.getName() << "\n");
  DEBUG(S.CoroAlloc->dump());
  DEBUG(S.CoroInit->dump());

  CoroPartExtractor Extractor;
  outlineAllocPart(S, Extractor);
  outlinePrepPart(S, Extractor);
  outlineFreePart(S, Extractor);
  outlineReturnPart(S, Extractor);
}

static void replaceEmulatedIntrinsicsWithRealOnes(Module& M) {
  for (Function& F : M) {
    for (Instruction &I : instructions(&F)) {
      if (auto CI = dyn_cast<CallInst>(&I)) {
        if (auto F = CI->getCalledFunction()) {
          const auto id = StringSwitch<unsigned>(F->getName())
            .Case("CoroAlloc", Intrinsic::coro_alloc)
            .Case("CoroInit", Intrinsic::coro_init)
            .Case("CoroStart", Intrinsic::coro_start)
            .Case("CoroFree", Intrinsic::coro_free)
            .Case("CoroEnd", Intrinsic::coro_end)
            .Default(0);
          switch (id) {
          case Intrinsic::coro_alloc:
            CoroAllocInst::replaceCall(CI);
            break;
          }
        }
      }
    }
  }
}
#endif
//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

namespace {
struct CoroEarly : public FunctionPass {
  static char ID; // Pass identification, replacement for typeid
  CoroEarly() : FunctionPass(ID) {}

  bool doInitialization(Module& M) override {
    SmallVector<CoroInitInst*, 8> Coroutines;
    // replaceEmulatedIntrinsicsWithRealOnes(M);
    Function *CoroInitFn = Intrinsic::getDeclaration(&M, Intrinsic::coro_init);

    // Find all pre-split coroutines.
    for (User* U : CoroInitFn->users())
      if (auto CoroInit = dyn_cast<CoroInitInst>(U))
        if (CoroInit->isPreSplit())
          Coroutines.push_back(CoroInit);

    // Outline coroutine parts to guard against code movement
    // during optimizations. We inline them back in CoroSplit.
    for (CoroInitInst *CI : Coroutines) {
      CoroutineShape S{ CI }; // TODO: move out
      //outlineCoroutineParts(S);
    }

    return !Coroutines.empty();
  }

  bool runOnFunction(Function &F) override {
    DEBUG(dbgs() << "CoroEarly is looking at " << F.getName() << "\n");
    return false;
  }
};
}

char CoroEarly::ID = 0;
INITIALIZE_PASS(
  CoroEarly, "coro-early",
  "Coroutine frame allocation elision and indirect calls replacement", false,
  false);
Pass *llvm::createCoroEarlyPass() { return new CoroEarly(); }
