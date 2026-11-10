// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/aarch64_peephole.h"

#if defined(CINDER_AARCH64)

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/operand.h"

#include <iterator>
#include <utility>

namespace jit::lir {

namespace {

using namespace jit::codegen;

bool supportsZeroRegisterStore(DataType data_type) {
  switch (data_type) {
    case DataType::k8bit:
    case DataType::k16bit:
    case DataType::k32bit:
    case DataType::k64bit:
    case DataType::kObject:
      return true;
    case DataType::kDouble:
      return false;
  }
  return false;
}

bool operandUsesRegister(
    const OperandBase* operand,
    PhyLocation physical_register) {
  return operand != nullptr && operand->isReg() &&
      operand->getPhyRegister() == physical_register;
}

bool indirectAddressUsesRegister(
    const OperandBase* output,
    PhyLocation physical_register) {
  auto* address = output->getMemoryIndirect();
  return operandUsesRegister(address->getBaseRegOperand(), physical_register) ||
      operandUsesRegister(address->getIndexRegOperand(), physical_register);
}

bool isSubWordZeroStoreShape(
    DataType materialization_type,
    DataType stored_value_type,
    DataType store_type) {
  return materialization_type == DataType::k32bit &&
      (store_type == DataType::k8bit || store_type == DataType::k16bit) &&
      stored_value_type == store_type;
}

RewriteResult rewriteZeroMaterializationStore(instr_iter_t instr_iter) {
  auto* materialize = instr_iter->get();
  if (!materialize->isMove() || materialize->getNumInputs() != 1 ||
      !materialize->output()->isReg()) {
    return kUnchanged;
  }

  auto* zero = materialize->getInput(0);
  if (!zero->isImm() || zero->isLinked() || zero->isFp() ||
      zero->getConstant() != 0) {
    return kUnchanged;
  }

  auto* block = materialize->basicblock();
  auto store_iter = std::next(instr_iter);
  if (store_iter == block->instructions().end()) {
    return kUnchanged;
  }

  auto* store = store_iter->get();
  if (!store->isMove() || store->getNumInputs() != 1 ||
      !store->output()->isInd()) {
    return kUnchanged;
  }

  auto* stored_value = store->getInput(0);
  auto materialized_register = materialize->output()->getPhyRegister();
  if (!stored_value->isReg() || stored_value->isLinked() ||
      stored_value->isFp() || !stored_value->isLastUse() ||
      stored_value->getPhyRegister() != materialized_register) {
    return kUnchanged;
  }

  auto store_type = store->output()->dataType();
  auto materialization_type = materialize->output()->dataType();
  auto stored_value_type = stored_value->dataType();
  bool exact_width = bitSize(materialization_type) == bitSize(store_type) &&
      bitSize(stored_value_type) == bitSize(store_type);
  bool subword_zero_store = isSubWordZeroStoreShape(
      materialization_type, stored_value_type, store_type);
  if ((!exact_width && !subword_zero_store) ||
      indirectAddressUsesRegister(store->output(), materialized_register)) {
    return kUnchanged;
  }

  if (!supportsZeroRegisterStore(store_type)) {
    return kUnchanged;
  }

  // Preserve zero as an immediate in LIR.  Register number 31 is
  // instruction-context-sensitive on AArch64, so representing XZR/WZR as an
  // ordinary physical register can be interpreted as SP by the assembler.
  // The AArch64 Move lowering recognizes this canonical zero-store shape and
  // emits the architectural zero register explicitly.
  auto* concrete_value = static_cast<Operand*>(stored_value);
  concrete_value->setConstant(0, store_type);

  // The stored value was the materialized register's last use, and the
  // immediately following store does not use that register in its address.
  // It is therefore safe to remove the now-dead zero materialization.
  block->removeInstr(instr_iter);
  return kRemoved;
}

struct PairableStore {
  OperandBase* base;
  OperandBase* value;
  int32_t offset;
};

bool isPointerWidth(DataType data_type) {
  return data_type == DataType::k64bit || data_type == DataType::kObject;
}

struct PairableLoad {
  OperandBase* base;
  PhyLocation destination;
  DataType base_type;
  DataType destination_type;
  int32_t offset;
};

bool isValidLoadPairBase(const OperandBase* base) {
  if (base == nullptr || !base->isReg() || base->isLinked() ||
      !isPointerWidth(base->dataType())) {
    return false;
  }

  auto physical_register = base->getPhyRegister();
  return physical_register == SP ||
      (physical_register.is_gp_register() && physical_register != XZR);
}

bool getPairableLoad(Instruction* instr, PairableLoad& result) {
  if (instr->opcode() != Instruction::kMove || instr->getNumInputs() != 1 ||
      !instr->output()->isReg() ||
      !isPointerWidth(instr->output()->dataType())) {
    return false;
  }

  auto destination = instr->output()->getPhyRegister();
  if (!destination.is_gp_register() || destination == X29 ||
      destination == XZR) {
    return false;
  }

  auto* memory = instr->getInput(0);
  if (!memory->isInd() || !isPointerWidth(memory->dataType())) {
    return false;
  }

  auto* address = memory->getMemoryIndirect();
  auto* base = address->getBaseRegOperand();
  if (!isValidLoadPairBase(base) || address->getIndexRegOperand() != nullptr) {
    return false;
  }

  result = PairableLoad{
      base,
      destination,
      base->dataType(),
      instr->output()->dataType(),
      address->getOffset()};
  return true;
}

bool canPairLoads(
    Instruction* first,
    Instruction* second,
    PairableLoad& first_load,
    PairableLoad& second_load) {
  if (!getPairableLoad(first, first_load) ||
      !getPairableLoad(second, second_load) ||
      first_load.base->getPhyRegister() != second_load.base->getPhyRegister()) {
    return false;
  }

  // LDP names destinations in increasing address order.  Scalar loads may
  // appear in either order; neither destination may overwrite the base.
  if (first_load.offset > second_load.offset) {
    std::swap(first_load, second_load);
  }

  const int64_t lower_offset = first_load.offset;
  const int64_t upper_offset = second_load.offset;
  if (upper_offset - lower_offset != kPointerSize ||
      lower_offset % kPointerSize != 0 || lower_offset < -64 * kPointerSize ||
      lower_offset > 63 * kPointerSize) {
    return false;
  }

  auto base_register = first_load.base->getPhyRegister();
  return first_load.destination != second_load.destination &&
      first_load.destination != base_register &&
      second_load.destination != base_register;
}

RewriteResult rewriteAdjacentLoadsToLoadPair(instr_iter_t instr_iter) {
  auto* first = instr_iter->get();
  auto* block = first->basicblock();
  auto second_iter = std::next(instr_iter);
  if (second_iter == block->instructions().end()) {
    return kUnchanged;
  }

  auto* second = second_iter->get();
  PairableLoad first_load{};
  PairableLoad second_load{};
  if (!canPairLoads(first, second, first_load, second_load)) {
    return kUnchanged;
  }

  const int32_t lower_offset = first_load.offset;
  const auto base_register = first_load.base->getPhyRegister();

  // Keep the second load's origin while replacing the two adjacent scalar
  // loads. kLoadPair's final two inputs are post-RA-only hidden definitions,
  // not ordinary LIR uses.
  while (second->getNumInputs() > 0) {
    second->removeInput(second->getNumInputs() - 1);
  }
  second->setOpcode(Instruction::kLoadPair);
  second->output()->setNone();
  second->allocateImmediateInput(
      static_cast<uint64_t>(static_cast<int64_t>(lower_offset)),
      DataType::k64bit);
  second->allocatePhyRegisterInput(base_register)
      ->setDataType(first_load.base_type);
  second->allocatePhyRegisterInput(first_load.destination)
      ->setDataType(first_load.destination_type);
  second->allocatePhyRegisterInput(second_load.destination)
      ->setDataType(second_load.destination_type);

  block->removeInstr(instr_iter);
  return kRemoved;
}

bool getPairableStore(Instruction* instr, PairableStore& result) {
  if (!instr->isMove() || instr->getNumInputs() != 1 ||
      !instr->output()->isInd() ||
      !isPointerWidth(instr->output()->dataType())) {
    return false;
  }

  auto* value = instr->getInput(0);
  if (!value->isReg() || value->isLinked() || value->isFp() ||
      !isPointerWidth(value->dataType())) {
    return false;
  }

  auto* address = instr->output()->getMemoryIndirect();
  auto* base = address->getBaseRegOperand();
  if (base == nullptr || !base->isReg() || base->isLinked() || base->isFp() ||
      base->dataType() != DataType::kObject ||
      address->getIndexRegOperand() != nullptr) {
    return false;
  }

  result = PairableStore{base, value, address->getOffset()};
  return true;
}

bool canPairStores(
    Instruction* first,
    Instruction* second,
    PairableStore& first_store,
    PairableStore& second_store) {
  if (!getPairableStore(first, first_store) ||
      !getPairableStore(second, second_store) ||
      first_store.base->getPhyRegister() !=
          second_store.base->getPhyRegister()) {
    return false;
  }

  // Preserve the source store order.  Reversing an upper-then-lower pair
  // would produce the same final bytes but could change externally observable
  // memory ordering, so only fold the lower-then-upper form.
  const int64_t lower_offset = first_store.offset;
  const int64_t upper_offset = second_store.offset;
  if (upper_offset - lower_offset != kPointerSize ||
      lower_offset % (2 * kPointerSize) != 0 ||
      lower_offset < -64 * kPointerSize || lower_offset > 63 * kPointerSize) {
    return false;
  }

  auto base_register = first_store.base->getPhyRegister();
  if (first_store.value->getPhyRegister() == SP ||
      second_store.value->getPhyRegister() == SP ||
      first_store.value->getPhyRegister() == base_register ||
      second_store.value->getPhyRegister() == base_register) {
    return false;
  }

  return true;
}

RewriteResult rewriteAdjacentStoresToStorePair(instr_iter_t instr_iter) {
  auto* first = instr_iter->get();
  auto* block = first->basicblock();
  auto second_iter = std::next(instr_iter);
  if (second_iter == block->instructions().end()) {
    return kUnchanged;
  }

  auto* second = second_iter->get();
  PairableStore first_store{};
  PairableStore second_store{};
  if (!canPairStores(first, second, first_store, second_store)) {
    return kUnchanged;
  }

  int32_t lower_offset = first_store.offset;

  auto base_register = first_store.base->getPhyRegister();
  auto base_type = first_store.base->dataType();
  auto lower_register = first_store.value->getPhyRegister();
  auto lower_type = first_store.value->dataType();
  auto upper_register = second_store.value->getPhyRegister();
  auto upper_type = second_store.value->dataType();

  // Mutate the second store before removing the current instruction.  The
  // rewrite walker has already advanced its iterator to the second store, so
  // this preserves iterator validity.
  while (second->getNumInputs() > 0) {
    second->removeInput(second->getNumInputs() - 1);
  }
  second->setOpcode(Instruction::kStorePair);
  second->output()->setNone();
  second->allocateImmediateInput(
      static_cast<uint64_t>(static_cast<int64_t>(lower_offset)),
      DataType::k64bit);
  second->allocatePhyRegisterInput(base_register)->setDataType(base_type);
  second->allocatePhyRegisterInput(lower_register)->setDataType(lower_type);
  second->allocatePhyRegisterInput(upper_register)->setDataType(upper_type);

  block->removeInstr(instr_iter);
  return kRemoved;
}

bool sameNonZeroImmediateMaterialization(
    const Instruction* first,
    const Instruction* second) {
  if (!first->isMove() || !second->isMove() || !first->output()->isReg() ||
      !second->output()->isReg() || first->getNumInputs() != 1 ||
      second->getNumInputs() != 1) {
    return false;
  }

  auto* first_value = first->getInput(0);
  auto* second_value = second->getInput(0);
  return first_value->isImm() && second_value->isImm() &&
      !first_value->isLinked() && !second_value->isLinked() &&
      !first_value->isFp() && !second_value->isFp() &&
      first_value->getConstant() != 0 &&
      first_value->getConstant() == second_value->getConstant() &&
      first_value->dataType() == second_value->dataType() &&
      first_value->dataType() == first->output()->dataType() &&
      isPointerWidth(first->output()->dataType()) &&
      first->output()->dataType() == second->output()->dataType() &&
      first->output()->getPhyRegister() == second->output()->getPhyRegister();
}

RewriteResult rewriteConstantFillForStorePair(instr_iter_t instr_iter) {
  auto* first_materialization = instr_iter->get();
  auto* block = first_materialization->basicblock();

  auto first_store_iter = std::next(instr_iter);
  if (first_store_iter == block->instructions().end()) {
    return kUnchanged;
  }
  auto second_materialization_iter = std::next(first_store_iter);
  if (second_materialization_iter == block->instructions().end()) {
    return kUnchanged;
  }
  auto second_store_iter = std::next(second_materialization_iter);
  if (second_store_iter == block->instructions().end()) {
    return kUnchanged;
  }

  auto* first_store_instr = first_store_iter->get();
  auto* second_materialization = second_materialization_iter->get();
  auto* second_store_instr = second_store_iter->get();
  if (!sameNonZeroImmediateMaterialization(
          first_materialization, second_materialization)) {
    return kUnchanged;
  }

  PairableStore first_store{};
  PairableStore second_store{};
  if (!canPairStores(
          first_store_instr, second_store_instr, first_store, second_store)) {
    return kUnchanged;
  }

  auto materialized_register =
      first_materialization->output()->getPhyRegister();
  if (first_store.value->getPhyRegister() != materialized_register ||
      second_store.value->getPhyRegister() != materialized_register) {
    return kUnchanged;
  }

  // Removing only the second rematerialization makes the two stores adjacent.
  // The same stage's store-pair rule will then combine them.  This rule is not
  // a general redundant-Move optimization: it is enabled only when the whole
  // four-instruction window is a legal AArch64 STP candidate.
  block->removeInstr(second_materialization_iter);
  return kChanged;
}

RewriteResult rewriteAArch64Peepholes(instr_iter_t instr_iter) {
  auto* instr = instr_iter->get();

  // Keep this as the module's single dispatcher.  Future rules must first
  // partition on opcode and operand shape so unrelated rules are not tried for
  // every LIR instruction.
  switch (instr->opcode()) {
    case Instruction::kMove: {
      if (instr->output()->isReg()) {
        if (auto result = rewriteAdjacentLoadsToLoadPair(instr_iter);
            result != kUnchanged) {
          return result;
        }
        if (auto result = rewriteZeroMaterializationStore(instr_iter);
            result != kUnchanged) {
          return result;
        }
        return rewriteConstantFillForStorePair(instr_iter);
      }
      if (instr->output()->isInd()) {
        return rewriteAdjacentStoresToStorePair(instr_iter);
      }
      return kUnchanged;
    }
    case Instruction::kLoadPair:
      // A post-RA pseudo with hidden physical definitions. Treat it as an
      // opaque barrier rather than attempting to reinterpret its inputs.
      return kUnchanged;
    default:
      return kUnchanged;
  }
}

} // namespace

void registerAArch64PeepholeRewrites(Rewrite& rewrite) {
  // Reuse the existing move-optimization stage instead of adding another
  // pipeline pass or another whole-function traversal.
  rewrite.registerOneRewriteFunction(rewriteAArch64Peepholes, 1);
}

} // namespace jit::lir

#endif // CINDER_AARCH64
