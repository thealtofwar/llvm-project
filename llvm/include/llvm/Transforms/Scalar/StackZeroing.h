//===- StackZeroing.h - Zero stack variables on scope exit -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides the StackZeroing pass.
//
// The pass inserts memset operations to zero stack-allocated variables when
// they go out of scope (before llvm.lifetime.end intrinsics) and for allocas
// without explicit lifetime markers, before function exits.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_STACKZEROING_H
#define LLVM_TRANSFORMS_SCALAR_STACKZEROING_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Function;

class StackZeroingPass : public PassInfoMixin<StackZeroingPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_STACKZEROING_H
