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
#include <llvm/IR/InstIterator.h>
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

// make a call to Callee with undef arguments
static CallInst* makeCallWithUndefArguments(Function* Caller, Function* Callee) {
  FunctionType* FTy = Callee->getFunctionType();

  SmallVector<Value*, 8> Args;
  for (Type* Ty : FTy->params()) {
    Value* V = UndefValue::get(Ty);
    Args.push_back(V);
  }

  auto InsertPt = findInsertionPoint(Caller);
  return CallInst::Create(Callee, Args, "", InsertPt);
}

// Make sure that all functions are in the same SCC by
// adding calls to each other. 
static void addCallsToEachOther(ArrayRef<CallGraphNode *> Nodes) {
  for (unsigned I = 0, N = Nodes.size(); I < N; ++I) {
    auto Caller = Nodes[I];
    auto Callee = Nodes[(I + 1) % N];
    auto CS = makeCallWithUndefArguments(Caller->getFunction(),
                                         Callee->getFunction());
    Caller->addCalledFunction(CS, Callee);
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

  addCallsToEachOther({ CG[F], ResumeNode, DestroyNode, CleanupNode});

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

static void removeCall(CallGraphNode* From, CallGraphNode* To) {
  auto Caller = From->getFunction();
  auto Callee = To->getFunction();

  // assumes there is exactly one call from Caller to Callee
  for (Instruction& I : instructions(Caller))
    if (CallSite CS = CallSite(&I))
      if (CS.getCalledFunction() == Callee) {
        From->removeCallEdgeFor(CS);
        I.eraseFromParent();
        return;
      }
  llvm_unreachable("subfunction call is missing from a coroutine");
}

static CallGraphNode* functionToCGN(Function* F, CallGraphSCC &SCC) {
  for (CallGraphNode* CGN : SCC)
    if (CGN->getFunction() == F)
      return CGN;
  llvm_unreachable("coroutine subfunction is missing from SCC");
}

static void preSplitPass(CoroInitInst * CI, CallGraphSCC &SCC) {
  // 1. If there are parts, replace noinline with alwaysinline. (TODO)
  // 2. Remove fake calls that make coroutine and its resumers to be
  //    in the same SCC.
  // 3. Add a dummy call to 

  Function& F = *CI->getFunction();
  CallGraphNode* CoroNode = functionToCGN(&F, SCC);
  CoroInfo Info = CI->meta().getCoroInfo();
  SmallVector<CallGraphNode*, 4> CGNs;
  CGNs.push_back(CoroNode);
  CGNs.push_back(functionToCGN(Info.Resume, SCC));
  CGNs.push_back(functionToCGN(Info.Destroy, SCC));
  CGNs.push_back(functionToCGN(Info.Cleanup, SCC));

  for (unsigned I = 0, N = CGNs.size(); I < N; ++I)
    removeCall(CGNs[I], CGNs[(I+1) % N]);

  // We add an indirect call to be removed later by CoroElide
  // in order to trigger rerun due to devirtualization
  // TODO: addIndirectCall(CI, CoroNode);
}

static void splitCoroutine(CoroInitInst * CI) {
  Function& F = *CI->getFunction();
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
      // SCC should be at least of size 4
      // Coroutine + Resume + Destroy + Cleanup
      //if (SCC.size() < 3)
      //  return false;

      // find coroutines for processing
      SmallVector<CoroInitInst*, 4> CIs;
      for (CallGraphNode *CGN : SCC)
        if (auto F = CGN->getFunction())
          if (auto CoroInit =
                  CoroCommon::findCoroInit(F, Phase::PostSplit, false))
            CIs.push_back(CoroInit);

      if (CIs.empty())
        return false;

      for (CoroInitInst* CoroInit : CIs)
        if (CoroInit->meta().getPhase() == Phase::PreIPO)
          preSplitPass(CoroInit, SCC);
        else
          splitCoroutine(CoroInit);

      return true;
    }
  };
}

char CoroSplit::ID = 0;
INITIALIZE_PASS(
  CoroSplit, "coro-split",
  "Split coroutine into a set of funcitons driving its state machine", false,
  false);

Pass *llvm::createCoroSplitPass() { return new CoroSplit(); }
