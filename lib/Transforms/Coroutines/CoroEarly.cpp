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
#include "llvm/IR/InstIterator.h"

using namespace llvm;

#define DEBUG_TYPE "coro-early"

namespace {
  /// Holds all structural Coroutine Intrinsics for a particular function
  struct CoroutineShape {
    CoroInitInst* const CoroInit;
    CoroAllocInst* CoroAlloc;
    CoroForkInst* CoroFork = nullptr;
    CoroFreeInst* CoroFree = nullptr;
    CoroEndInst* CoroEnd = nullptr;

    CoroutineShape(CoroInitInst*);

    explicit operator bool() const {
      return CoroFork != nullptr && CoroFree != nullptr && CoroEnd != nullptr;
    }
  };
}

CoroutineShape::CoroutineShape(CoroInitInst *CoroInit)
  : CoroInit(CoroInit), CoroAlloc(CoroInit->getAlloc()) {
  for (User* U : CoroInit->users()) {
    if (auto II = dyn_cast<CoroIntrinsic>(U)) {
      switch (II->getIntrinsicID()) {
      default:
        continue;
      case Intrinsic::coro_fork:
        CoroFork = cast<CoroForkInst>(II);
        break;
      case Intrinsic::experimental_coro_delete:
        CoroFree = cast<CoroFreeInst>(II);
        break;
      case Intrinsic::coro_end:
        CoroEnd = cast<CoroEndInst>(II);
        break;
      }
    }
  }
}

static BasicBlock* splitBlockIfNotFirst(Instruction* I, StringRef Name) {
  auto BB = I->getParent();
  if (&*BB->begin() == I) {
    BB->setName(Name);
    return BB;
  }

  return BB->splitBasicBlock(I, Name);
}


static void outlineAllocPart(CoroutineShape& S, CoroPartExtractor& E) {
  auto Start = splitBlockIfNotFirst(S.CoroAlloc, "AllocPart");
  auto End = splitBlockIfNotFirst(S.CoroInit, "InitBB");

  // after we extract the AllocPart, we no longer need CoroAlloc
  S.CoroAlloc->replaceAllUsesWith(
      ConstantPointerNull::get(cast<PointerType>(S.CoroAlloc->getType())));
  S.CoroAlloc->eraseFromParent();
  S.CoroAlloc = nullptr;
  E.createFunction(Start, End);
}

static void outlineFreePart(CoroutineShape& S, CoroPartExtractor& E) {
  auto Start = splitBlockIfNotFirst(S.CoroFree, "FreePart");
  auto End = splitBlockIfNotFirst(S.CoroEnd, "EndBB");

  S.CoroFree->addAttribute(0, Attribute::NonNull);
  E.createFunction(Start, End);
}

static void outlinePrepPart(CoroutineShape const& S, CoroPartExtractor& E) {
  auto Start = splitBlockIfNotFirst(S.CoroInit->getNextNode(), "PrepPart");
  auto End = splitBlockIfNotFirst(S.CoroFork, "ForkBB");
  E.createFunction(Start, End);
}

static void outlineBodyPart(CoroutineShape const& S, CoroPartExtractor& E) {
  auto Start = splitBlockIfNotFirst(S.CoroFork->getNextNode(), "BodyPart");
  auto End = splitBlockIfNotFirst(S.CoroEnd, "EndBB");
  End->eraseFromParent();
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

  // we no longer need the CoroEnd
  //S.CoroEnd->eraseFromParent();

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
  outlineBodyPart(S, Extractor);
  outlineReturnPart(S, Extractor);
}

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

namespace {
struct CoroEarly : public FunctionPass {
  static char ID; // Pass identification, replacement for typeid
  CoroEarly() : FunctionPass(ID) {}

  bool doInitialization(Module& M) override {
    SmallVector<CoroInitInst*, 8> Coroutines;
#if EMULATE_INTRINSICS
    bool changed = false;
    for (Function& F : M)
      for (Instruction& I : instructions(&F))
        if (auto *CoroInit = dyn_cast<CoroInitInst>(&I))
          Coroutines.push_back(CoroInit);

    for (CoroInitInst* CI: Coroutines) {
      if (CoroutineShape S = { CI }) {
        outlineCoroutineParts(S);
        changed = true;
        break;
      }
    }
    return changed;
#else
    Function *CoroInitFn = Intrinsic::getDeclaration(&M, Intrinsic::coro_init);

    // find all coroutines and outline their parts
    for (User* U : CoroInitFn->users())
      if (auto CoroInit = dyn_cast<CoroInitInst>(U))
        if (CoroutineShape S = { CoroInit })
          outlineCoroutineParts(S);

    return !CoroInitFn->user_empty();
#endif
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
