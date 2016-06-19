//===- CoroSplit.cpp - Converts coroutine into a state machine-------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// TODO: explain what it is
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

static Instruction* findInsertionPoint(Function *F) {
  // Get the first non alloca instruction
  for (Instruction& I : F->getEntryBlock())
    if (!isa<AllocaInst>(&I))
      return &I;

  llvm_unreachable("no terminator in the entry block");
}

// insert nulls for pointer parameters and undef for others
static CallInst* makeFakeCall(Function* Caller, Function* Callee) {
  FunctionType* FTy = Callee->getFunctionType();
  SmallVector<Value*, 8> Args;
  for (Type* Ty : FTy->params()) {
    Value* V = nullptr;
    if (auto PTy = dyn_cast<PointerType>(Ty))
      V = ConstantPointerNull::get(PTy);
    else
      V = UndefValue::get(Ty);
    Args.push_back(V);
  }

  auto InsertPt = findInsertionPoint(Caller);
  return CallInst::Create(Callee, Args, "", InsertPt);
}

/// addAbstractEdges - Add abstract edges to keep a coroutine
/// and its subfunctions together in one SCC
static void addAbstractEdges(std::initializer_list<CallGraphNode*> Nodes) {
  assert(Nodes.size() > 0);
  for (auto it = Nodes.begin(), e = Nodes.end(); it != e; ++it) {
    auto Next = it + 1;
    auto Target = (Next == e) ? Nodes.begin() : Next;
    auto F = (*it)->getFunction();
    auto CS = makeFakeCall(F, (*Target)->getFunction());
    (*it)->addCalledFunction(CS, *Target);
  }
}

void addCallToCoroutine(CallGraphNode *From, CallGraphNode* To) {
  Function* F = From->getFunction();
  Function* Callee = To->getFunction();
  assert(Callee->hasFnAttribute(Attribute::Coroutine));
  FunctionType* FTy = Callee->getFunctionType();
  SmallVector<Value*, 8> Args;
  for (Type* Ty : FTy->params())
    Args.push_back(UndefValue::get(Ty));

  auto InsertPt = findInsertionPoint(F);
  auto CS = CallInst::Create(Callee, Args, "", InsertPt);
  From->addCalledFunction(CS, To);
}

CoroInfo preSplit(CallGraph& CG, Function *F, CoroInitInst *CI)
{
  CoroInfo Info;

  SmallString<64> Name(F->getName());
  Name.push_back('.');
  auto const FirstPartSize = Name.size();

  Name.append("frame");
  LLVMContext& Ctx = F->getContext();
  Info.FrameType = StructType::create(Ctx, Name);
  auto AS = CI->getMem()->getType()->getPointerAddressSpace();
  auto FramePtrTy = PointerType::get(Info.FrameType, AS);

  auto FnTy = FunctionType::get(
    Type::getVoidTy(Ctx),
    FramePtrTy, /*isVarArg=*/false);

  auto CreateSubFunction = [&](StringRef Suffix) {
    Name.resize(FirstPartSize);
    Name.append(Suffix);
    auto Fn = Function::Create(FnTy, F->getLinkage(), Name, F->getParent());
    Fn->setCallingConv(CallingConv::Fast);
    Fn->addFnAttr(Attribute::NoInline);

    auto BB = BasicBlock::Create(Ctx, "entry", Fn);
    auto Ret = ReturnInst::Create(Ctx, BB);
    Argument* Arg = &*Fn->getArgumentList().begin();
    auto Cast = new BitCastInst(Arg, Type::getInt8PtrTy(Ctx), "", Ret);
    CoroEndInst::Create(Ret, Cast);
    return CG.getOrInsertFunction(Fn);
  };

  auto ResumeNode = CreateSubFunction("resume");
  auto DestroyNode = CreateSubFunction("destroy");
  auto CleanupNode = CreateSubFunction("cleanup");

  addAbstractEdges({ CG[F], ResumeNode, DestroyNode, CleanupNode});
#if 0
  auto CoroNode = CG[F];
  addCall(CoroNode, ResumeNode, FramePtrTy);
  addCall(ResumeNode, DestroyNode, FramePtrTy);
  addCall(DestroyNode, CleanupNode, FramePtrTy);
  addCallToCoroutine(CleanupNode, CoroNode);
#endif

  Info.Resume = ResumeNode->getFunction();
  Info.Destroy = DestroyNode->getFunction();
  Info.Cleanup = CleanupNode->getFunction();

  return Info;
}

void updateMetadata(CoroInitInst* CI, CoroInfo& Info) {
  CI->meta().update(
      {CoroMeta::Setter{Info.FrameType},
       CoroMeta::Setter{CoroMeta::Field::Resumers,
                        {Info.Resume, Info.Destroy, Info.Cleanup}}});

  CI->meta().getCoroInfo();
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

    CoroInfo Info = preSplit(CG, &F, CoroInit);
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

static void splitCoroutine(Function& F, CoroInitInst * CI) {
  DEBUG(dbgs() << "Splitting coroutine: " << F.getName() << "\n");
  if (CI->meta().getPhase() == Phase::PreIPO) {
    CI->meta().setPhase(Phase::PreSplit);
    return;
  }
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
      // SCC should be at least of size 4
      // Coroutine + Resume + Destroy + Cleanup
      if (SCC.size() < 3)
        return false;

      bool changed = false;

      for (CallGraphNode *CGN : SCC) {
        if (auto F = CGN->getFunction()) {
          if (auto CoroInit =
                  CoroCommon::findCoroInit(F, Phase::PostSplit, false)) {
            splitCoroutine(*F, CoroInit);
            changed = true;
          }
        }
      }
      return changed;
    }
  };
}

char CoroSplit::ID = 0;
INITIALIZE_PASS(
  CoroSplit, "coro-split",
  "Split coroutine into a set of funcitons driving its state machine", false,
  false);

Pass *llvm::createCoroSplitPass() { return new CoroSplit(); }
