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
#include "llvm/IR/IRBuilder.h"
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
#if 0
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
#endif

static void finalizeCoroutine(Function& F) {
  CoroutineShape CoroShape(F);
  F.addFnAttr(Attribute::Coroutine);

  auto SuspendCount = CoroShape.CoroSuspend.size();
  if (SuspendCount == 0)
    return;

  CoroReturnInst* End = CoroShape.CoroReturn.back();
  BasicBlock* EndBB = End->getParent();
  if (&EndBB->front() != End) {
    EndBB = EndBB->splitBasicBlock(End, "CoroEnd");
  }

  IRBuilder<> Builder(End);

  // add a branch for coro_suspend to EndBB
  // this is done since, we cannot represent suspend jump when emulating
  // intrinsics with function call in coroutine unaware frontend
  for (CoroSuspendInst* S : CoroShape.CoroSuspend) {
    auto ResumeBB =
      S->getParent()->splitBasicBlock(S->getNextNode(), "ResumeBB");
    Builder.SetInsertPoint(S->getNextNode());
    auto Cond = Builder.CreateICmpSLT(S, Builder.getInt8(0));
    Builder.CreateCondBr(Cond, EndBB, ResumeBB);
    Builder.GetInsertPoint()->eraseFromParent();
  }

#if 0
  auto SuspendCount = CoroShape.CoroSuspend.size();
  if (SuspendCount == 0)
    return;

  CoroEndInst* End = CoroShape.CoroEndFinal.back();
  BasicBlock* EndBB = End->getParent();
  if (&EndBB->front() != End) {
    EndBB = EndBB->splitBasicBlock(End, "CoroEnd");
  }

  IRBuilder<> Builder(End);

  Value* FrameArg = End->getFrameArg();
  PHINode* Phi = Builder.CreatePHI(FrameArg->getType(), 1 + SuspendCount);
  for (BasicBlock *BB : predecessors(EndBB))
    Phi->addIncoming(FrameArg, BB);
  End->setArgOperand(0, Phi);

  Value* Undef = UndefValue::get(FrameArg->getType());

  // add a branch for coro_suspend to EndBB
  // this is done since, we cannot represent suspend jump when emulating
  // intrinsics with function call in coroutine unaware frontend
  for (CoroSuspendInst* S : CoroShape.CoroSuspend) {
    auto ResumeBB =
        S->getParent()->splitBasicBlock(S->getNextNode(), "ResumeBB");
    Builder.SetInsertPoint(S->getNextNode());
    auto Cond = Builder.CreateICmpSLT(S, Builder.getInt8(0));
    Builder.CreateCondBr(Cond, EndBB, ResumeBB);
    Builder.GetInsertPoint()->eraseFromParent();
    Phi->addIncoming(Undef, S->getParent());
  }
#endif
}

static bool replaceEmulatedIntrinsicsWithRealOnes(Module& M) {
  bool changed = true;
  SmallVector<Value*, 8> Args;
  LLVMContext & C = M.getContext();
  auto BytePtrTy = PointerType::get(IntegerType::get(C, 8), 0);
//  auto Zero = ConstantInt::get(IntegerType::get(C, 32), 0);
  auto Null = ConstantPointerNull::get(BytePtrTy);

  CallInst* SavedIntrinsic = nullptr;

  for (Function& F : M) {
    bool hasCoroSuspend = false;
    bool hasCoroInit = false;
    for (auto it = inst_begin(F), e = inst_end(F); it != e;) {
      Instruction& I = *it++;
      if (auto CI = dyn_cast<CallInst>(&I)) {
        if (auto F = CI->getCalledFunction()) {
          const auto ID = StringSwitch<Intrinsic::ID>(F->getName())
            .Case("llvm_coro_alloc", Intrinsic::coro_alloc)
            .Case("llvm_coro_begin", Intrinsic::coro_begin)
            .Case("llvm_coro_save", Intrinsic::coro_save)
            .Case("llvm_coro_suspend", Intrinsic::coro_suspend)
            .Case("llvm_coro_free", Intrinsic::coro_free)
            .Case("llvm_coro_size", Intrinsic::coro_size)
            .Case("llvm_coro_frame", Intrinsic::coro_frame)
            .Case("llvm_coro_end", Intrinsic::coro_end)
            .Case("llvm_coro_return", Intrinsic::coro_return)
            .Default(Intrinsic::not_intrinsic);

          Function *Fn =
              ID == Intrinsic::coro_size
                  ? Intrinsic::getDeclaration(&M, ID, F->getReturnType())
                  : Intrinsic::getDeclaration(&M, ID);
          Args.clear();
//          dbgs() << "Looking at >>>>  "; CI->dump();
          switch (ID) {
          case Intrinsic::not_intrinsic:
            continue;
          default:
            break;
          case Intrinsic::coro_suspend:
            hasCoroSuspend = true;
            assert(SavedIntrinsic && "coro_suspend expects saved intrinsic");
            Args.push_back(SavedIntrinsic);
            break;
          case Intrinsic::coro_return:
          case Intrinsic::coro_end:
          case Intrinsic::coro_save:
          case Intrinsic::coro_free:
            Args.push_back(CI->getArgOperand(0));
            break;
          case Intrinsic::coro_begin:
            hasCoroInit = true;
            Args.push_back(CI->getArgOperand(0));
            Args.push_back(CI->getArgOperand(1));
            Args.push_back(CI->getArgOperand(2));
            Args.push_back(CI->getArgOperand(3));
            Args.push_back(CI->getArgOperand(4));
            break;
          }

          auto IntrinCall = CallInst::Create(Fn, Args, "");
          if (ID == Intrinsic::coro_save) {
            IntrinCall->insertBefore(CI);
            BasicBlock::iterator BI(CI);
            ReplaceInstWithValue(CI->getParent()->getInstList(), BI, Null);
            SavedIntrinsic = IntrinCall;
            continue;
          }
          else if (ID == Intrinsic::coro_suspend) {
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
    if (hasCoroInit)
      finalizeCoroutine(F);
  }
  return changed;
}

// Keeps data common to all lowering functions.
struct Lowerer {
  Module& M;
  LLVMContext& C;
  IRBuilder<> Builder;
  Type* Int8Ty;
  Type* Int8PtrTy;

  PointerType* AnyResumeFnPtrTy;

  Lowerer(Function &F)
      : M(*F.getParent()), C(F.getContext()), Builder(C),
        Int8Ty(Type::getInt8Ty(C)), Int8PtrTy(Int8Ty->getPointerTo()),
        AnyResumeFnPtrTy(FunctionType::get(Type::getVoidTy(C), Int8PtrTy,
                                           /*isVarArg=*/false)
                             ->getPointerTo()) {}

  void replaceCoroPromise(IntrinsicInst *intrin, bool from = false);

};

void Lowerer::replaceCoroPromise(IntrinsicInst *intrin, bool from) {
  Value *Operand = intrin->getArgOperand(0);
  auto PromisePtr = cast<PointerType>(
    from ? Operand->getType() : intrin->getFunctionType()->getReturnType());
  auto PromiseType = PromisePtr->getElementType();

  // FIXME: this should be queried from FrameBuilding layer, not here
  auto SampleStruct = StructType::get(C, 
      {AnyResumeFnPtrTy, AnyResumeFnPtrTy, Int8Ty, PromiseType});
  const DataLayout &DL = M.getDataLayout();
  const int64_t Offset = DL.getStructLayout(SampleStruct)->getElementOffset(3);

  Value* Replacement = nullptr;

  Builder.SetInsertPoint(intrin);
  if (from) {
    auto BCI = Builder.CreateBitCast(Operand, Int8PtrTy);
    auto Gep = Builder.CreateConstInBoundsGEP1_32(Int8Ty, BCI, -Offset);
    Replacement = Gep;
  }
  else {
    auto Gep = Builder.CreateConstInBoundsGEP1_32(Int8Ty, Operand, Offset);
    auto BCI = Builder.CreateBitCast(Gep, PromisePtr);
    Replacement = BCI;
  }

  intrin->replaceAllUsesWith(Replacement);
  intrin->eraseFromParent();
}

void lowerResumeOrDestroy(IntrinsicInst* II, unsigned Index) {
  Module *M = II->getModule();
  auto Fn = Intrinsic::getDeclaration(M, Intrinsic::coro_subfn_addr);
  LLVMContext& C = II->getContext();
  auto IndexVal = ConstantInt::get(Type::getInt8Ty(C), Index);

  SmallVector<Value*, 2> Args{ II->getArgOperand(0), IndexVal };
  auto Call = CallInst::Create(Fn, Args, "", II);

  auto FTy = FunctionType::get(Type::getVoidTy(C), Type::getInt8PtrTy(C),
                               /*isVarArg=*/false);
  auto Bitcast = new BitCastInst(Call, FTy->getPointerTo(), "", II);

  auto Indirect = CallInst::Create(Bitcast, II->getArgOperand(0), "", II);
  Indirect->setCallingConv(CallingConv::Fast);

  II->eraseFromParent();
}

// TODO: handle invoke coro.resume and coro.destroy
bool lowerEarlyIntrinsics(Function& F) {
  Lowerer L(F);
  bool changed = false;
  for (auto IB = inst_begin(F), IE = inst_end(F); IB != IE;)
    if (auto II = dyn_cast<IntrinsicInst>(&*IB++)) {
      switch (II->getIntrinsicID()) {
      default:
        continue;
      case Intrinsic::coro_resume:
        lowerResumeOrDestroy(II, 0);
        break;
      case Intrinsic::coro_destroy:
        lowerResumeOrDestroy(II, 1);
        break;
      case Intrinsic::coro_promise:
        L.replaceCoroPromise(II);
        break;
      case Intrinsic::coro_from_promise:
        L.replaceCoroPromise(II, /*from=*/true);
        break;
      }
      changed = true;
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
    bool changed = lowerEarlyIntrinsics(F);
    if (!F.hasFnAttribute(Attribute::Coroutine))
      return changed;

    Shape.buildFrom(F);

    return changed;
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
