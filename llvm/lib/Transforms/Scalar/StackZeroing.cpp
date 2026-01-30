//===- StackZeroing.cpp - Zero stack variables when they go out of scope -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a pass that zeroes all stack-allocated variables when
// they go out of scope, similar to Rust's behavior. This is done by inserting
// llvm.memset intrinsics before llvm.lifetime.end markers or at function exits
// for allocas without explicit lifetime markers.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/StackZeroing.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

#define DEBUG_TYPE "stack-zeroing"

namespace {

/// Helper function to get the size of an alloca in bytes
static std::optional<uint64_t> getAllocaSize(AllocaInst *AI,
                                             const DataLayout &DL) {
  Type *AllocatedType = AI->getAllocatedType();
  if (!AllocatedType->isSized())
    return std::nullopt;
  
  uint64_t TypeSize = DL.getTypeAllocSize(AllocatedType);
  
  // Handle array allocations
  if (AI->isArrayAllocation()) {
    if (auto *CI = dyn_cast<ConstantInt>(AI->getArraySize())) {
      uint64_t NumElements = CI->getZExtValue();
      // Check for overflow
      bool Overflow = false;
      uint64_t TotalSize = SaturatingMultiply(TypeSize, NumElements, &Overflow);
      if (Overflow)
        return std::nullopt;
      return TotalSize;
    }
    // Can't determine size for dynamic allocations
    return std::nullopt;
  }
  
  return TypeSize;
}

/// Insert a memset to zero the given alloca at the specified insertion point
static void insertMemsetZero(AllocaInst *AI, Instruction *InsertBefore,
                             const DataLayout &DL) {
  auto Size = getAllocaSize(AI, DL);
  if (!Size || *Size == 0)
    return;
  
  IRBuilder<> Builder(InsertBefore);
  Type *IntPtrTy = DL.getIntPtrType(AI->getType());
  
  // Create memset(alloca, 0, size)
  Value *ZeroValue = Builder.getInt8(0);
  Value *SizeValue = ConstantInt::get(IntPtrTy, *Size);
  
  Builder.CreateMemSet(AI, ZeroValue, SizeValue, AI->getAlign(), true);
  
  LLVM_DEBUG(dbgs() << "Inserted memset for alloca: " << *AI << "\n");
}

} // anonymous namespace

PreservedAnalyses StackZeroingPass::run(Function &F,
                                         FunctionAnalysisManager &AM) {
  const DataLayout &DL = F.getDataLayout();
  bool Modified = false;

  LLVM_DEBUG(dbgs() << "StackZeroingPass running on function: " << F.getName()
                    << "\n");
  
  // Collect all allocas in the function
  SmallVector<AllocaInst *, 16> Allocas;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        Allocas.push_back(AI);
      }
    }
  }
  
  LLVM_DEBUG(dbgs() << "Found " << Allocas.size() << " allocas\n");
  
  if (Allocas.empty())
    return PreservedAnalyses::all();
  
  // Track which allocas have lifetime.end markers
  DenseMap<AllocaInst *, SmallVector<IntrinsicInst *, 4>> AllocaToLifetimeEnds;
  
  // Find all lifetime.end intrinsics
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
        if (II->getIntrinsicID() == Intrinsic::lifetime_end) {
          // lifetime.end has the form: llvm.lifetime.end(i64 Size, ptr Ptr)
          Value *Ptr = II->getArgOperand(1);
          Ptr = Ptr->stripPointerCasts();

          if (auto *AI = dyn_cast<AllocaInst>(getUnderlyingObject(Ptr)))
            AllocaToLifetimeEnds[AI].push_back(II);
        }
      }
    }
  }
  
  // Insert memsets before lifetime.end markers
  for (auto &Entry : AllocaToLifetimeEnds) {
    AllocaInst *AI = Entry.first;
    for (IntrinsicInst *LifetimeEnd : Entry.second) {
      insertMemsetZero(AI, LifetimeEnd, DL);
      Modified = true;
    }
  }
  
  // For allocas without lifetime.end markers, insert memset before all returns
  SmallVector<AllocaInst *, 16> AllocasWithoutLifetimeEnd;
  for (AllocaInst *AI : Allocas) {
    if (!AllocaToLifetimeEnds.count(AI)) {
      AllocasWithoutLifetimeEnd.push_back(AI);
    }
  }
  
  if (!AllocasWithoutLifetimeEnd.empty()) {
    // Find all return instructions
    SmallVector<ReturnInst *, 8> Returns;
    for (BasicBlock &BB : F) {
      if (auto *RI = dyn_cast<ReturnInst>(BB.getTerminator())) {
        Returns.push_back(RI);
      }
    }
    
    LLVM_DEBUG(dbgs() << "Found " << AllocasWithoutLifetimeEnd.size()
              << " allocas without lifetime.end and " << Returns.size()
              << " returns\n");
    
    // Insert memsets before each return
    for (ReturnInst *RI : Returns) {
      for (AllocaInst *AI : AllocasWithoutLifetimeEnd) {
        insertMemsetZero(AI, RI, DL);
        Modified = true;
      }
    }
  }
  
  if (Modified)
    return PreservedAnalyses::none();
  
  return PreservedAnalyses::all();
}
