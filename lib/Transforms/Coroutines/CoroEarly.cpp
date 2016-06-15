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
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/InstIterator.h"

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

/// definedInRegion - Return true if the specified value is defined in the
/// extracted region.
static bool definedInRegion(const SetVector<BasicBlock *> &Blocks, Value *V) {
  if (Instruction *I = dyn_cast<Instruction>(V))
    if (Blocks.count(I->getParent()))
      return true;
  return false;
}

/// definedInCaller - Return true if the specified value is defined in the
/// function being code extracted, but not in the region being extracted.
/// These values must be passed in as live-ins to the function.
static bool definedInCaller(const SetVector<BasicBlock *> &Blocks, Value *V) {
  if (isa<Argument>(V)) return true;
  if (Instruction *I = dyn_cast<Instruction>(V))
    if (!Blocks.count(I->getParent()))
      return true;
  return false;
}

struct CoroPartExtractor {

};

CoroutineShape::CoroutineShape(CoroInitInst *CoroInit)
    : CoroInit(CoroInit), CoroElide(CoroInit->getElide()) {}

void removeLifetimeIntrinsics(Function &F) {
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

static void outlineCoroutineParts(CoroutineShape const& S) {
  Function& F = *S.CoroInit->getParent()->getParent();
  removeLifetimeIntrinsics(F); // for now
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
#if EMULATE_INTRINSICS
    bool changed = false;
    for (Function& F : M) {
      for (Instruction& I : instructions(&F)) {
        if (auto *CoroInit = dyn_cast<CoroInitInst>(&I)) {
          if (CoroutineShape S = { CoroInit }) {
            outlineCoroutineParts(S);
            changed = true;
            break;
          }
        }
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
