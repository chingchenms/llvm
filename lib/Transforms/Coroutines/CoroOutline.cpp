//===- CoroParts.cpp - ModuleEarly Coroutine Pass -------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// CoroParts - ModulePass ran at extension point EP_EarlyAsPossible
// see ./Coroutines.rst for details
//
//===----------------------------------------------------------------------===//

#include "CoroutineCommon.h"
#include "CoroExtract.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/ConstantFolder.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/InstIterator.h"
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <utility>

using namespace llvm;
using namespace llvm::CoroCommon;

#define DEBUG_TYPE "coro-outline"

static std::pair<Instruction*,Instruction*> getRetCode(CoroutineShape& S) {
  auto NextNode = S.CoroReturn.back()->getNextNode();
  BasicBlock* EndBB = nullptr;
  if(NextNode->isTerminator()) {
    if (isa<ReturnInst>(NextNode))
      return{NextNode, NextNode};
    auto NextBB = NextNode->getParent()->getSingleSuccessor();
    assert(NextBB);
    NextNode = NextBB->getFirstNonPHI();
    EndBB = NextBB;
  }
  else {
    EndBB = NextNode->getParent()->splitBasicBlock(NextNode, "RetBB");
  }

  auto RetStartBB = EndBB;

  auto RetEndBB = RetStartBB;
  while (BasicBlock* Next = RetEndBB->getSingleSuccessor()) {
    RetEndBB = Next;
  }
  return {&RetStartBB->front(), RetEndBB->getTerminator()};
}

void llvm::outlineCoroutineParts(Function &F, CallGraph &CG,
                                 CallGraphSCC &SCC) {
  CoroutineShape S(F);
  Module &M = *F.getParent();
  CoroCommon::removeLifetimeIntrinsics(F); // for now
  DEBUG(dbgs() << "Processing Coroutine: " << F.getName() << "\n");
  DEBUG(S.dump());

  CoroPartExtractor Extractor;
  auto Outline = [&](StringRef Name, Instruction *From, Instruction *Upto) {
    auto First = splitBlockIfNotFirst(From, Name);
    auto Last = splitBlockIfNotFirst(Upto, "End" + Name);
    auto Fn = Extractor.createFunction(First, Last, Name);
    Fn->addFnAttr(Attribute::AlwaysInline);
    return Fn;
  };

  // Outline the parts and create a metadata tuple, so that CoroSplit
  // pass can quickly figure out what they are.

  SmallVector<Function *, 8> Funcs{Outline(".AllocPart", S.CoroAlloc.back(),
                                           S.CoroBegin.back()->getNextNode())};

  for (CoroEndInst *CE : S.CoroEnd) {
    Value* FrameArg = CE->getFrameArg();
    if (isa<ConstantPointerNull>(FrameArg))
      continue;

    if (isa<UndefValue>(FrameArg))
      continue;

    auto Start = cast<CoroFreeInst>(FrameArg);
    auto End = CE->getNextNode();
    Funcs.push_back(Outline(".FreePart", Start, End));
  }

  {
    auto RC = getRetCode(S);
    if (RC.first != RC.second)
      Funcs.push_back(Outline(".RetPart", RC.first, RC.second));
  }

  // Outline suspend points.
  for (CoroSuspendInst *SI : S.CoroSuspend) {
    Funcs.push_back(
        Outline(".SuspendPart", SI->getCoroSave(), SI->getNextNode()));
  }

  ArrayRef<Function *> FuncArrRef(Funcs);
  ArrayRef<Constant *> &ConstantArrRef =
      reinterpret_cast<ArrayRef<Constant *> &>(FuncArrRef);
  auto ConstVal = ConstantStruct::getAnon(ConstantArrRef);
  auto GV = new GlobalVariable(M, ConstVal->getType(), /*isConstant=*/true,
                               GlobalVariable::PrivateLinkage, ConstVal,
                               F.getName() + Twine(".outlined"));

  // Update coro.begin instruction to refer to this constant
  LLVMContext &C = F.getContext();
  auto BC = ConstantFolder().CreateBitCast(GV, Type::getInt8PtrTy(C));
  S.CoroBegin.back()->setInfo(BC);

  updateCallGraph(F, Funcs, CG, SCC);
  }

#if 0
static bool processModule(Module& M) {
  SmallVector<Function*, 8> Coroutines;
  Function *CoroBeginFn = Intrinsic::getDeclaration(&M, Intrinsic::coro_begin);

  // Find all unprocessed coroutines.
  for (User* U : CoroBeginFn->users())
    if (auto CoroBeg = dyn_cast<CoroBeginInst>(U))
      if (CoroBeg->isUnprocessed())
        Coroutines.push_back(CoroBeg->getFunction());

  // Outline coroutine parts to guard against code movement
  // during optimizations. We inline them back in CoroSplit.
  CoroutineShape S;
  for (Function *F : Coroutines) {
    S.buildFrom(*F);
    outlineCoroutineParts(S);
  }
  return !Coroutines.empty();
}

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

namespace {
  struct CoroOutline : public ModulePass {
    static char ID; // Pass identification, replacement for typeid
    CoroOutline() : ModulePass(ID) {}

    bool runOnModule(Module &M) override {
      return false; //processModule(M);
    }
  };
}

char CoroOutline::ID = 0;
INITIALIZE_PASS(CoroOutline, "coro-outline", "Outline parts of a coroutine to"
                                         "protect against code motion",
                false, false);
Pass *llvm::createCoroOutlinePass() { return new CoroOutline(); }
#endif