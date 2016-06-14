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
#include "llvm/Analysis/ConstantFolding.h"
//#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
//#include "llvm/PassSupport.h"
//#include "llvm/Support/raw_ostream.h"
//#include "llvm/Transforms/Utils/Local.h"
using namespace llvm;

#define DEBUG_TYPE "coro-elide"

STATISTIC(CoroElideCounter, "Number of coroutine allocation elision performed");

namespace llvm { 

  /// This represents the llvm.coro.init instruction.
  class CoroInitInst : public IntrinsicInst {
    enum { kMem, kAlign, kPromise, kParts };
  public:
    Value *getMem() const { return getArgOperand(kMem); }    
    ConstantInt *getAlignment() const {
      return cast<ConstantInt>(getArgOperand(kAlign));
    }

    AllocaInst *getPromise() const {
      auto Arg = getArgOperand(kPromise);
      if (isa<ConstantPointerNull>(Arg))
        return nullptr;
      return cast<AllocaInst>(Arg->stripPointerCasts());
    }

    struct Parts {
      Type* const FrameTy;
      Function* const ResumeFn;
      Function* const DestroyFn;
      Function* const CleanupFn;

      // If CoroSplit pass did not generate CleanupFn
      // Heap Allocation Elision cannot be performed
      bool elidable() { return CleanupFn != nullptr; }
    };

    MDNode *getRawParts() const {
      auto MV = cast<MetadataAsValue>(getArgOperand(kParts))->getMetadata();
      auto MN = dyn_cast<MDNode>(MV);
      if (!MN)
        return nullptr;

      if (MN->getNumOperands() < 4)
        return nullptr;

      return MN;
    }

    // FIXME: Create class for CoroMetadata
    Parts getParts() const {
      auto MN = getRawParts();
      ConstantAsMetadata* C1 = cast<ConstantAsMetadata>(MN->getOperand(1));
      auto* C2 = cast<ConstantAsMetadata>(MN->getOperand(2))->getValue();
      auto* C3 = cast<ConstantAsMetadata>(MN->getOperand(3))->getValue();
      ConstantAsMetadata* C4 = 
        MN->getNumOperands() < 5 ? nullptr :
        cast<ConstantAsMetadata>(MN->getOperand(4));

      ;

      return{
        cast<PointerType>(C1->getType())->getElementType(),
        cast<Function>(C2),
        cast<Function>(C3),
        nullptr };
    }

    bool isPostSplit() const {
      return getRawParts();
    }

#if 0
    Parts getParts() const {
      assert(isPostSplit() && "getParts is called for preSplit coroinit");

      auto CE = cast<ConstantExpr>(getArgOperand(kParts));
      assert(CE->getOpcode() == Instruction::BitCast);

      auto GV = cast<GlobalVariable>(CE->getOperand(0));
      auto Value = GV->getInitializer();

      return {
        cast<Function>(Value->getAggregateElement(0u)),
        cast<Function>(Value->getAggregateElement(1u)),
        cast_or_null<Function>(Value->getAggregateElement(2u))
      };
    }

    // post-split coroutines have non-null parts parameter
    bool isPostSplit() const {
      return !isa<ConstantPointerNull>(getArgOperand(kParts));
    }
    // post-split coroutine has cleanup part
#endif
    // Methods for support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::coro_init;
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

static void constantFoldUsers(Constant* Value) {
  std::set<Instruction*> WorkList;
  for (User *U : Value->users())
    WorkList.insert(cast<Instruction>(U));

  if (WorkList.empty())
    return;

  Instruction *FirstInstr = *WorkList.begin();
  Function* F = FirstInstr->getParent()->getParent();
  const DataLayout &DL = F->getParent()->getDataLayout();

  do {
    Instruction *I = *WorkList.begin();
    WorkList.erase(WorkList.begin());    // Get an element from the worklist...

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

static void replaceWithConstant(Constant *Value,
                         SmallVectorImpl<IntrinsicInst*> &Users) {
  if (Users.empty())
    return;

  // All intrinsics in Users list should have the same type  
  auto IntrTy = Users.front()->getType();
  auto ValueTy = Value->getType();
  if (ValueTy != IntrTy) {
    assert(ValueTy->isPointerTy() && IntrTy->isPointerTy());
    Value = ConstantExpr::getBitCast(Value, IntrTy);
  }

  for (IntrinsicInst *I : Users) {
    I->replaceAllUsesWith(Value);
    I->eraseFromParent();
  }
  
  // do constant propagation
  constantFoldUsers(Value);
}

// IRBuilder<> Builder(I);
// Value *Counter = Builder.CreateConstInBoundsGEP2_64(Parts, 0, Index);
// Value *Count = Builder.CreateLoad(Counter);

static bool replaceIndirectCalls(CoroInitInst *CoroInit) {

  SmallVector<IntrinsicInst*, 8> ResumeAddr;
  SmallVector<IntrinsicInst*, 8> DestroyAddr;

  for (auto U : CoroInit->users()) {
    if (auto II = dyn_cast<IntrinsicInst>(U))
      switch (II->getIntrinsicID()) {
      default:
        continue;
      case Intrinsic::coro_resume_addr:
        ResumeAddr.push_back(II);
        break;
      case Intrinsic::coro_destroy_addr:
        DestroyAddr.push_back(II);
        break;
      }
  }

  auto P = CoroInit->getParts();
  DEBUG(dbgs() << "  found CoroInit with: " 
    << P.ResumeFn->getName() << ", "
    << P.DestroyFn->getName());
  if (P.CleanupFn) {
    DEBUG(dbgs() << ", " << P.CleanupFn->getName());
  }
  DEBUG(dbgs() << "\n");

  replaceWithConstant(P.ResumeFn, ResumeAddr);
  replaceWithConstant(P.DestroyFn, DestroyAddr);

  return !ResumeAddr.empty() || !DestroyAddr.empty();
}

bool CoroElide::runOnFunction(Function &F) {
  DEBUG(dbgs() << "CoroElide is looking at " << F.getName() << "\n");
  bool changed = false;

  // Collect all coro inits that belong to post-split coroutines 
  SmallVector<CoroInitInst*, 4> CoroInits;
  for (auto& I : instructions(F))
    if (auto CI = dyn_cast<CoroInitInst>(&I))
      if (CI->isPostSplit())
        CoroInits.push_back(CI);

  for (auto CI : CoroInits)
    changed = replaceIndirectCalls(CI);

  return changed;
}
