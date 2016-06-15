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
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Coroutines.h"
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

static void outlineCoroutineParts(CoroutineShape const& S) {
  Function& F = *S.CoroInit->getParent()->getParent();
  DEBUG(dbgs() << "Processing Coroutine: " << F.getName() << "\n");
  DEBUG(S.CoroElide->dump());
  DEBUG(S.CoroInit->dump());
}

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

namespace {
struct CoroEarly : public FunctionPass {
  static char ID; // Pass identification, replacement for typeid
  CoroEarly() : FunctionPass(ID) {}

  bool doInitialization(Module& M) override {
    Function *CoroInitFn = Intrinsic::getDeclaration(&M, Intrinsic::coro_init);

    // find all coroutines and outline their parts
    for (User* U : CoroInitFn->users())
      if (auto CoroInit = dyn_cast<CoroInitInst>(U))
        if (CoroutineShape S = { CoroInit })
          outlineCoroutineParts(S);

    return !CoroInitFn->user_empty();
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
