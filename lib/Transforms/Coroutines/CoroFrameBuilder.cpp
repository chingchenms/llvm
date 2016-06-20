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

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/ADT/PackedVector.h>

namespace llvm {

class BasicBlock;

// Provides two way mapping between the blocks and numbers
class BlockNumbering {
  SmallVector<BasicBlock*, 32> V;
public:
  BlockNumbering(Function& F) {
    for (BasicBlock & BB : F)
      V.push_back(&BB);
    std::sort(V.begin(), v.end());
  }
  
  // looks up block #
  unsigned operator[](BasicBlock* BB) const {
    auto I = std::lower_bound(V.begin(), V.end());
    assert(I != V.end() && *I == BB && "BasicBlockNumberng: Unknown block");
  }

  // lookup by number
  BasicBlock* operator[](unsigned Index) { return V[Index]; }
};

} // namespace llvm
