//===- CoroFrameBuilder.cpp - Decide which values go into coroutine frame -===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains classes used to discover if for a particular value
// there from sue to definition that crosses a suspend block.
//
// Using the information discovered we form a Coroutine Frame structure to 
// contain those values. All uses of those values are replaed with appropriate
// GEP + load from the coroutine frame. At the point of the defintion we 
// spill the value into the coroutine frame.
// 
// TODO: pack values tightly using liveness info
// TODO: propery update debug information
//
//===----------------------------------------------------------------------===//

#include "CoroUtils.h"

#include <llvm/ADT/BitVector.h>
#include <llvm/ADT/PackedVector.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IRBuilder.h>
#include "llvm/IR/Dominators.h"
#include <llvm/Support/Debug.h>
#include <llvm/Support/circular_raw_ostream.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include "llvm/IR/CFG.h"

// coro-suspend-crossing is very noisy
// there is another debug type defined later on which
// is much nicer, called "coro-frame"
#define DEBUG_TYPE "coro-suspend-crossing"

using namespace llvm;
using namespace llvm::CoroUtils;

enum { SmallVectorThreshold = 32 };
// Provides two way mapping between the blocks and numbers
class BlockToIndexMapping {
  SmallVector<BasicBlock*, SmallVectorThreshold> V;
public:
  size_t size() const { return V.size(); }

  BlockToIndexMapping(Function& F) {
    for (BasicBlock & BB : F)
      V.push_back(&BB);
    std::sort(V.begin(), V.end());
  }

  size_t blockToIndex(BasicBlock* BB) const {
    auto I = std::lower_bound(V.begin(), V.end(), BB);
    assert(I != V.end() && *I == BB && "BasicBlockNumberng: Unknown block");
    return I - V.begin();
  }

  BasicBlock* indexToBlock(unsigned Index) { return V[Index]; }
};

struct SuspendCrossingInfo {
  BlockToIndexMapping Mapping;

  struct BlockData {
    BitVector Consumes;
    BitVector Kills;
    bool Suspend = false;
    bool End = false;
  };
  SmallVector<BlockData, SmallVectorThreshold> Block;

  iterator_range<succ_iterator> successors(BlockData const &BD) {
    BasicBlock* BB = Mapping.indexToBlock(&BD - &Block[0]);
    return llvm::successors(BB);
  }

  BlockData &getBlockData(BasicBlock *BB) {
    return Block[Mapping.blockToIndex(BB)];
  }

  void dump();
  void dump(StringRef Label, BitVector const& BV);

  SuspendCrossingInfo(Function& F, CoroutineShape& Shape);

  bool hasPathCrossingSuspendPoint(BasicBlock* DefBB, BasicBlock* UseBB) {
    size_t const DefIndex = Mapping.blockToIndex(DefBB);
    size_t const UseIndex = Mapping.blockToIndex(UseBB);

    assert(Block[UseIndex].Consumes[DefIndex] && "use must consume def");
    auto Result = Block[UseIndex].Kills[DefIndex];
    DEBUG(dbgs() << UseBB->getName() << " => " << DefBB->getName()
                 << " answer is " << Result << "\n");
    return Result;
  }

  bool definitionAcrossSuspend(BasicBlock* DefBB, User* U) {
    auto I = cast<Instruction>(U);

    // We rewritten PHINodes, so that only the ones with exactly one incoming
    // value neeed to be analyzed.
    if (auto PN = dyn_cast<PHINode>(I))
      if (PN->getNumIncomingValues() > 1)
        return false;

    BasicBlock* UseBB = I->getParent();
    return hasPathCrossingSuspendPoint(DefBB, UseBB);
  }

  bool definitionAcrossSuspend(Argument &A, User *U) {
    return definitionAcrossSuspend(&A.getParent()->getEntryBlock(), U);
  }

  bool definitionAcrossSuspend(Instruction &I, User *U) {
    return definitionAcrossSuspend(I.getParent(), U);
  }
};

void SuspendCrossingInfo::dump(StringRef Label, BitVector const& BV) {
  dbgs() << Label << ":";
  for (size_t I = 0, N = BV.size(); I < N; ++I)
    if (BV[I])
      dbgs() << " " << Mapping.indexToBlock(I)->getName();
  dbgs() << "\n";
}

void SuspendCrossingInfo::dump() {
  for (size_t I = 0, N = Block.size(); I < N; ++I) {
    BasicBlock* const B = Mapping.indexToBlock(I);
    dbgs() << B->getName() << ":\n";
    dump("   Consumes", Block[I].Consumes);
    dump("      Kills", Block[I].Kills);
  }
  dbgs() << "\n";
}

SuspendCrossingInfo::SuspendCrossingInfo(Function &F, CoroutineShape &Shape)
    : Mapping(F) {
  const size_t N = Mapping.size();
  Block.resize(N);

  // Initialize every block so that it consumes itself
  for (size_t I = 0; I < N; ++I) {
    auto& B = Block[I];
    B.Consumes.resize(N);
    B.Kills.resize(N);
    B.Consumes.set(I);
  }

  // Mark all CoroEnd Blocks
  for (auto CE : Shape.CoroEnds)
    getBlockData(CE->getParent()).End = true;

  // Mark all suspend blocks and indicate that kill everything they consume
  for (CoroSuspendInst* CSI : Shape.CoroSuspends) {
    CoroSaveInst* const CoroSave = CSI->getCoroSave();
    BasicBlock* const CoroSaveBB = CoroSave->getParent();
    auto &B = getBlockData(CoroSaveBB);
    B.Suspend = true;
    B.Kills |= B.Consumes;
  }

  // Iterate propagating consumes and kills until they stop changing
  int Iteration = 0;

  bool Changed;
  do {
    DEBUG(dbgs() << "iteration " << ++Iteration);
    DEBUG(dbgs() << "==============\n");

    Changed = false;
    for (size_t I = 0; I < N; ++I) {
      auto& B = Block[I];
      for (BasicBlock* SI : successors(B)) {

        auto SuccNo = Mapping.blockToIndex(SI);

        auto& S = Block[SuccNo];
        auto SavedCons = S.Consumes;
        auto SavedKills = S.Kills;

        S.Consumes |= B.Consumes;
        S.Kills |= B.Kills;

        if (B.Suspend) {
          S.Kills |= B.Consumes;
        }
        if (S.Suspend) {
          S.Kills |= S.Consumes;
        }
        else if (S.End) {
          S.Kills.reset();
        }
        else {
          S.Kills.reset(SuccNo);
        }

        Changed |=
          (S.Kills != SavedKills) || (S.Consumes != SavedCons);

        if (S.Kills != SavedKills) {
          DEBUG(dbgs() << "\nblock " << I << " follower " << SI->getName() << "\n");
          DEBUG(dump("s.kills", S.Kills));
          DEBUG(dump("savedKills", SavedKills));
        }
        if (S.Consumes != SavedCons) {
          DEBUG(dbgs() <<"\nblock " << I << " follower " << SI << "\n");
          DEBUG(dump("s.consume", S.Consumes));
          DEBUG(dump("savedCons", SavedCons));
        }
      }
    }
  } while (Changed);
  DEBUG(dump());
}

#undef DEBUG_TYPE // "coro-suspend-crossing"
#define DEBUG_TYPE "coro-frame"

// Split above and below a particular instruction so that it
// is all alone by itself.
static void splitAround(Instruction *I, const Twine &Name) {
  splitBlockIfNotFirst(I, Name);
  splitBlockIfNotFirst(I->getNextNode(), "After" + Name);
}

struct Spill : std::pair<Value*, Instruction*> {
  using base = std::pair<Value*, Instruction*>;

  Spill(Value* Def, User* U) : base(Def, cast<Instruction>(U)) {}

  Value* def() const { return first; }
  Instruction* user() const { return second; }
  BasicBlock* userBlock() const { return second->getParent(); }

  std::pair<Value *, BasicBlock *> getKey() const {
    return{ def(), userBlock() };
  }

  bool operator<(Spill const &rhs) const { return getKey() < rhs.getKey(); }
};

using SpillInfo = SmallVector<Spill, 8>;

static void dump(StringRef Title, SpillInfo const& Spills) {
  dbgs() << "------------- " << Title << "--------------\n";
  Value* CurrentValue = nullptr;
  for (auto const &E : Spills) {
    if (CurrentValue != E.def()) {
      CurrentValue = E.def();
      CurrentValue->dump();
    }
    dbgs() << "   user: "; E.user()->dump();
  }
}

static Instruction* insertSpills(SpillInfo &Spills,
                         CoroutineShape &Shape) {
  auto CB = Shape.CoroBegin;
  IRBuilder<> Builder(CB->getNextNode());
  PointerType* FramePtrTy = Shape.FrameTy->getPointerTo();
  Instruction *FramePtr =
      cast<Instruction>(Builder.CreateBitCast(CB, FramePtrTy, "FramePtr"));
  Type* FrameTy = FramePtrTy->getElementType();

  Value* CurrentValue = nullptr;
  BasicBlock* CurrentBlock = nullptr;
  Value* CurrentReload = nullptr;
  unsigned Index = Shape.PromiseAlloca ? 3 : 2;

  // we need to keep track of any allocas that need "spilling"
  // since they will live in the coroutine frame now, all access to them
  // need to be changed, not just the access across suspend points
  // we remember allocas and their indices to be handled once we processed
  // all the spills

  SmallVector<std::pair<AllocaInst*, unsigned>, 4> Allocas;
  if (Shape.PromiseAlloca)
    Allocas.emplace_back(Shape.PromiseAlloca, 3);

  auto CreateReload = [&](Instruction* InsertBefore) {
    Builder.SetInsertPoint(InsertBefore);
    auto G = Builder.CreateConstInBoundsGEP2_32(
      FrameTy, FramePtr, 0, Index,
      CurrentValue->getName() + Twine(".reload.addr"));
    return isa<AllocaInst>(CurrentValue)
      ? G
      : Builder.CreateLoad(G, CurrentValue->getName() +
        Twine(".reload"));
  };

  for (auto const &E : Spills) {
    // if we have not seen the value, generate a spill
    if (CurrentValue != E.def()) {
      CurrentValue = E.def();
      CurrentBlock = nullptr;
      CurrentReload = nullptr;

      ++Index;

      if (auto AI = dyn_cast<AllocaInst>(CurrentValue)) {
        Allocas.emplace_back(AI, Index);
      } else {
        Builder.SetInsertPoint(
            isa<Argument>(CurrentValue)
                ? FramePtr->getNextNode()
                : dyn_cast<Instruction>(E.def())->getNextNode());

        auto G = Builder.CreateConstInBoundsGEP2_32(FrameTy, FramePtr, 0, Index,
          CurrentValue->getName() + Twine(".spill.addr"));
        Builder.CreateStore(CurrentValue, G);
      }
    }

    // If we have not seen this block, generate a reload.
    if (CurrentBlock != E.userBlock()) {
      CurrentBlock = E.userBlock();
      CurrentReload =
          CreateReload(&*CurrentBlock->getFirstInsertionPt());
    }

    if (auto PN = dyn_cast<PHINode>(E.user())) {
      assert(PN->getNumIncomingValues() == 1 && "unexpected number of incoming "
                                                "values in the PHINode");
      PN->replaceAllUsesWith(CurrentReload);
      PN->eraseFromParent();
      continue;
    }

    // replace all uses of CurrentValue in the current instruction with reload
    for (Use& U : E.user()->operands())
      if (U.get() == CurrentValue)
        U.set(CurrentReload);
  }

  auto FramePtrBB = FramePtr->getParent();
  Shape.AllocaSpillBlock =
      FramePtrBB->splitBasicBlock(FramePtr->getNextNode(), "AllocaSpillBB");
  Shape.AllocaSpillBlock->splitBasicBlock(&Shape.AllocaSpillBlock->front(),
                                          "PostSpill");

  Builder.SetInsertPoint(&Shape.AllocaSpillBlock->front());
  // if we found any allocas, replace all of their remaining uses with Geps
  for (auto& P : Allocas) {
    auto G = Builder.CreateConstInBoundsGEP2_32(FrameTy, FramePtr, 0, P.second);
    G->takeName(P.first);
    P.first->replaceAllUsesWith(G);
    P.first->eraseFromParent();
  }
  return FramePtr;
}

static StructType *buildFrameType(Function &F, CoroutineShape &Shape,
                                  SpillInfo const &Spills) {
  LLVMContext& C = F.getContext();
  SmallString<32> Name(F.getName()); Name.append(".Frame");
  StructType* FrameTy = StructType::create(C, Name);
  auto FramePtrTy = FrameTy->getPointerTo();
  auto FnTy = FunctionType::get(Type::getVoidTy(C), FramePtrTy,
                                /*IsVarArgs=*/false);
  auto FnPtrTy = FnTy->getPointerTo();

  SmallVector<Type*, 8> Types{ FnPtrTy, FnPtrTy, Type::getInt8Ty(C) };
  if (Shape.PromiseAlloca)
    Types.push_back(Shape.PromiseAlloca->getType()->getElementType());

  Value* CurrentDef = nullptr;

  for (auto const& S: Spills) {
    if (CurrentDef == S.def())
      continue;

    CurrentDef = S.def();
    if (CurrentDef == Shape.PromiseAlloca)
      continue;

    Type* Ty = nullptr;
    if (auto AI = dyn_cast<AllocaInst>(CurrentDef))
      Ty = AI->getAllocatedType();
    else
      Ty = CurrentDef->getType();

    Types.push_back(Ty);
  }
  FrameTy->setBody(Types);

  return FrameTy;
}

static bool materializable(Instruction& V) {
  return isa<CastInst>(&V)
    || isa<GetElementPtrInst>(&V)
    || isa<BinaryOperator>(&V)
    || isa<CmpInst>(&V)
    || isa<SelectInst>(&V)
    ;
}

static void rewriteMaterializableInstructions(IRBuilder<> &IRB,
                                              SpillInfo const &Spills) {
  BasicBlock* CurrentBlock = nullptr;
  Instruction* CurrentMaterialization = nullptr;
  Instruction* CurrentDef = nullptr;

  auto CloneInstruction = [&](Instruction* InsertPt) {
    auto ClonedInst = cast<Instruction>(CurrentDef)->clone();
    if (CurrentMaterialization)
      ClonedInst->setName(CurrentMaterialization->getName());
    else {
      ClonedInst->takeName(CurrentDef);
    }
    ClonedInst->insertBefore(InsertPt);
    return ClonedInst;
  };

  for (auto const &E : Spills) {
    if (CurrentDef != E.def()) {
      CurrentDef = cast<Instruction>(E.def());
      CurrentBlock = nullptr;
      CurrentMaterialization = nullptr;
    }

    // if we have not seen this block, materialize the value
    if (CurrentBlock != E.userBlock()) {
      CurrentBlock = E.userBlock();
      CurrentMaterialization =
          CloneInstruction(&*CurrentBlock->getFirstInsertionPt());
    }

    if (auto PN = dyn_cast<PHINode>(E.user())) {
      assert(PN->getNumIncomingValues() == 1 && "unexpected number of incoming "
        "values in the PHINode");
      PN->replaceAllUsesWith(CurrentMaterialization);
      PN->eraseFromParent();
      continue;
    }

    // replace all uses of CurrentValue in the current instruction with reload
    for (Use& U : E.user()->operands())
      if (U.get() == CurrentDef)
        U.set(CurrentMaterialization);
  }
}

static void rewritePHIs(BasicBlock &BB) {
  // For every incoming edge we will create a block holding all
  // incoming values in a single PHI nodes.
  //
  // loop:
  //    %n.val = phi i32[%n, %entry], [%inc, %loop]
  // 
  // It will create:
  //
  // loop.from.entry:
  //    %n.loop.pre = phi i32 [%n, %entry]
  //    br %label loop
  // loop.from.loop:
  //    %inc.loop.pre = phi i32 [%inc, %loop]
  //    br %label loop
  //
  // After this rewrite, further analysis will ignore any phi nodes with more
  // than one incoming edge.

  SmallVector<BasicBlock*, 8> Preds(pred_begin(&BB), pred_end(&BB));
  for (BasicBlock* Pred : Preds) {
    auto IncomingBB = SplitEdge(Pred, &BB);
    IncomingBB->setName(BB.getName() + Twine(".from.") + Pred->getName());
    auto PN = cast<PHINode>(&BB.front());
    do {
      int Index = PN->getBasicBlockIndex(IncomingBB);
      Value* V = PN->getIncomingValue(Index);
      PHINode *InputV = PHINode::Create(
          V->getType(), 1, V->getName() + Twine(".") + BB.getName(),
          &IncomingBB->front());
      InputV->addIncoming(V, Pred);
      PN->setIncomingValue(Index, InputV);
      PN = dyn_cast<PHINode>(PN->getNextNode());
    } while (PN);
  }
}

static void rewritePHIs(Function &F) {
  SmallVector<BasicBlock*, 8> WorkList;

  for (BasicBlock& BB : F)
    if (auto PN = dyn_cast<PHINode>(&BB.front()))
      if (PN->getNumIncomingValues() > 1)
        WorkList.push_back(&BB);

  for (BasicBlock* BB : WorkList)
    rewritePHIs(*BB);
}

// Move early uses of spilled variable after CoroBegin.
// For example, if a parameter had address taken, we may end up with the code
// like:
//        define @f(i32 %n) {
//          %n.addr = alloca i32
//          store %n, %n.addr
//          ...
//          call @coro.begin
//    we need to move the store after coro.begin
static void fixupUses(Function &F, SpillInfo const &Spills,
                      CoroBeginInst *CoroBegin) {
  DominatorTree DT(F);
  SmallVector<Instruction*, 8> NeedsMoving;

  Value* CurrentValue = nullptr;

  for (auto const &E : Spills) {
    if (CurrentValue == E.def())
      continue;

    CurrentValue = E.def();

    for (User* U: CurrentValue->users()) {
      Instruction* I = cast<Instruction>(U);
      if (DT.dominates(I, CoroBegin)) {
        DEBUG({
          for (User* UI : I->users())
            assert(DT.dominates(CoroBegin, cast<Instruction>(UI)) &&
                   "cannot move instruction"
                   " since users are not dominated by CoroBegin");
          dbgs() << "will move: " << *I << "\n";
        });
        NeedsMoving.push_back(I);
      }
    }
  }

  auto InsertPt = CoroBegin->getNextNode();
  for (Instruction* I : NeedsMoving)
    I->moveBefore(InsertPt);
}

void llvm::buildCoroutineFrame(Function &F, CoroutineShape& Shape) {
  Shape.PromiseAlloca = Shape.CoroBegin->getPromise();
  if (Shape.PromiseAlloca) {
    Shape.CoroBegin->clearPromise();
  }

  // Split all of the blocks on CoroSave.
  for (CoroSuspendInst* CSI : Shape.CoroSuspends)
    splitAround(CSI->getCoroSave(), "CoroSave");

  // Put final CoroEnd into its own block.
  splitAround(Shape.CoroEnds.front(), "CoroEnd");
  rewritePHIs(F);

  SuspendCrossingInfo Checker(F, Shape);

  IRBuilder<> Builder(F.getContext());

  SpillInfo Spills;
  // Collect the 

  // See if there are materializable instructions across suspend points.
  for (Instruction& I : instructions(F))
    if (materializable(I))
      for (User* U : I.users())
        if (Checker.definitionAcrossSuspend(I, U))
          Spills.emplace_back(&I, U);

  // Rewrite materializable instructions to be materialized at the use point.
  std::sort(Spills.begin(), Spills.end());
  DEBUG(dump("Materializations", Spills));
  rewriteMaterializableInstructions(Builder, Spills);
  Spills.clear();

  for (Argument& A : F.getArgumentList())
    for (User* U : A.users())
      if (Checker.definitionAcrossSuspend(A, U))
        Spills.emplace_back(&A, U);

  for (Instruction& I : instructions(F)) {
    // token returned by CoroSave is an artifact of how we build save/suspend
    // pairs and should not be part of the Coroutine Frame
    if (isa<CoroSaveInst>(&I))
      continue;
    // CoroBeginInst returns a handle to a coroutine which is passed as a sole
    // parameter to .resume and .cleanup parts and should not go into coroutine
    // frame.
    if (isa<CoroBeginInst>(&I))
      continue;
    if (Shape.PromiseAlloca == &I)
      continue;

    for (User* U : I.users())
      if (Checker.definitionAcrossSuspend(I, U)) {
        assert(!materializable(I) &&
               "rewriteMaterializable did not do its job");
        Spills.emplace_back(&I, U);
      }
  }

  std::sort(Spills.begin(), Spills.end());
  DEBUG(dump("Spills", Spills));
  fixupUses(F, Spills, Shape.CoroBegin);
  Shape.FrameTy = buildFrameType(F, Shape, Spills);
  Shape.FramePtr = insertSpills(Spills, Shape);
}
