//===- CoroSplit4.h - utilities for coroutine passes-------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file provides internal interfaces used to implement coroutine passes.
///
//===----------------------------------------------------------------------===//
#include "CoroutineCommon.h"

namespace llvm {
namespace coro {

  struct CoroutineData {
    StructType* FrameTy;
    PointerType* FramePtrTy;
    FunctionType* ResumeFnTy;
    PointerType* ResumeFnPtrTy;

    struct SubInfo {
      Function* Func;
      Value* Frame;
      Value* vFrame;

      void Init(Function& F, Twine Suffix, CoroutineData& Data);
    };

    SubInfo Ramp;
    SubInfo Resume;
    SubInfo Destroy;
    SubInfo Cleanup;

    CoroutineData(Function& F);
    void split(CoroutineCommon* CC);
  };
}
}