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
#include "CoroEarly.h"

using namespace llvm;

#define DEBUG_TYPE "coro-early"

namespace {
  /// Holds all structural Coroutine Intrinsics for a particular function
  struct CoroutineShape {
    CoroInitInst* CoroInit = nullptr;
    CoroElideInst* CoroElide = nullptr;

    CoroutineShape(CoroInitInst*);

    operator bool() const {
      return CoroElide != nullptr;
    }
  };
}

CoroutineShape::CoroutineShape(CoroInitInst *CoroInit)
    : CoroInit(CoroInit), CoroElide(CoroInit->getElide()) {}

static BasicBlock* splitBlockIfNotFirst(Instruction* I, StringRef Name) {
  auto BB = I->getParent();
  if (&*BB->begin() == I) {
    BB->setName(Name);
    return BB;
  }

  return BB->splitBasicBlock(I, Name);
}

static void outlineAllocPart(CoroutineShape const& S, CoroPartExtractor& E) {
  // split blocks before CoroElide and Before CoroInit

  auto Start = splitBlockIfNotFirst(S.CoroElide, "AllocBB");
  auto End = splitBlockIfNotFirst(S.CoroInit, "InitBB");
  E.createFunction(".AllocPart", Start, End);
}

static void outlineCoroutineParts(CoroutineShape const& S) {
  Function& F = *S.CoroInit->getParent()->getParent();
  removeLifetimeIntrinsics(F); // for now
  DEBUG(dbgs() << "Processing Coroutine: " << F.getName() << "\n");
  DEBUG(S.CoroElide->dump());
  DEBUG(S.CoroInit->dump());

  CoroPartExtractor Extractor;
  outlineAllocPart(S, Extractor);
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
