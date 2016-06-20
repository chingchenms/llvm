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
#include <llvm/Support/Debug.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>

#define DEBUG_TYPE "coro-cfb"

namespace llvm {

class BasicBlock;

enum { SmallVectorThreshold = 32 };
// Provides two way mapping between the blocks and numbers
class BlockNumbering {
  SmallVector<BasicBlock*, SmallVectorThreshold> V;
public:
  BlockNumbering(Function& F) {
    for (BasicBlock & BB : F)
      V.push_back(&BB);
    std::sort(V.begin(), V.end());
  }

  // looks up block #
  size_t operator[](BasicBlock* BB) const {
    auto I = std::lower_bound(V.begin(), V.end(), BB);
    assert(I != V.end() && *I == BB && "BasicBlockNumberng: Unknown block");
    return I - V.begin();
  }
  auto size() { return V.size(); }

  // lookup by number
  BasicBlock* operator[](unsigned Index) { return V[Index]; }
};

struct Builder {
  BlockNumbering BlockNumber;

  struct BlockData {
    BitVector consumes;
    BitVector kills;
    bool suspend;
  };
  SmallVector<BlockData, SmallVectorThreshold> Block;

  Builder(Function& F, CoroutineShape& Shape);
};

Builder::Builder(Function& F, CoroutineShape& Shape) : BlockNumber(F) {
  const size_t N = BlockNumber.size();
  Block.resize(N);

  // Initialize every block so that it consumes itself
  for (size_t I = 0; I < N; ++I) {
    auto& B = Block[I];
    B.consumes.set(I);
    if (B.suspend)
      B.kills.set(I);
  }

  // Mark all suspend blocks and indicate that kill everything they consume
  for (CoroSuspendInst* CSI : Shape.CoroSuspend) {
    CoroSaveInst* const CoroSave = CSI->getCoroSave();
    BasicBlock* const CoroSaveBB = CoroSave->getParent();
    BasicBlock* const SuspendBB = CoroSaveBB->getSingleSuccessor();
    auto &B = Block[BlockNumber[SuspendBB]];
    B.suspend = true;
    B.kills |= B.consumes;
  }
}

void buildCoroutineFrame(Function &F, CoroutineShape& Shape) {
  DEBUG(dbgs() << "entering buildCoroutineFrame\n");

  // 1 split all of the blocks on CoroSave

  for (CoroSuspendInst* CSI : Shape.CoroSuspend) {
    CoroSaveInst* CoroSave = CSI->getCoroSave();
    CoroSave->getParent()->splitBasicBlock(CoroSave->getNextNode());
  }

  // 2 make coro.begin a fork, jumping to ret block
  // 3 Split on end(final), so that return block can never enter
}

} // namespace llvm
