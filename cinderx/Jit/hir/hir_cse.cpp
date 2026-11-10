// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/hir_cse.h"

#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/copy_propagation.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/ssa.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace jit::hir {

namespace {

struct ExprKey {
  Opcode opcode;
  uint64_t aux;
  Type type;
  Register* a;
  Register* b;

  bool operator==(const ExprKey& other) const {
    return opcode == other.opcode && aux == other.aux && type == other.type &&
        a == other.a && b == other.b;
  }
};

struct ExprKeyHash {
  std::size_t operator()(const ExprKey& key) const {
    std::size_t h = std::hash<int>{}(static_cast<int>(key.opcode));
    auto mix = [&h](std::size_t v) {
      h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };
    mix(std::hash<uint64_t>{}(key.aux));
    mix(std::hash<Type>{}(key.type));
    mix(std::hash<Register*>{}(key.a));
    mix(std::hash<Register*>{}(key.b));
    return h;
  }
};

// Pure instructions eligible for CSE.  Every operand is an immutable value
// (exact int / primitive), so the result depends on the operands alone.
std::optional<ExprKey> keyFor(const Instr& instr) {
  switch (instr.opcode()) {
    case Opcode::kIsCompactLong:
    case Opcode::kCompactLongUnbox:
      return ExprKey{
          instr.opcode(),
          0,
          TBottom,
          chaseAssignOperand(instr.GetOperand(0)),
          nullptr};
    case Opcode::kPrimitiveUnbox: {
      auto& unbox = static_cast<const PrimitiveUnbox&>(instr);
      return ExprKey{
          instr.opcode(),
          0,
          unbox.type(),
          chaseAssignOperand(unbox.value()),
          nullptr};
    }
    case Opcode::kPrimitiveCompare: {
      auto& cmp = static_cast<const PrimitiveCompare&>(instr);
      return ExprKey{
          instr.opcode(),
          static_cast<uint64_t>(cmp.op()),
          TBottom,
          chaseAssignOperand(cmp.left()),
          chaseAssignOperand(cmp.right())};
    }
    case Opcode::kIntBinaryOp: {
      auto& bin = static_cast<const IntBinaryOp&>(instr);
      return ExprKey{
          instr.opcode(),
          static_cast<uint64_t>(bin.op()),
          TBottom,
          chaseAssignOperand(bin.left()),
          chaseAssignOperand(bin.right())};
    }
    case Opcode::kPrimitiveUnaryOp: {
      auto& un = static_cast<const PrimitiveUnaryOp&>(instr);
      return ExprKey{
          instr.opcode(),
          static_cast<uint64_t>(un.op()),
          TBottom,
          chaseAssignOperand(un.value()),
          nullptr};
    }
    default:
      return std::nullopt;
  }
}

} // namespace

void HirCSE::Run(Function& irfunc) {
  if (irfunc.cfg.entry_block == nullptr) {
    return;
  }

  DominatorAnalysis doms(irfunc);
  std::unordered_map<const BasicBlock*, std::vector<BasicBlock*>> children;
  for (auto& block : irfunc.cfg.blocks) {
    const BasicBlock* idom = doms.immediateDominator(&block);
    if (idom != nullptr) {
      children[idom].push_back(&block);
    }
  }

  std::unordered_map<ExprKey, Register*, ExprKeyHash> available;
  std::unordered_set<Register*> guarded;
  std::vector<ExprKey> expr_undo;
  std::vector<Register*> guard_undo;
  std::vector<std::unique_ptr<Instr>> removed;
  bool changed = false;

  struct Frame {
    BasicBlock* block;
    std::size_t expr_mark;
    std::size_t guard_mark;
    std::size_t next_child;
  };

  auto process = [&](BasicBlock* block) {
    for (auto it = block->begin(); it != block->end();) {
      Instr& instr = *it;
      ++it;
      if (instr.IsGuard()) {
        // An explicit nonce identifies an independently observable deopt site.
        if (instr.asDeoptBase()->nonce() != -1) {
          continue;
        }
        Register* cond = chaseAssignOperand(instr.GetOperand(0));
        if (guarded.contains(cond)) {
          instr.unlink();
          delete &instr;
          changed = true;
        } else {
          guarded.insert(cond);
          guard_undo.push_back(cond);
        }
        continue;
      }
      auto key = keyFor(instr);
      if (!key.has_value()) {
        continue;
      }
      auto found = available.find(*key);
      if (found == available.end()) {
        available.emplace(*key, instr.output());
        expr_undo.push_back(*key);
        continue;
      }
      auto assign = Assign::create(instr.output(), found->second);
      assign->copyBytecodeOffset(instr);
      instr.ReplaceWith(*assign);
      removed.emplace_back(&instr);
      changed = true;
    }
  };

  std::vector<Frame> stack;
  stack.push_back(
      Frame{irfunc.cfg.entry_block, expr_undo.size(), guard_undo.size(), 0});
  process(irfunc.cfg.entry_block);
  while (!stack.empty()) {
    Frame& top = stack.back();
    auto kids = children.find(top.block);
    if (kids != children.end() && top.next_child < kids->second.size()) {
      BasicBlock* child = kids->second[top.next_child++];
      stack.push_back(Frame{child, expr_undo.size(), guard_undo.size(), 0});
      process(child);
      continue;
    }
    // Leave the scope of this block.
    while (expr_undo.size() > top.expr_mark) {
      available.erase(expr_undo.back());
      expr_undo.pop_back();
    }
    while (guard_undo.size() > top.guard_mark) {
      guarded.erase(guard_undo.back());
      guard_undo.pop_back();
    }
    stack.pop_back();
  }

  if (changed) {
    CopyPropagation{}.Run(irfunc);
    reflowTypes(irfunc);
  }
}

} // namespace jit::hir
