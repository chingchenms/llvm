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
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/InstIterator.h"
#include <llvm/Transforms/Utils/BasicBlockUtils.h>

using namespace llvm;

#define DEBUG_TYPE "coro-early"

// Need metadata helper
//   Coroutine comes out of front end with !"" in place of the metadata
// 
// CoroEarly, will replace it with the tuple:
//   (!func,!"",!"",!"")

#if 0
static void initMetadata(CoroInitInst& CoroInit, Function& F) {
  if (auto S = dyn_cast<MDString>(CoroInit.getRawMeta())) {
    assert(S->getLength() != 0 && "Unexpected metadata string on coro.init");
    SmallVector<Metadata *, 4> Args{ValueAsMetadata::get(&F), S, S, S};
    CoroInit.setMeta(MDNode::get(F.getContext(), Args));
  }
}
#endif

static void addBranchToCoroEnd(CoroutineShape &S, Function &F) {
  S.buildFrom(F);
  auto Zero = ConstantInt::get(S.CoroSuspend.back()->getType(), 0);
  auto RetBB = S.CoroEndFinal.back()->getParent();
  assert(isa<CoroEndInst>(RetBB->front()) &&
         "coro.end must be a first instruction in a block");
  for (CoroSuspendInst* CI : S.CoroSuspend) {
    auto InsertPt = CI->getNextNode();
    auto Cond =
      new ICmpInst(InsertPt, CmpInst::Predicate::ICMP_SLT, CI, Zero);
    SplitBlockAndInsertIfThen(Cond, InsertPt, /*unreachable=*/false);
  }
}

static bool replaceEmulatedIntrinsicsWithRealOnes(Module& M) {
  bool changed = true;
  SmallVector<Value*, 8> Args;
  LLVMContext & C = M.getContext();
  auto BytePtrTy = PointerType::get(IntegerType::get(C, 8), 0);
  auto Zero = ConstantInt::get(IntegerType::get(C, 32), 0);
  auto Null = ConstantPointerNull::get(BytePtrTy);
  auto MetaVal = MetadataAsValue::get(C, MDString::get(C, ""));

  CallInst* SavedIntrinsic = nullptr;
  CoroutineShape CoroShape;

  for (Function& F : M) {
    bool hasCoroSuspend = false;
    bool hasCoroInit = false;
    for (auto it = inst_begin(F), e = inst_end(F); it != e;) {
      Instruction& I = *it++;
      if (auto CI = dyn_cast<CallInst>(&I)) {
        if (auto F = CI->getCalledFunction()) {
          const auto id = StringSwitch<Intrinsic::ID>(F->getName())
            .Case("llvm_coro_alloc", Intrinsic::coro_alloc)
            .Case("llvm_coro_init", Intrinsic::coro_init)
            .Case("llvm_coro_begin", Intrinsic::coro_begin)
            .Case("llvm_coro_save", Intrinsic::coro_save)
            .Case("llvm_coro_suspend", Intrinsic::coro_suspend)
            .Case("llvm_coro_free", Intrinsic::coro_free)
            .Case("llvm_coro_end", Intrinsic::coro_end)
            .Default(Intrinsic::not_intrinsic);

          Function *Fn = Intrinsic::getDeclaration(&M, id);
          Args.clear();
//          dbgs() << "Looking at >>>>  "; CI->dump();
          switch (id) {
          case Intrinsic::not_intrinsic:
            continue;
          default:
            break;
          case Intrinsic::coro_suspend:
            hasCoroSuspend = true;
            assert(SavedIntrinsic && "coro_suspend expects saved intrinsic");
            Args.push_back(SavedIntrinsic);
            Args.push_back(CI->getArgOperand(1));
            break;
          case Intrinsic::coro_begin:
            Args.push_back(Null);
            break;
          case Intrinsic::coro_end:
            Args.push_back(Null);
            Args.push_back(CI->getArgOperand(0));
            break;
          case Intrinsic::coro_free:
            Args.push_back(CI->getArgOperand(0));
            break;
          case Intrinsic::coro_init:
            hasCoroInit = true;
            Args.push_back(CI->getArgOperand(0));
            Args.push_back(CI->getArgOperand(1));
            Args.push_back(Zero);
            Args.push_back(CI->getArgOperand(2));
            Args.push_back(MetaVal);
            break;
          }

          auto IntrinCall = CallInst::Create(Fn, Args, "");
          if (id == Intrinsic::coro_save) {
            IntrinCall->insertBefore(CI);
            BasicBlock::iterator BI(CI);
            ReplaceInstWithValue(CI->getParent()->getInstList(), BI, Null);
            SavedIntrinsic = IntrinCall;
            continue;
          }
          else if (id == Intrinsic::coro_suspend) {
            ReplaceInstWithInst(CI, IntrinCall);
            SavedIntrinsic = nullptr;
          }
          else {
            ReplaceInstWithInst(CI, IntrinCall);
          }
  //        dbgs() << "Replaced with >>>>  "; IntrinCall->dump();
          changed = true;
        }
      }
    }
    if (hasCoroSuspend)
      addBranchToCoroEnd(CoroShape, F);
    if (hasCoroInit)
      F.addFnAttr(Attribute::Coroutine);
  }
  return changed;
}

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

namespace {
struct CoroEarly : public FunctionPass {
  static char ID; // Pass identification, replacement for typeid
  CoroEarly() : FunctionPass(ID) {}

  bool doInitialization(Module& M) override {
    return replaceEmulatedIntrinsicsWithRealOnes(M);
  }

  bool runOnFunction(Function &F) override {
    if (!F.hasFnAttribute(Attribute::Coroutine))
      return false;

    Shape.buildFrom(F);

    Shape.CoroInit.back()->meta().update({
      {Phase::NotReadyForSplit, F.getContext()},
      {CoroMeta::Field::Func, &F}
    });
    return true;
  }

  CoroutineShape Shape;
};
}

char CoroEarly::ID = 0;
INITIALIZE_PASS(
  CoroEarly, "coro-early",
  "Coroutine frame allocation elision and indirect calls replacement", false,
  false);
Pass *llvm::createCoroEarlyPass() { return new CoroEarly(); }
