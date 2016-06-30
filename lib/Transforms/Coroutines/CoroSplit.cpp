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
#include <llvm/IR/IRBuilder.h>
#include <llvm/Analysis/CallGraphSCCPass.h>

using namespace llvm;

#define DEBUG_TYPE "coro-split"

#if 0

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

static void addIndirectCall(CallGraph &CG, CallGraphNode *CoroNode,
                            CoroInitInst *CI, CoroInfo const& Info) {
  IRBuilder<> Builder(CI->getNextNode());
  auto FnPtrType = Info.Destroy->getFunctionType()->getPointerTo();
  auto DestroyAddrIntrin =
      Intrinsic::getDeclaration(CI->getModule(), Intrinsic::coro_destroy_addr);

  auto Addr = Builder.CreateCall(DestroyAddrIntrin, CI);
  auto Fn = Builder.CreateBitCast(Addr, FnPtrType);
  auto Arg = Builder.CreateBitCast(CI, Info.FrameType->getPointerTo());
  auto CS = Builder.CreateCall(Fn, Arg);
  CS->setCallingConv(CallingConv::Fast);

  CoroNode->addCalledFunction(CS, CG.getCallsExternalNode());
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

  auto CoroNode = CG[F];
  auto ResumeNode = CreateSubFunction("resume");
  auto DestroyNode = CreateSubFunction("destroy");
  auto CleanupNode = CreateSubFunction("cleanup");

  addCallsToEachOther({ CoroNode, ResumeNode, DestroyNode, CleanupNode});

  Info.Resume = ResumeNode->getFunction();
  Info.Destroy = DestroyNode->getFunction();
  Info.Cleanup = CleanupNode->getFunction();

  addIndirectCall(CG, CoroNode, CI, Info);

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

  // If there are parts, replace noinline with alwaysinline

  for (MDOperand const& P : CI->meta().getParts()) {
    auto SubF = cast<Function>(cast<ValueAsMetadata>(P)->getValue());
    SubF->removeFnAttr(Attribute::NoInline);
    SubF->addFnAttr(Attribute::AlwaysInline);
  }

  // Finally, indicate that the coroutine is ready for Split
  CI->meta().setPhase(Phase::ReadyForSplit);
}

static void splitCoroutine(CoroInitInst * CI, CoroutineShape& Shape) {
  Function& F = *CI->getFunction();
  DEBUG(dbgs() << "Splitting coroutine: " << F.getName() << "\n");
  mem2reg(F);
  Shape.buildFrom(F);
  buildCoroutineFrame(F, Shape);
}

// FIXME: Use explicit tag for the phase
// it is likely that we would like to control inlining by
// ourselves prior to split
bool removeNoInline(Function& F, CoroBeginInst& CoroBeg) {
  ConstantStruct *ST = CoroBeg.getOutlinedParts();
  unsigned I = 0, N = ST->getNumOperands();
  assert(N > 0);

  for (;I < N; ++I) {
    auto Part = cast<Function>(ConstantFolder().CreateExtractValue(ST, I)); 
    if (!Part->hasFnAttribute(Attribute::NoInline))
      return false;
    Part->removeFnAttr(Attribute::NoInline);
    Part->addFnAttr(Attribute::AlwaysInline);
  }
  return true;
}
#endif

BasicBlock* createResumeEntryBlock(Function& F, CoroutineShape& Shape) {
  LLVMContext& C = F.getContext();
  auto NewEntry = BasicBlock::Create(C, "resume.entry", &F);
  auto UnreachBB = BasicBlock::Create(C, "UnreachBB", &F);

  IRBuilder<> Builder(NewEntry);
  auto vFramePtr = CoroFrameInst::Create(Builder);
  auto FramePtr = Builder.CreateBitCast(vFramePtr, Shape.FramePtrTy);
  auto FrameTy = Shape.FramePtrTy->getElementType();
  auto GepIndex =
      Builder.CreateConstInBoundsGEP2_32(FrameTy, FramePtr, 0, 2, "index.addr");
  auto Index =
    Builder.CreateLoad(GepIndex, "index");
  auto Switch =
      Builder.CreateSwitch(Index, UnreachBB, Shape.CoroSuspend.size());

  uint8_t SuspendIndex = -1;
  for (auto S: Shape.CoroSuspend) {
    ++SuspendIndex;
    ConstantInt* IndexVal = Builder.getInt8(SuspendIndex);

    // replace CoroSave with a store to Index
    auto Save = S->getCoroSave();
    Builder.SetInsertPoint(Save);
    auto GepIndex =
      Builder.CreateConstInBoundsGEP2_32(FrameTy, FramePtr, 0, 2, "index.addr");
    Builder.CreateStore(IndexVal, GepIndex);
    Save->replaceAllUsesWith(ConstantTokenNone::get(C));
    Save->eraseFromParent();

    // make sure that CoroSuspend is at the beginning of the block
    // and add a branch to it from a switch

    auto SuspendBB = S->getParent();
    if (&SuspendBB->front() != S) {
      SuspendBB = SuspendBB->splitBasicBlock(S);
    }
    SuspendBB->setName("resume." + Twine::utohexstr(SuspendIndex));
    Switch->addCase(IndexVal, SuspendBB);
  }

  Builder.SetInsertPoint(UnreachBB);
  Builder.CreateUnreachable();

  return NewEntry;
}

static void replaceWith(ArrayRef<Instruction *> Instrs, Value *NewValue,
                        ValueToValueMapTy *VMap = nullptr) {
  for (Instruction* I : Instrs) {
    if (VMap) I = cast<Instruction>((*VMap)[I]);
    I->replaceAllUsesWith(NewValue);
    I->eraseFromParent();
  }
}

template <typename T>
static void replaceWith(T &C, Value *NewValue,
                        ValueToValueMapTy *VMap = nullptr) {
  Instruction* TestB = *C.begin();
  Instruction* TestE = *C.begin();
  TestB; TestE;
  Instruction** B = reinterpret_cast<Instruction**>(C.begin());
  Instruction** E = reinterpret_cast<Instruction**>(C.end());
  ArrayRef<Instruction*> Ar(B, E);
  replaceWith(Ar, NewValue, VMap);
}

void replaceCoroEnd(ArrayRef<CoroEndInst*> Ends, ValueToValueMapTy& VMap) {
  for (auto E : Ends) {
    auto NewE = cast<CoroEndInst>(VMap[E]);
    LLVMContext& C = NewE->getContext();
    auto Ret = ReturnInst::Create(C, nullptr, NewE);
    auto BB = NewE->getParent();
    BB->splitBasicBlock(NewE);
    Ret->getNextNode()->eraseFromParent();
  }
}

static Function *createClone(Function &F, Twine Suffix, CoroutineShape &Shape,
  BasicBlock *ResumeEntry, int8_t index) {

  Module* M = F.getParent();
  auto FrameTy = cast<StructType>(Shape.FramePtrTy->getElementType());
  auto FnPtrTy = cast<PointerType>(FrameTy->getElementType(0));
  auto FnTy = cast<FunctionType>(FnPtrTy->getElementType());

  Function *NewF = Function::Create(
    FnTy, GlobalValue::LinkageTypes::InternalLinkage, F.getName() + Suffix, M);

  SmallVector<ReturnInst*, 4> Returns;

  ValueToValueMapTy VMap;
  // replace all args with undefs
  for (Argument& A : F.getArgumentList())
    VMap[&A] = UndefValue::get(A.getType());

  CloneFunctionInto(NewF, &F, VMap, true, Returns);


  auto Entry = cast<BasicBlock>(VMap[ResumeEntry]);
  Entry->moveBefore(&NewF->getEntryBlock());
  IRBuilder<> Builder(F.getContext());

  auto NewValue = Builder.getInt8(index);
  replaceWith(Shape.CoroSuspend, NewValue, &VMap);

  replaceCoroEnd(Shape.CoroEndFinal, VMap);
  replaceCoroEnd(Shape.CoroEndUnwind, VMap);

  //for (auto S : Shape.CoroEndFinal) {
  //  auto NewS = cast<Instruction>(VMap[S]);
  //  NewS->replaceAllUsesWith(NewValue);
  //  NewS->eraseFromParent();
  //}


  return &F;
}

static void splitCoroutine(Function &F, CoroBeginInst &CB) {
  CoroutineShape Shape(F);
  buildCoroutineFrame(F, Shape);
  auto ResumeEntry = createResumeEntryBlock(F, Shape);
  auto ResumeClone = createClone(F, ".Resume", Shape, ResumeEntry, 0);
  auto DestroyClone = createClone(F, ".Destroy", Shape, ResumeEntry, 1);
}

static bool handleCoroutine(Function& F, CallGraph &CG, CallGraphSCC &SCC) {
  for (auto& I : instructions(F)) {
    if (auto CB = dyn_cast<CoroBeginInst>(&I)) {
      auto Info = CB->getInfo();
      // this coro.begin belongs to inlined post-split coroutine we called
      if (Info.postSplit())
        continue;

      if (Info.needToOutline()) {
        outlineCoroutineParts(F, CG, SCC);
        return true; // restart needed
      }

      splitCoroutine(F, *CB);
      F.removeFnAttr(Attribute::Coroutine); // now coroutine is a normal func
      return false; // restart not needed
    }
  }
  llvm_unreachable("Coroutine without defininig coro.begin");
}

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

namespace {

  struct CoroSplit : public CallGraphSCCPass {
    static char ID; // Pass identification, replacement for typeid
    CoroSplit() : CallGraphSCCPass(ID) {}

    bool needToRestart;
    
    bool restartRequested() const override { return needToRestart; }

    bool runOnSCC(CallGraphSCC &SCC) override {
      CallGraph &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
      needToRestart = false;

      // find coroutines for processing
      SmallVector<Function*, 4> Coroutines;
      for (CallGraphNode *CGN : SCC)
        if (auto F = CGN->getFunction())
          if (F->hasFnAttribute(Attribute::Coroutine))
            Coroutines.push_back(F);

      if (Coroutines.empty())
        return false;

      for (Function* F : Coroutines)
        needToRestart |= handleCoroutine(*F, CG, SCC);

      return true;
    }

  };
}

char CoroSplit::ID = 0;
#if 1
INITIALIZE_PASS(
    CoroSplit, "coro-split",
    "Split coroutine into a set of functions driving its state machine", false,
    false);
#else
INITIALIZE_PASS_BEGIN(
    CoroSplit, "coro-split",
    "Split coroutine into a set of functions driving its state machine", false,
    false)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(
    CoroSplit, "coro-split",
    "Split coroutine into a set of functions driving its state machine", false,
    false)
#endif
Pass *llvm::createCoroSplitPass() { return new CoroSplit(); }
