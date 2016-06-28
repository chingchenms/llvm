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

#include "CoroutineCommon.h"

#include <llvm/ADT/BitVector.h>
#include <llvm/ADT/PackedVector.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/circular_raw_ostream.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include "llvm/IR/CFG.h"

#define DEBUG_TYPE "coro-cfb"

using namespace llvm;
using namespace llvm::CoroCommon;

enum { SmallVectorThreshold = 32 };
// Provides two way mapping between the blocks and numbers
class BlockToIndexMapping {
  SmallVector<BasicBlock*, SmallVectorThreshold> V;
public:
  auto size() { return V.size(); }

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
    bool Suspend;
  };
  SmallVector<BlockData, SmallVectorThreshold> Block;

  auto successors(BlockData const& BD) {
    BasicBlock* BB = Mapping.indexToBlock(&BD - &Block[0]);
    return llvm::successors(BB);
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

  bool definitionAcrossSuspend(Argument& A, User* U) {
    BasicBlock* DefBB = &A.getParent()->getEntryBlock();
    BasicBlock* UseBB = cast<Instruction>(U)->getParent();
    return hasPathCrossingSuspendPoint(DefBB, UseBB);
  }

  bool definitionAcrossSuspend(Instruction& I, User* U) {
    BasicBlock* DefBB = I.getParent();
    BasicBlock* UseBB = cast<Instruction>(U)->getParent();
    return hasPathCrossingSuspendPoint(DefBB, UseBB);
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

SuspendCrossingInfo::SuspendCrossingInfo(Function& F, CoroutineShape& Shape) : Mapping(F) {
  const size_t N = Mapping.size();
  Block.resize(N);

  // Initialize every block so that it consumes itself
  for (size_t I = 0; I < N; ++I) {
    auto& B = Block[I];
    B.Consumes.resize(N);
    B.Kills.resize(N);
    B.Consumes.set(I);
  }

  // Mark all suspend blocks and indicate that kill everything they consume
  for (CoroSuspendInst* CSI : Shape.CoroSuspend) {
    CoroSaveInst* const CoroSave = CSI->getCoroSave();
    BasicBlock* const CoroSaveBB = CoroSave->getParent();
    auto &B = Block[Mapping.blockToIndex(CoroSaveBB)];
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

        // Do not propagate beyond Coro.End
        if (SI == Shape.CoroEndFinal.back()->getParent())
          continue;

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
  dump();
}

// Split above and below a particular instruction so that it
// is all alone by itself.
static void splitAround(Instruction *I, const Twine &Name) {
  splitBlockIfNotFirst(I, Name);
  splitBlockIfNotFirst(I->getNextNode(), "After" + Name);
}

void llvm::buildCoroutineFrame(Function &F, CoroutineShape& Shape) {
  DEBUG(dbgs() << "entering buildCoroutineFrame\n");

  // 1 split all of the blocks on CoroSave

  for (CoroSuspendInst* CSI : Shape.CoroSuspend)
    splitAround(CSI->getCoroSave(), "CoroSave");

  // put and CoroEnd into their own blocks
  splitAround(Shape.CoroEndFinal.back(), "CoroEnd");

#if 0 // not sure about that
  //// 2 make coro.begin a fork, jumping to ret block
  //// 3 Split on end(final), so that return block can never enter
#endif
  SuspendCrossingInfo Checker(F, Shape);

  SmallVector<Argument*, 4> ArgsToSpill;
  for (Argument& A : F.getArgumentList())
    for (User* U : A.users())
      if (Checker.definitionAcrossSuspend(A, U))
        ArgsToSpill.push_back(&A);

  SmallVector<Instruction*, 4> ValuesToSpill;
  for (Instruction& I : instructions(F)) {
    // token returned by CoroSave is an artifact of how we build save/suspend
    // pairs and should not be part of the Coroutine Frame
    if (isa<CoroSaveInst>(&I))
      continue;

    for (User* U : I.users())
      if (Checker.definitionAcrossSuspend(I, U)) {
        assert(!I.getType()->isTokenTy() &&
               "tokens cannot be live across suspends");
        ValuesToSpill.push_back(&I);
        break; // no need to scan for more users of this instruction
      }
  }

  DEBUG(dbgs() << "Args to spill:");
  BasicBlock& EntryBB = F.getEntryBlock();
  for (Argument* A : ArgsToSpill)
    DEBUG(dbgs() << " " << A->getName());
  DEBUG(dbgs() << "\n");

  DEBUG(dbgs() << "Values to spill:\n");
  for (Instruction* I : ValuesToSpill)
    DEBUG(I->dump());
  DEBUG(dbgs() << "\n");

  EntryBB; // Do spills and reloads
}
