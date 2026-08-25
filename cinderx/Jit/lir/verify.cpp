// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/python.h"

#include "cinderx/Common/util.h"
#include "cinderx/Jit/lir/dce.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/printer.h"

#include <algorithm>

namespace jit::lir {

bool verifyPostRegAllocInvariants(Function* func, std::ostream& err) {
  auto& blocks = func->basicblocks();
  for (auto iter = blocks.begin(); iter != blocks.end();) {
    auto& block = *iter;
    ++iter;
    auto& succs = block->successors();
    BasicBlock* next_block = iter == blocks.end() ? nullptr : *iter;
    std::unordered_set<BasicBlock*> branched_blocks;
#if PY_VERSION_HEX < 0x030C0000
    if (block->instructions().empty()) {
      const bool falls_through = succs.size() == 1 && next_block != nullptr &&
          succs.front() == next_block &&
          next_block->section() == block->section();
      if (falls_through) {
        continue;
      }
      fmt::print(
          err, "ERROR: Basic block {} has no instructions.\n", block->id());
      return false;
    }

    auto recordBranchTarget = [&](const Instruction* instr, size_t input_idx) {
      const OperandBase* operand = instr->getInput(input_idx);
      if (operand == nullptr || operand->isLinked() || !operand->isLabel()) {
        fmt::print(
            err,
            "ERROR: Branch target in block {} must be a label.\n",
            block->id());
        return false;
      }

      const auto* label = static_cast<const Operand*>(operand);
      if (label->hasAsmLabel()) {
        // An AsmJit label is not represented by a CFG successor.
        return true;
      }
      BasicBlock* target = label->getBasicBlock();
      if (target == nullptr) {
        fmt::print(
            err,
            "ERROR: Branch target in block {} must not be null.\n",
            block->id());
        return false;
      }
      branched_blocks.insert(target);
      return true;
    };

    for (auto& instr : block->instructions()) {
      if (instr->opcode() == Instruction::kPhi && instr->getNumInputs() == 0) {
        fmt::print(
            err, "ERROR: Basic block {} contains an empty Phi.\n", block->id());
        return false;
      }
      if (instr->isCondBranch()) {
        fmt::print(
            err,
            "ERROR: CondBranch in block {} must be rewritten before "
            "post-regalloc verification.\n",
            block->id());
        return false;
      }

      if (instr->isBranch()) {
        if (instr->getNumInputs() != 1) {
          fmt::print(
              err,
              "ERROR: Branch in block {} must have exactly one input.\n",
              block->id());
          return false;
        }
        const OperandBase* target = instr->getInput(0);
        if (target == nullptr) {
          fmt::print(
              err,
              "ERROR: Branch target in block {} must not be null.\n",
              block->id());
          return false;
        }
        if (target->isInd() || target->isImm() || target->isReg()) {
          // Indirect or direct-address branch: no CFG successor to verify.
          continue;
        }
        if (!recordBranchTarget(instr.get(), 0)) {
          return false;
        }
      } else if (instr->isBranchCC()) {
        size_t label_input_idx = 0;
        const size_t num_inputs = instr->getNumInputs();
        if (num_inputs == 2) {
#if defined(CINDER_AARCH64)
          const OperandBase* reg_input = instr->getInput(0);
          if (reg_input == nullptr ||
              (instr->opcode() != Instruction::kBranchZ &&
               instr->opcode() != Instruction::kBranchNZ) ||
              !reg_input->isReg() ||
              (reg_input->dataType() != OperandBase::k32bit &&
               reg_input->dataType() != OperandBase::k64bit &&
               reg_input->dataType() != OperandBase::kObject)) {
            fmt::print(
                err,
                "ERROR: Two-input branch in block {} is not a legal "
                "AArch64 register-tested BranchZ/BranchNZ.\n",
                block->id());
            return false;
          }
#else
          fmt::print(
              err,
              "ERROR: Two-input branch in block {} is only valid on "
              "AArch64.\n",
              block->id());
          return false;
#endif
          label_input_idx = 1;
        } else if (num_inputs != 1) {
          fmt::print(
              err,
              "ERROR: Conditional branch in block {} must have one or two "
              "inputs.\n",
              block->id());
          return false;
        }
        if (!recordBranchTarget(instr.get(), label_input_idx)) {
          return false;
        }
      } else if (instr->isBranchBitSet() || instr->isBranchBitNotSet()) {
        if (instr->getNumInputs() != 3) {
          fmt::print(
              err,
              "ERROR: BranchBitSet/BranchBitNotSet in block {} must have "
              "value, bit, and label inputs.\n",
              block->id());
          return false;
        }
        if (!recordBranchTarget(instr.get(), 2)) {
          return false;
        }
      } else if (instr->isCmpBranch()) {
        if (instr->getNumInputs() != 2) {
          fmt::print(
              err,
              "ERROR: CmpBranch in block {} must have register and label "
              "inputs.\n",
              block->id());
          return false;
        }
        if (!recordBranchTarget(instr.get(), 1)) {
          return false;
        }
      }
    }

    const Instruction* last = block->instructions().back().get();
    const bool is_always_fail_guard = last->isGuard() &&
        last->getNumInputs() > 0 && last->getInput(0)->isImm() &&
        last->getInput(0)->getConstant() == InstrGuardKind::kAlwaysFail;
    const bool ends_control_flow = last->isBranch() || last->isTerminator() ||
        last->isUnreachable() || last->isRet() || is_always_fail_guard;
    const bool falls_through = next_block != nullptr &&
        next_block->section() == block->section() &&
        std::find(succs.begin(), succs.end(), next_block) != succs.end();
    if (!ends_control_flow && !falls_through) {
      fmt::print(
          err,
          "ERROR: Basic block {} has no terminator or valid fallthrough.\n",
          block->id());
      return false;
    }
#else
    for (auto& instr : block->instructions()) {
      if (instr->isBranch() || instr->isBranchCC() || instr->isBranchBitSet() ||
          instr->isBranchBitNotSet()) {
        size_t label_input_idx = 0;
        if (instr->isBranchBitSet() || instr->isBranchBitNotSet()) {
          JIT_DCHECK(
              instr->getNumInputs() == 3,
              "BranchBitSet/BranchBitNotSet must have value, bit, and label "
              "inputs.");
          label_input_idx = 2;
        } else {
          auto num_inputs = instr->getNumInputs();
          JIT_DCHECK(num_inputs > 0, "Branch must have at least one input.");
          if (num_inputs == 2) {
#if defined(CINDER_AARCH64)
            auto reg_input = instr->getInput(0);
            auto reg_type = reg_input->dataType();
            JIT_DCHECK(
                (instr->opcode() == Instruction::kBranchZ ||
                 instr->opcode() == Instruction::kBranchNZ) &&
                    reg_input->isReg() &&
                    (reg_type == OperandBase::k32bit ||
                     reg_type == OperandBase::k64bit ||
                     reg_type == OperandBase::kObject),
                "Two-input branches must be AArch64 register-tested "
                "BranchZ/BranchNZ.");
#else
            JIT_DCHECK(false, "Two-input branches are only valid on AArch64.");
#endif
          } else {
            JIT_DCHECK(num_inputs == 1, "Branch must have one or two inputs.");
          }
          label_input_idx = num_inputs - 1;
        }
        auto operand = instr->getInput(label_input_idx);
        if (label_input_idx == 0 &&
            (operand->isInd() || operand->isImm() || operand->isReg())) {
          continue;
        }
        JIT_DCHECK(
            operand->type() == OperandBase::kLabel,
            "Branch must jump to a label.");
        branched_blocks.insert(operand->getBasicBlock());
      } else if (instr->isCmpBranch()) {
        JIT_DCHECK(
            instr->getNumInputs() == 2,
            "CmpBranch must have register and label inputs.");
        auto operand = instr->getInput(1);
        JIT_DCHECK(
            operand->type() == OperandBase::kLabel,
            "CmpBranch second input must be a label.");
        branched_blocks.insert(operand->getBasicBlock());
      }
    }
#endif

    for (const auto& succ : succs) {
      // Go through the instructions and ensure that each successor has a
      // matching jump.
      if (succ == next_block && next_block->section() == block->section()) {
        // If a successor is physically the next block in the block order and
        // the blocks are emitted to the same section, we don't need a branch.
        continue;
      }
      // Ensure that a jump to the successor exists.
      if (!branched_blocks.contains(succ)) {
        fmt::print(
            err,
            "ERROR: Basic block {} does not contain a jump to non-immediate "
            "successor {}.\n",
            block->id(),
            succ->id());
        return false;
      }
    }
  }
  return true;
}

} // namespace jit::lir
