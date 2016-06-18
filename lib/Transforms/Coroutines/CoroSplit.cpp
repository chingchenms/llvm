//===- CoroSplit2.cpp - Manager for Coroutine Passes -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// TODO: explaing what it is
//
//===----------------------------------------------------------------------===//

#include "CoroutineCommon.h"
#include <llvm/Transforms/Coroutines.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/LegacyPassManagers.h>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/Analysis/CallGraphSCCPass.h>

using namespace llvm;

#define DEBUG_TYPE "coro-split"

// Since runOnSCC cannot create any functions,
// we will create functions with empty bodies
// that we will fill later during runOnSCC.
// This structure will keep information about
// the functions we created during doInitialize
namespace {
  struct CoroInfoTy {
    StructType* FrameTy;
    Function* Resume;
    Function* Destroy;
    Function* Cleanup;
  };
}

/// addAbstractEdges - Add abstract edges to keep a coroutine
/// and its subfunctions together in one SCC
static void addAbstractEdges(std::initializer_list<CallGraphNode*> Nodes) {
  assert(Nodes.size() > 0);
  for (auto it = Nodes.begin(), e = Nodes.end(); it != e; ++it) {
    auto Target = (it + 1 == e) ? Nodes.begin() : it;
    (*it)->addCalledFunction(CallSite(), *Target);
  }
}

CoroInfoTy preSplit(CallGraph& CG, Function *F, CoroInitInst *CI)
{
  CoroInfoTy Info;

  SmallString<64> Name(F->getName());
  Name.push_back('.');
  auto const FirstPartSize = Name.size();

  Name.append("frame");
  LLVMContext& Ctx = F->getContext();
  Info.FrameTy = StructType::create(Ctx, Name);

  auto AS = CI->getMem()->getType()->getPointerAddressSpace();
  auto FnTy = FunctionType::get(
    Type::getVoidTy(Ctx),
    PointerType::get(Info.FrameTy, AS), /*isVarArg=*/false);

  auto CreateSubFunction = [&](StringRef Suffix) {
    Name.resize(FirstPartSize);
    Name.append(Suffix);
    auto Fn = Function::Create(FnTy, F->getLinkage(), Name, F->getParent());
    Fn->setCallingConv(CallingConv::Fast);
    Fn->addFnAttr(Attribute::NoInline);

    auto BB = BasicBlock::Create(Ctx, "entry", Fn);
    ReturnInst::Create(Ctx, BB);
    return CG.getOrInsertFunction(Fn);
  };

  auto ResumeNode = CreateSubFunction("resume");
  auto DestroyNode = CreateSubFunction("destroy");
  auto CleanupNode = CreateSubFunction("cleanup");

  addAbstractEdges({ CG[F], ResumeNode, DestroyNode, CleanupNode });

  Info.Resume = ResumeNode->getFunction();
  Info.Destroy = DestroyNode->getFunction();
  Info.Cleanup = CleanupNode->getFunction();

  return Info;
}

void updateMetadata(CoroInitInst* CI, CoroInfoTy& Info) {

}

#if 0
/// removeAbstractEdges - Remove abstract edges that keep a coroutine
/// and its subfunctions together in one SCC
static void removeAbstractEdges(CallGraphNode *CoroNode,
  CoroInfoTy const &CoroInfo) {

  CoroNode->removeOneAbstractEdgeTo(CoroInfo.ResumeNode);
  CoroInfo.ResumeNode->removeOneAbstractEdgeTo(CoroInfo.DestroyNode);
  CoroInfo.DestroyNode->removeOneAbstractEdgeTo(CoroInfo.CleanupNode);
  CoroInfo.CleanupNode->removeOneAbstractEdgeTo(CoroNode);
}
#endif

// CallGraphSCC Pass cannot add new functions
static bool preSplitCoroutines(CallGraph &CG) {
  Module &M = CG.getModule();
  bool changed = false;

  for (Function& F : M) {
    if (!F.hasFnAttribute(Attribute::Coroutine))
      continue;
    auto CoroInit = CoroCommon::findCoroInit(&F, Phase::PostSplit, false);
    if (!CoroInit)
      continue;

    CoroInfoTy Info = preSplit(CG, &F, CoroInit);
    updateMetadata(CoroInit, Info);
    changed = true;
  }

  return changed;
}

static void mem2reg(Function& F) {
  DominatorTree DT{ F };
  SmallVector<AllocaInst*, 8> Allocas;
  BasicBlock &BB = F.getEntryBlock();  // Get the entry node for the function
  while (1) {
    Allocas.clear();

    // Find allocas that are safe to promote, by looking at all instructions in
    // the entry node
    for (BasicBlock::iterator I = BB.begin(), E = --BB.end(); I != E; ++I)
      if (AllocaInst *AI = dyn_cast<AllocaInst>(I))       // Is it an alloca?
        if (isAllocaPromotable(AI))
          Allocas.push_back(AI);

    if (Allocas.empty()) break;

    PromoteMemToReg(Allocas, DT, nullptr);
  }
}

static void splitCoroutine(Function& F, CoroInfoTy const& CoroInfo) {
  DEBUG(dbgs() << "Splitting coroutine: " << F.getName() << "\n");
  mem2reg(F);
}

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

namespace {

  struct CoroSplit : public CallGraphSCCPass {
    static char ID; // Pass identification, replacement for typeid
    CoroSplit() : CallGraphSCCPass(ID) {}

    bool doInitialization(CallGraph &CG) override {
      bool changed = preSplitCoroutines(CG);
      changed |= CallGraphSCCPass::doInitialization(CG);
      return changed;
    }

    bool runOnSCC(CallGraphSCC &SCC) override {
#if 0
      // No coroutines, bail out
      if (DB.empty())
        return false;
      
      // SCC should be at least of size 4
      // Coroutine + Resume + Destroy + Cleanup
      if (SCC.size() < 4)
        return false;

      bool changed = false;

      for (CallGraphNode *CGN : SCC) {
        if (auto F = CGN->getFunction()) {
          if (auto CoroInfo = DB.find(F)) {
            splitCoroutine(*F, *CoroInfo);
            changed = true;
          }
        }
      }
      return changed;
#endif
      return false;
    }
  };
}

char CoroSplit::ID = 0;
INITIALIZE_PASS(
  CoroSplit, "coro-split",
  "Split coroutine into a set of funcitons driving its state machine", false,
  false);

Pass *llvm::createCoroSplitPass() { return new CoroSplit(); }
