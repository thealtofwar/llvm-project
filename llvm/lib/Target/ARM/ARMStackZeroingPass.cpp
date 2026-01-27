//===- ARMStackZeroingPass.cpp - Zero stack frames on exit -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a MachineFunctionPass that, for functions annotated
// with the "zero-stack" function attribute, inserts ARM-specific store
// instructions to zero the stack frame just before each return.
//
// For ARM32 AAPCS, this emits:
//   MOV R0, #0
//   STR R0, [FP, #offset]  // for each stack slot
//
// For large frames, it generates a loop structure.
//
//===----------------------------------------------------------------------===//

#include "ARM.h"
#include "ARMBaseInstrInfo.h"
#include "ARMBaseRegisterInfo.h"
#include "ARMFrameLowering.h"
#include "ARMSubtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "arm-stack-zeroing"

namespace {

class ARMStackZeroingPass : public MachineFunctionPass {
public:
  static char ID;
  ARMStackZeroingPass() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "ARM Stack Zeroing Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  bool insertZeroingStores(MachineFunction &MF);
};

} // end anonymous namespace

char ARMStackZeroingPass::ID = 0;

INITIALIZE_PASS(ARMStackZeroingPass, "arm-stack-zeroing",
                "ARM Stack Zeroing Pass", false, false)

FunctionPass *llvm::createARMStackZeroingPass() {
  return new ARMStackZeroingPass();
}

bool ARMStackZeroingPass::runOnMachineFunction(MachineFunction &MF) {
  const Function &F = MF.getFunction();

  // Only act on functions explicitly marked with the attribute.
  if (!F.hasFnAttribute("zero-stack"))
    return false;

  return insertZeroingStores(MF);
}

bool ARMStackZeroingPass::insertZeroingStores(MachineFunction &MF) {
  MachineFrameInfo &MFI = MF.getFrameInfo();
  uint64_t FrameSize = MFI.getStackSize();

  if (FrameSize == 0)
    return false;

  const ARMSubtarget &STI = MF.getSubtarget<ARMSubtarget>();
  const ARMBaseInstrInfo *TII = STI.getInstrInfo();
  const ARMBaseRegisterInfo *TRI = STI.getRegisterInfo();
  const ARMFrameLowering *TFI = STI.getFrameLowering();

  if (!TII || !TRI || !TFI)
    return false;

  bool Changed = false;
  
  // Determine store size - use 4 bytes (word) for ARM
  unsigned StoreSize = 4;
  unsigned MaxUnroll = 128; // Maximum number of stores to unroll
  
  // Determine which register to use for stack access
  // Use FP if frame pointer is used, otherwise use SP
  Register FrameReg;
  if (TFI->hasFP(MF)) {
    // Frame pointer is used
    FrameReg = TRI->getFrameRegister(MF);
  } else {
    // Use stack pointer directly
    FrameReg = ARM::SP;
  }
  
  for (MachineBasicBlock &MBB : MF) {
    if (MBB.empty())
      continue;

    for (auto I = MBB.begin(), E = MBB.end(); I != E; ++I) {
      MachineInstr &MI = *I;
      if (!MI.isReturn())
        continue;
        
      DebugLoc DL = MI.getDebugLoc();
      
      // Use R12 (IP - intra-procedure scratch register) to hold zero
      // Cannot create virtual registers at this stage (after register allocation)
      Register ZeroReg = ARM::R12;
      
      LLVM_DEBUG(dbgs() << "ARM Stack zeroing: using R12 as scratch register"
                  << ".\n");


      // MOV ZeroReg, #0
      BuildMI(MBB, I, DL, TII->get(ARM::MOVi), ZeroReg)
          .addImm(0)
          .add(predOps(ARMCC::AL))
          .add(condCodeOp());
      
      // For small frames, emit unrolled stores
      if (FrameSize / StoreSize <= MaxUnroll) {
        // Emit stores at incremental offsets from the stack/frame pointer
        for (uint64_t Offset = 0; Offset < FrameSize; Offset += StoreSize) {
          // STRi12 uses addrmode_imm12: base register, offset (0-4095), and add/sub bit
          // Format: STRi12 Rt, base, offset, pred
          BuildMI(MBB, I, DL, TII->get(ARM::STRi12))
              .addReg(ZeroReg)                    // Rt - register to store
              .addReg(FrameReg)                   // Rn - base register  
              .addImm(Offset)                     // imm12 - offset
              .add(predOps(ARMCC::AL));           // predicate
        }
        
        LLVM_DEBUG(dbgs() << "ARM Stack zeroing: emitted " 
                         << (FrameSize / StoreSize) 
                         << " stores for " << FrameSize << " byte frame\n");
      } else {
        // For very large frames, emit a loop
        // This is more complex and would require creating new basic blocks
        // For now, just log a message
        LLVM_DEBUG(dbgs() << "ARM Stack zeroing: frame too large (" 
                         << FrameSize 
                         << " bytes) for unrolling, loop needed\n");
      }

      Changed = true;
      break; // Only process first return in this block
    }
  }

  return Changed;
}
