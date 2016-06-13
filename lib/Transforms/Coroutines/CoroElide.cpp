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

//#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"
//#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
//#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
//#include "llvm/PassSupport.h"
//#include "llvm/Support/raw_ostream.h"
//#include "llvm/Transforms/Utils/Local.h"
using namespace llvm;

#define DEBUG_TYPE "coro-elide"

STATISTIC(CoroElideCounter, "Number of coroutine allocation elision performed");

namespace llvm { 

  /// This represents the llvm.coro.init instruction.
  struct CoroInitInst : public IntrinsicInst {

    // Methods for support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::experimental_coro_init;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };

}

namespace {

// TODO: paste explanation
struct CoroElide : FunctionPass {
  static char ID; 
  CoroElide() : FunctionPass(ID) {}
  bool runOnFunction(Function &F) override;
};

}

char CoroElide::ID = 0;
INITIALIZE_PASS(
    CoroElide, "coro-elide",
    "Coroutine frame allocation elision and indirect calls replacement", false,
    false)

Pass *llvm::createCoroElidePass() { return new CoroElide(); }

bool CoroElide::runOnFunction(Function &F) {
  DEBUG(dbgs() << "CoroElide is looking at " << F.getName() << "\n");

  for (auto II = inst_begin(&F), IE = inst_end(&F); II != IE;) {
    auto &I = *II++;

    if (auto CoroInit = dyn_cast<CoroInitInst>(&I)) {
      DEBUG(dbgs() << "  found CoroInit: ");
      DEBUG(CoroInit->print(dbgs(), true));
      DEBUG(dbgs() << "\n");
    }
  }

  return false;
}
