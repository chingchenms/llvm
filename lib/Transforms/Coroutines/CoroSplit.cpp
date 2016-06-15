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

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Function.h>
#include <llvm/Analysis/CallGraphSCCPass.h>

using namespace llvm;

#define DEBUG_TYPE "coro-split"

namespace {
  /// This represents the llvm.coro.init instruction.
  class CoroInitInst : public IntrinsicInst {
    enum { kElide, kMem, kAlign, kPromise, kMeta };
  public:
    Value *getElide() const { return getArgOperand(kElide); }

    Value *getMem() const { return getArgOperand(kMem); }

    ConstantInt *getAlignment() const {
      return cast<ConstantInt>(getArgOperand(kAlign));
    }

    // if this CoroInit belongs to pre-Split coroutine function Fn,
    // metadata contains a Function* pointing back to Fn.
    // If so, return it, otherwise, return nullptr
    Function* getCoroutine() const {
      auto MD = cast<MetadataAsValue>(getArgOperand(kMeta))->getMetadata();
      if (auto MV = dyn_cast<ValueAsMetadata>(MD))
        return dyn_cast<Function>(MV->getValue());
      return nullptr;
    }

    // Methods for support type inquiry through isa, cast, and dyn_cast:
    static inline bool classof(const IntrinsicInst *I) {
      return I->getIntrinsicID() == Intrinsic::coro_init;
    }
    static inline bool classof(const Value *V) {
      return isa<IntrinsicInst>(V) && classof(cast<IntrinsicInst>(V));
    }
  };

}

// Since runOnSCC cannot create any functions,
// we will create functions with empty bodies
// that we will fill later during runOnSCC.
// This structure will keep information about
// the functions we created during doInitialize
namespace {
  struct CoroInfoTy {
    CoroInitInst* CoroInit;
    StructType* FrameTy;
    CallGraphNode* ResumeNode;
    CallGraphNode* DestroyNode;
    CallGraphNode* CleanupNode;

    CoroInfoTy(CallGraph& CG, Function* Coro, CoroInitInst* CI);
  };
}

CoroInfoTy::CoroInfoTy(CallGraph& CG, Function *F, CoroInitInst *CI)
  : CoroInit(CI)
{
  assert(CI->getFunction() == F && "coro.init in a pre-split coroutine"
                                   "must refer to an enclosing function");
  SmallString<64> Name(F->getName());
  Name.push_back('.');
  auto const FirstPartSize = Name.size();


  Name.append("frame");
  LLVMContext& Ctx = F->getContext();
  this->FrameTy = StructType::create(Ctx, Name);

  auto AS = CI->getMem()->getType()->getPointerAddressSpace();
  auto FnTy = FunctionType::get(
    Type::getVoidTy(Ctx),
    PointerType::get(FrameTy, AS), /*isVarArg=*/false);

  auto CreateSubFunction = [&](StringRef Suffix) {
    Name.resize(FirstPartSize);
    Name.append(Suffix);
    auto Fn = Function::Create(FnTy, F->getLinkage(), Name, F->getParent());
    Fn->setCallingConv(CallingConv::Fast);

    auto BB = BasicBlock::Create(Ctx, "entry", Fn);
    ReturnInst::Create(Ctx, BB);
    return CG.getOrInsertFunction(Fn);
  };

  this->ResumeNode = CreateSubFunction("resume");
  this->DestroyNode = CreateSubFunction("destroy");
  this->CleanupNode = CreateSubFunction("cleanup");
}

// CoroDatabase maintains a mapping 
// Function* -> CoroInfoTy
namespace {
  struct CoroDatabase {

    CoroInfoTy const& add(CallGraph&, Function*, CoroInitInst*);
    bool empty() const { return Data == nullptr; }
    CoroInfoTy const * find(Function*);
    void sort();

  private:
    struct CoroIndexTy {
      Function* Fn;
      size_t Index;
    };
    struct DataTy {
      SmallVector<CoroIndexTy, 8> Index;
      SmallVector<CoroInfoTy, 8> Parts;
      bool Sorted;
    };
    std::unique_ptr<DataTy> Data;
  };
}

void CoroDatabase::sort() {
  if (empty())
    return;
  if (Data->Sorted)
    return;

  std::sort(Data->Index.begin(), Data->Index.end(),
            [](auto a, auto b) { return a.Fn < b.Fn; });
  Data->Sorted = true;

  // Verify that we don't have duplicates
  // Note, if assert is OK, Data->Index is unchanged
  assert(Data->Index.end() ==
             std::unique(Data->Index.begin(), Data->Index.end(),
                         [](auto a, auto b) { return a.Fn == b.Fn; }) &&
         "duplicate CoroInit");
}

CoroInfoTy const *CoroDatabase::find(Function *F) {
  assert(!empty() && Data->Sorted && "invalid coroutine database");
  auto Beg = Data->Index.begin();
  auto End = Data->Index.end();
  auto I = std::lower_bound(
      Beg, End, F, [](CoroIndexTy V, Function *F) { return V.Fn < F; });
  if (I == End || I->Fn != F)
    return nullptr;

  return &Data->Parts[I->Index];
}

CoroInfoTy const &CoroDatabase::add(CallGraph &CG, Function *F,
                                    CoroInitInst *CoroInit) {
  if (!Data) {
    Data = std::make_unique<DataTy>();
    Data->Sorted = true; // with only one element, obviously sorted
  }
  else {
    Data->Sorted = false;
  }
  Data->Index.push_back({ F, Data->Parts.size() });
  Data->Parts.emplace_back(CG, F, CoroInit);
  return Data->Parts.back();
}

/// addAbstractEdges - Add abstract edges to keep a coroutine
/// and its subfunctions together in one SCC
static void addAbstractEdges(CallGraphNode *CoroNode,
                             CoroInfoTy const &CoroInfo) {
  CoroNode->addCalledFunction(CallSite(), CoroInfo.ResumeNode);
  CoroInfo.ResumeNode->addCalledFunction(CallSite(), CoroInfo.DestroyNode);
  CoroInfo.DestroyNode->addCalledFunction(CallSite(), CoroInfo.CleanupNode);
  CoroInfo.CleanupNode->addCalledFunction(CallSite(), CoroNode);
}

/// removeAbstractEdges - Remove abstract edges that keep a coroutine
/// and its subfunctions together in one SCC
static void removeAbstractEdges(CallGraphNode *CoroNode,
                             CoroInfoTy const &CoroInfo) {

  CoroNode->removeOneAbstractEdgeTo(CoroInfo.ResumeNode);
  CoroInfo.ResumeNode->removeOneAbstractEdgeTo(CoroInfo.DestroyNode);
  CoroInfo.DestroyNode->removeOneAbstractEdgeTo(CoroInfo.CleanupNode);
  CoroInfo.CleanupNode->removeOneAbstractEdgeTo(CoroNode);
}

// CallGraphSCC Pass cannot add new functions
static bool preSplitCoroutines(CallGraph &CG, CoroDatabase& DB) {
  Module &M = CG.getModule();
  Function *CoroInitFn = Intrinsic::getDeclaration(&M, Intrinsic::coro_init);

  for (User* U : CoroInitFn->users()) {
    if (auto CoroInit = dyn_cast<CoroInitInst>(U)) {
      if (Function* CoroFn = CoroInit->getCoroutine()) {
        auto &CoroInfo = DB.add(CG, CoroFn, CoroInit);
        addAbstractEdges(CG[CoroFn], CoroInfo);
      }
    }
  }
  DB.sort();

  return !DB.empty();
}

static void splitCoroutine(Function& F, CoroInfoTy const& CoroInfo) {
  DEBUG(dbgs() << "Splitting coroutine: " << F.getName() << "\n");
}

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

namespace {

  struct CoroSplit : public CallGraphSCCPass {
    static char ID; // Pass identification, replacement for typeid
    CoroSplit() : CallGraphSCCPass(ID) {}

    bool doInitialization(CallGraph &CG) override {
      bool changed = preSplitCoroutines(CG, DB);
      changed |= CallGraphSCCPass::doInitialization(CG);
      return changed;
    }

    bool runOnSCC(CallGraphSCC &SCC) override {
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
            removeAbstractEdges(CGN, *CoroInfo);
            splitCoroutine(*F, *CoroInfo);
            changed = true;
          }
        }
      }
      return changed;
    }

    CoroDatabase DB;
  };
}

char CoroSplit::ID = 0;
INITIALIZE_PASS(
  CoroSplit, "coro-split",
  "Split coroutine into a set of funcitons driving its state machine", false,
  false);

Pass *llvm::createCoroSplitPass() { return new CoroSplit(); }
