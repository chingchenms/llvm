//===-- Coroutines.cpp ----------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// This file implements the common infrastructure for Coroutine Passes.
//===----------------------------------------------------------------------===//

#include "CoroInternal.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/InitializePasses.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

using namespace llvm;

void llvm::initializeCoroutines(PassRegistry &Registry) {
  initializeCoroEarlyPass(Registry);
  initializeCoroSplitPass(Registry);
  initializeCoroElidePass(Registry);
  initializeCoroCleanupPass(Registry);
}

static void addCoroutineOpt0Passes(const PassManagerBuilder &Builder,
                                   legacy::PassManagerBase &PM) {
  PM.add(createCoroSplitPass());
  PM.add(createCoroElidePass());

  PM.add(createBarrierNoopPass());
  PM.add(createCoroCleanupPass());
}

static void addCoroutineEarlyPasses(const PassManagerBuilder &Builder,
                                    legacy::PassManagerBase &PM) {
  PM.add(createCoroEarlyPass());
}

static void addCoroutineScalarOptimizerPasses(const PassManagerBuilder &Builder,
                                              legacy::PassManagerBase &PM) {
  PM.add(createCoroElidePass());
}

static void addCoroutineSCCPasses(const PassManagerBuilder &Builder,
                                  legacy::PassManagerBase &PM) {
  PM.add(createCoroSplitPass());
}

static void addCoroutineOptimizerLastPasses(const PassManagerBuilder &Builder,
                                            legacy::PassManagerBase &PM) {
  PM.add(createCoroCleanupPass());
}

void llvm::addCoroutinePassesToExtensionPoints(PassManagerBuilder &Builder) {
  Builder.addExtension(PassManagerBuilder::EP_EarlyAsPossible,
                       addCoroutineEarlyPasses);
  Builder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                       addCoroutineOpt0Passes);
  Builder.addExtension(PassManagerBuilder::EP_CGSCCOptimizerLate,
                       addCoroutineSCCPasses);
  Builder.addExtension(PassManagerBuilder::EP_ScalarOptimizerLate,
                       addCoroutineScalarOptimizerPasses);
  Builder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                       addCoroutineOptimizerLastPasses);
}

// Construct the lowerer base class and initialize its members.
coro::LowererBase::LowererBase(Module &M)
    : TheModule(M), Context(M.getContext()),
      ResumeFnType(FunctionType::get(Type::getVoidTy(Context),
                                     Type::getInt8PtrTy(Context),
                                     /*isVarArg=*/false)) {}

// Creates a sequence of instructions to obtain a resume function address using
// llvm.coro.subfn.addr. It generates the following sequence:
//
//    call i8* @llvm.coro.subfn.addr(i8* %Arg, i8 %index)
//    bitcast i8* %2 to void(i8*)*

Value *coro::LowererBase::makeSubFnCall(Value *Arg, int Index,
                                        Instruction *InsertPt) {
  auto *IndexVal = ConstantInt::get(Type::getInt8Ty(Context), Index);
  auto *Fn = Intrinsic::getDeclaration(&TheModule, Intrinsic::coro_subfn_addr);

  assert(Index >= CoroSubFnInst::IndexFirst &&
         Index < CoroSubFnInst::IndexLast &&
         "makeSubFnCall: Index value out of range");
  auto *Call = CallInst::Create(Fn, {Arg, IndexVal}, "", InsertPt);

  auto *Bitcast =
      new BitCastInst(Call, ResumeFnType->getPointerTo(), "", InsertPt);
  return Bitcast;
}

#ifndef NDEBUG
static bool isCoroutineIntrinsicName(StringRef Name) {
  // NOTE: Must be sorted!
  static const char *const CoroIntrinsics[] = {
      "llvm.coro.alloc",   "llvm.coro.begin",   "llvm.coro.destroy",
      "llvm.coro.done",    "llvm.coro.end",     "llvm.coro.frame",
      "llvm.coro.free",    "llvm.coro.id",      "llvm.coro.param",
      "llvm.coro.promise", "llvm.coro.resume",  "llvm.coro.save",
      "llvm.coro.size",    "llvm.coro.suspend",
  };
  return Intrinsic::lookupLLVMIntrinsicByName(CoroIntrinsics, Name) != -1;
}
#endif

// Verifies if a module has named values listed. Also, in debug mode verifies
// that names are intrinsic names.
bool coro::declaresIntrinsics(Module &M,
                              std::initializer_list<StringRef> List) {

  for (StringRef Name : List) {
    assert(isCoroutineIntrinsicName(Name) && "not a coroutine intrinsic");
    if (M.getNamedValue(Name))
      return true;
  }

  return false;
}

// FIXME: This code is stolen from CallGraph::addToCallGraph(Function *F), which
// happens to be private. It is better for this functionality exposed by the
// CallGraph.
static void buildCGN(CallGraph &CG, CallGraphNode *Node) {
  Function *F = Node->getFunction();

  // Look for calls by this function.
  for (Instruction &I : instructions(F))
    if (CallSite CS = CallSite(cast<Value>(&I))) {
      const Function *Callee = CS.getCalledFunction();
      if (!Callee || !Intrinsic::isLeaf(Callee->getIntrinsicID()))
        // Indirect calls of intrinsics are not allowed so no need to check.
        // We can be more precise here by using TargetArg returned by
        // Intrinsic::isLeaf.
        Node->addCalledFunction(CS, CG.getCallsExternalNode());
      else if (!Callee->isIntrinsic())
        Node->addCalledFunction(CS, CG.getOrInsertFunction(Callee));
    }
}

// Rebuild CGN after we extracted parts of the code from ParentFunc into
// NewFuncs. Builds CGNs for the NewFuncs and adds them to the current SCC.
void coro::updateCallGraph(Function &ParentFunc, ArrayRef<Function *> NewFuncs,
                           CallGraph &CG, CallGraphSCC &SCC) {
  // Rebuild CGN from scratch for the ParentFunc
  auto *ParentNode = CG[&ParentFunc];
  ParentNode->removeAllCalledFunctions();
  buildCGN(CG, ParentNode);

  SmallVector<CallGraphNode *, 8> Nodes(SCC.begin(), SCC.end());

  for (Function *F : NewFuncs) {
    CallGraphNode *Callee = CG.getOrInsertFunction(F);
    Nodes.push_back(Callee);
    buildCGN(CG, Callee);
  }

  SCC.initialize(Nodes);
}

static void clear(coro::Shape &Shape) {
  Shape.CoroBegin = nullptr;
  Shape.CoroEnds.clear();
  Shape.CoroSizes.clear();
  Shape.CoroSuspends.clear();

  Shape.FrameTy = nullptr;
  Shape.FramePtr = nullptr;
  Shape.PromiseAlloca = nullptr;
  Shape.AllocaSpillBlock = nullptr;
  Shape.ResumeSwitch = nullptr;
  Shape.HasFinalSuspend = false;
}

void coro::Shape::buildFrom(Function &F) {
  clear(*this);
  for (auto IB = inst_begin(F), IE = inst_end(F); IB != IE;) {
    if (auto II = dyn_cast<IntrinsicInst>(&*IB++)) {
      switch (II->getIntrinsicID()) {
      default:
        continue;
      case Intrinsic::coro_size:
        CoroSizes.push_back(cast<CoroSizeInst>(II));
        break;
      case Intrinsic::coro_frame:
        assert(CoroBegin && "coro.frame should not appear before coro.begin");
        II->replaceAllUsesWith(CoroBegin);
        II->eraseFromParent();
        break;
      case Intrinsic::coro_suspend:
        assert(CoroBegin && "coro.suspend should not appear before coro.begin");
        CoroSuspends.push_back(cast<CoroSuspendInst>(II));
        if (!CoroSuspends.back()->getCoroSave()) {
          CoroSaveInst::Create(CoroBegin, CoroSuspends.back());
        }
        if (CoroSuspends.back()->isFinal()) {
          HasFinalSuspend = true;
          if (CoroSuspends.size() > 1) {
            assert(!CoroSuspends.front()->isFinal() &&
              "Only one suspend point can be marked as final");
            std::swap(CoroSuspends.front(), CoroSuspends.back());
          }
        }
        break;
      case Intrinsic::coro_begin: {
        auto CB = cast<CoroBeginInst>(II);
        if (CB->getId()->getInfo().isPreSplit()) {
          assert(!CoroBegin &&
            "coroutine should have exactly one defining @llvm.coro.begin");
          CB->addAttribute(0, Attribute::NonNull);
          CB->addAttribute(0, Attribute::NoAlias);
          CB->removeAttribute(0, Attribute::NoDuplicate);
          CoroBegin = CB;
        }
        break;
      }
      case Intrinsic::coro_end:
        CoroEnds.push_back(cast<CoroEndInst>(II));
        if (CoroEnds.back()->isFinal()) {
          if (CoroEnds.size() > 1) {
            assert(!CoroEnds.front()->isFinal() &&
              "Only one suspend point can be marked as final");
            std::swap(CoroEnds.front(), CoroEnds.back());
          }
        }
        break;
      }
    }
  }
}