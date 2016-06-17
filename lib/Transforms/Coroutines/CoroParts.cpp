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

#define DEBUG_TYPE "coro-parts"

static BasicBlock* splitBlockIfNotFirst(Instruction* I, StringRef Name = "") {
  auto BB = I->getParent();
  if (&*BB->begin() == I) {
    BB->setName(Name);
    return BB;
  }
  return BB->splitBasicBlock(I, Name);
}

Instruction* findRetEnd(CoroutineShape& S) {
  auto RetBB = S.Return.back()->getParent();
  auto EndBB = S.CoroEndFinal.back()->getParent();
  if (RetBB == EndBB || RetBB->getUniquePredecessor() == EndBB)
    return S.Return.back();

  for (BasicBlock* BB : predecessors(RetBB))
    if (BB == EndBB)
      return &RetBB->front();

  return nullptr;
}

static void outlineCoroutineParts(CoroutineShape& S) {
  Function& F = *S.CoroInit.front()->getParent()->getParent();
  CoroCommon::removeLifetimeIntrinsics(F); // for now
  DEBUG(dbgs() << "Processing Coroutine: " << F.getName() << "\n");
  DEBUG(S.dump());

  CoroPartExtractor Extractor;
  auto Outline = [&](StringRef Name, Instruction* From, Instruction* Upto) {
    auto First = splitBlockIfNotFirst(From, Name);
    auto Last = splitBlockIfNotFirst(Upto);
    auto Fn = Extractor.createFunction(First, Last);
    return ValueAsMetadata::get(Fn);
  };

  // Outline the parts and create a metadata tuple, so that CoroSplit
  // pass can quickly figure out what they are.

  LLVMContext& C = F.getContext();
  SmallVector<Metadata *, 8> MDs{
    MDString::get(C, kCoroEarlyTagStr),
    Outline(".AllocPart", S.CoroAlloc.front(),
    S.CoroBegin.front()->getNextNode()),
    Outline(".FreePart", S.CoroFree.front(), S.CoroEndFinal.front()),
  };

  // If we can figure out end of the ret part, outline it too.
  if (auto RetEnd = findRetEnd(S)) {
    MDs.push_back(Outline(".RetPart", S.CoroEndFinal.front(), RetEnd));
  }

  // Outline suspend points.
  for (CoroSuspendInst *SI : S.CoroSuspend) {
    MDs.push_back(
      Outline(".SuspendPart", SI->getCoroSave(), SI->getNextNode()));
  }

  S.CoroInit.front()->setMeta(
    MetadataAsValue::get(F.getContext(), MDNode::get(C, MDs)));
}

static bool processModule(Module& M) {
  SmallVector<Function*, 8> Coroutines;
  Function *CoroInitFn = Intrinsic::getDeclaration(&M, Intrinsic::coro_init);

  // Find all pre-split coroutines.
  for (User* U : CoroInitFn->users())
    if (auto CoroInit = dyn_cast<CoroInitInst>(U))
      if (CoroInit->isPreSplit())
        Coroutines.push_back(CoroInit->getParent()->getParent());

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
  struct CoroParts : public ModulePass {
    static char ID; // Pass identification, replacement for typeid
    CoroParts() : ModulePass(ID) {}

    bool runOnModule(Module &M) override {
      return processModule(M);
    }
  };
}

char CoroParts::ID = 0;
INITIALIZE_PASS(CoroParts, "coro-parts", "Protect coroutine from code motion "
                                         "by extracting parts of it into "
                                         "separate functions",
                false, false);
Pass *llvm::createCoroPartsPass() { return new CoroParts(); }
