// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/loop_phi_unbox.h"

#include "cinderx/Common/spike_env.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/hir/copy_propagation.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/threaded_compile.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jit::hir {

#if PY_VERSION_HEX < 0x030C0000
namespace {

// How one Phi input is represented once its web is unboxed.
enum class InputKind {
  // PrimitiveBox<CInt64> (PrimitiveBox<CDouble> in a float web): the Phi
  // takes the box's primitive operand.
  kBox,
  // A canonical cached small-int constant.
  kConst,
  // LoadConst<Nullptr>: the local is unbound on this edge.
  kUnbound,
  // Another object Phi of the same web.
  kPhi,
};

struct PhiInput {
  InputKind kind;
  BasicBlock* pred;
  // kBox: the box's output; kPhi: the input Phi's output.
  Register* reg{nullptr};
  // kBox: the box's primitive operand.
  Register* unboxed{nullptr};
  // kConst: the cached small-int value.
  int64_t value{0};
  // kPhi: index of the input Phi in the candidate list.
  int phi{-1};
};

struct Candidate {
  Phi* phi;
  // A float web (CDouble values) rather than an int web (CInt64 values).
  bool is_double{false};
  std::vector<PhiInput> inputs;
  bool rejected{false};
  // Referenced from an operand-stack slot of some frame state.
  bool on_stack{false};
  int component{-1};
  bool maybe_unbound{false};
  int flag_class{-1};
  Register* unboxed{nullptr};
};

// Deopt reconstructs each primitive with a fresh boxing operation. Only the
// canonical CPython 3.11 small-int objects preserve their identity that way.
// Other co_consts objects, including every float, must remain boxed even on a
// zero-iteration path, where the loop has not replaced the original object.
std::optional<int64_t> cachedLongConst(Register* reg) {
  Type ty = reg->type();
  if (!ty.hasObjectSpec()) {
    return std::nullopt;
  }
  PyObject* obj = ty.objectSpec();
  if (!PyLong_CheckExact(obj)) {
    return std::nullopt;
  }
  Py_ssize_t size = Py_SIZE(obj);
  if (size < -1 || size > 1) {
    return std::nullopt;
  }
  auto digit = reinterpret_cast<PyLongObject*>(obj)->ob_digit[0];
  int64_t value = static_cast<int64_t>(size) * static_cast<int64_t>(digit);
  if (value < -5 || value > 256) {
    return std::nullopt;
  }
  // Compile workers share the main thread's GIL. Serialize both the new
  // reference from PyLong_FromLong and its release before leaving this scope.
  ThreadedCompileSerialize guard;
  auto canonical = Ref<>::steal(PyLong_FromLong(static_cast<long>(value)));
  if (canonical.get() != obj) {
    return std::nullopt;
  }
  return value;
}

// Operand uses that keep their meaning when the operand is the unboxed
// value instead of the box.
bool isRewritableUse(const Instr& use, bool is_double) {
  if (use.IsUseType()) {
    return true;
  }
  if (use.IsPrimitiveUnbox()) {
    return static_cast<const PrimitiveUnbox&>(use).type() <=
        (is_double ? TCDouble : TCInt64);
  }
  return !is_double && (use.IsCompactLongUnbox() || use.IsIsCompactLong());
}

int findRoot(std::vector<int>& parent, int x) {
  while (parent[x] != x) {
    parent[x] = parent[parent[x]];
    x = parent[x];
  }
  return x;
}

Instr* firstNonPhi(BasicBlock* block) {
  for (auto& instr : *block) {
    if (!instr.IsPhi()) {
      return &instr;
    }
  }
  return nullptr;
}

} // namespace
#endif

void LoopPhiUnbox::Run([[maybe_unused]] Function& irfunc) {
#if PY_VERSION_HEX < 0x030C0000
  // PrimitiveBoxRemat only drops CInt64 boxes under SPIKE_LONGSPEC; without
  // it the boxes this pass frees would stay and the flags would be pure cost.
  // SPIKE(PHIUNBOXF): the same for float webs (CDouble boxes are dropped by
  // the remat unconditionally).
  static const bool int_webs = Ci_SpikeGetenv("SPIKE_PHIUNBOX") != nullptr &&
      Ci_SpikeGetenv("SPIKE_LONGSPEC") != nullptr;
  static const bool float_webs = Ci_SpikeGetenv("SPIKE_PHIUNBOXF") != nullptr;
  if (!int_webs && !float_webs) {
    return;
  }

  // 1. Every object Phi that could hold an exact int (or an exact float) is
  // a provisional candidate, so that Phi inputs can be resolved to
  // candidates.
  std::vector<Candidate> cands;
  std::unordered_map<Register*, int> index;
  for (auto& block : irfunc.cfg.blocks) {
    for (auto& instr : block) {
      if (!instr.IsPhi()) {
        continue;
      }
      Register* out = instr.output();
      bool is_double;
      if (int_webs && out->type() <= TOptLongExact) {
        is_double = false;
      } else if (float_webs && out->type() <= TOptFloatExact) {
        is_double = true;
      } else {
        continue;
      }
      index.emplace(out, static_cast<int>(cands.size()));
      cands.push_back(Candidate{static_cast<Phi*>(&instr), is_double});
    }
  }
  if (cands.empty()) {
    return;
  }

  // 2. Classify the inputs.  A Phi with an input of any other kind is
  // rejected, but its Phi inputs are still recorded so that step 3 puts it in
  // the same web as them.
  for (auto& cand : cands) {
    Phi* phi = cand.phi;
    auto preds = phi->basic_blocks();
    for (std::size_t i = 0, n = phi->NumOperands(); i < n; ++i) {
      Register* in = chaseAssignOperand(phi->GetOperand(i));
      PhiInput input{InputKind::kUnbound, preds[i]};
      Instr* def = in->instr();
      Type prim = cand.is_double ? TCDouble : TCInt64;
      if (def->IsPhi()) {
        auto it = index.find(in);
        if (it == index.end() ||
            cands[it->second].is_double != cand.is_double) {
          cand.rejected = true;
          continue;
        }
        input.kind = InputKind::kPhi;
        input.reg = in;
        input.phi = it->second;
      } else if (def->IsPrimitiveBox()) {
        auto& box = static_cast<PrimitiveBox&>(*def);
        if (!(box.type() <= prim) || !(box.value()->type() <= prim)) {
          cand.rejected = true;
          continue;
        }
        input.kind = InputKind::kBox;
        input.reg = in;
        input.unboxed = box.value();
      } else if (in->type() <= TNullptr) {
        input.kind = InputKind::kUnbound;
      } else if (
          auto value =
              cand.is_double ? std::optional<int64_t>{} : cachedLongConst(in)) {
        input.kind = InputKind::kConst;
        input.value = *value;
      } else {
        cand.rejected = true;
        continue;
      }
      cand.inputs.push_back(input);
    }
  }

  // 3. Webs: Phis connected through Phi inputs are unboxed together or not at
  // all, since a boxed Phi cannot take an unboxed input and vice versa.
  std::vector<int> parent(cands.size());
  for (std::size_t i = 0; i < cands.size(); ++i) {
    parent[i] = static_cast<int>(i);
  }
  // Phis fed by the same box are joined to propagate rejection. Reifying
  // separate primitive Phis cannot preserve the shared Python object.
  std::unordered_map<Register*, int> box_owner;
  for (std::size_t i = 0; i < cands.size(); ++i) {
    for (auto& input : cands[i].inputs) {
      int other = -1;
      if (input.kind == InputKind::kPhi) {
        other = input.phi;
      } else if (input.kind == InputKind::kBox) {
        auto [it, inserted] = box_owner.emplace(input.reg, static_cast<int>(i));
        if (inserted) {
          continue;
        }
        other = it->second;
        if (other != static_cast<int>(i)) {
          cands[i].rejected = true;
          cands[other].rejected = true;
        }
      } else {
        continue;
      }
      int a = findRoot(parent, static_cast<int>(i));
      int b = findRoot(parent, other);
      if (a != b) {
        parent[a] = b;
      }
    }
  }
  std::unordered_map<int, bool> web_ok;
  for (std::size_t i = 0; i < cands.size(); ++i) {
    cands[i].component = findRoot(parent, static_cast<int>(i));
    auto [it, inserted] = web_ok.emplace(cands[i].component, true);
    if (cands[i].rejected) {
      it->second = false;
    }
  }

  // 4. Every reference to a candidate must be one this pass can rewrite: an
  // input of a Phi of the same web, a rewritable operand use, or a slot of a
  // frame state.  References are counted through visitUses() as well, so that
  // any other kind of reference rejects the web instead of being missed.
  auto reject = [&](Register* reg) {
    auto it = index.find(reg);
    if (it != index.end()) {
      web_ok[cands[it->second].component] = false;
    }
  };
  std::unordered_map<Register*, int> counts;
  std::unordered_map<Register*, int> frame_components;
  for (auto& cand : cands) {
    frame_components.emplace(cand.phi->output(), cand.component);
  }
  for (auto& [reg, owner] : box_owner) {
    frame_components.emplace(reg, cands[owner].component);
  }
  std::unordered_map<int, int> frame_uses;
  auto record_frame_use = [&](Register* reg) {
    if (reg == nullptr) {
      return;
    }
    reg = chaseAssignOperand(reg);
    auto found = frame_components.find(reg);
    if (found == frame_components.end()) {
      return;
    }
    int component = found->second;
    // A Phi and its source box can alias on an incoming edge. Repeated
    // occurrences of either also denote one object. Deopt boxes each frame
    // slot separately, so retain the boxed web whenever those aliases live.
    if (++frame_uses[component] > 1) {
      web_ok[component] = false;
    }
  };
  for (auto& block : irfunc.cfg.blocks) {
    for (auto& instr : block) {
      counts.clear();
      instr.visitUses([&](Register*& reg) {
        if (index.contains(reg)) {
          counts[reg]++;
        }
        return true;
      });
      for (std::size_t i = 0, n = instr.NumOperands(); i < n; ++i) {
        Register* reg = instr.GetOperand(i);
        auto it = counts.find(reg);
        if (it == counts.end()) {
          continue;
        }
        it->second--;
        if (instr.IsPhi()) {
          auto user = index.find(instr.output());
          if (user == index.end() ||
              cands[user->second].component != cands[index.at(reg)].component) {
            reject(reg);
          }
        } else if (!isRewritableUse(instr, cands[index.at(reg)].is_double)) {
          reject(reg);
        }
      }
      frame_uses.clear();
      for (FrameState* fs = get_frame_state(instr); fs != nullptr;
           fs = fs->parent) {
        for (Register* reg : fs->localsplus) {
          record_frame_use(reg);
          auto it = counts.find(reg);
          if (it != counts.end()) {
            it->second--;
          }
        }
        for (Register* reg : fs->stack) {
          record_frame_use(reg);
          auto it = counts.find(reg);
          if (it != counts.end()) {
            it->second--;
            cands[index.at(reg)].on_stack = true;
          }
        }
      }
      for (auto& [reg, left] : counts) {
        if (left != 0) {
          reject(reg);
        }
      }
    }
  }

  // 5. A Phi may hold an unbound local if an unbound edge reaches it through
  // the web.  A value on the operand stack is always bound: LOAD_FAST pushes
  // the result of its CheckVar, which Simplify removes only when the local's
  // type excludes Nullptr, and a CheckVar left in place rejects the web.  So
  // a stack slot takes the unboxed value without a flag; a Phi seen on the
  // stack that could still be unbound would contradict this, and rejects its
  // web instead of being trusted.
  for (bool changed = true; changed;) {
    changed = false;
    for (auto& cand : cands) {
      if (cand.maybe_unbound || !web_ok[cand.component]) {
        continue;
      }
      for (auto& input : cand.inputs) {
        if (input.kind == InputKind::kUnbound ||
            (input.kind == InputKind::kPhi && cands[input.phi].maybe_unbound)) {
          cand.maybe_unbound = true;
          changed = true;
          break;
        }
      }
    }
  }
  for (auto& cand : cands) {
    if (cand.on_stack &&
        (cand.maybe_unbound || cand.phi->output()->type().couldBe(TNullptr))) {
      web_ok[cand.component] = false;
    }
  }

  // 6. Source boxes must not escape through object consumers: even if
  // another input box can be removed, reifying the primitive Phi would lose
  // the identity of an escaped input on its incoming edge. Pure primitive
  // reads are safe, but only frame-state/UseType-only boxes count as a gain.
  auto direct_uses = collectDirectRegUses(irfunc);
  std::unordered_map<int, bool> web_gain;
  for (auto& cand : cands) {
    if (!web_ok[cand.component]) {
      continue;
    }
    for (auto& input : cand.inputs) {
      if (input.kind != InputKind::kBox) {
        continue;
      }
      bool removable = true;
      auto uses = direct_uses.find(input.reg);
      if (uses != direct_uses.end()) {
        for (Instr* use : uses->second) {
          if (use->IsPhi()) {
            auto it = index.find(use->output());
            if (it != index.end() &&
                cands[it->second].component == cand.component) {
              continue;
            }
          }
          if (use->IsUseType()) {
            continue;
          }
          removable = false;
          if (!isRewritableUse(*use, cand.is_double)) {
            web_ok[cand.component] = false;
            break;
          }
        }
      }
      if (removable) {
        web_gain[cand.component] = true;
      }
    }
  }
  std::vector<int> chosen;
  for (std::size_t i = 0; i < cands.size(); ++i) {
    if (web_ok[cands[i].component] && web_gain[cands[i].component]) {
      chosen.push_back(static_cast<int>(i));
    }
  }
  if (chosen.empty()) {
    return;
  }

  // 7. Flags are shared: Phis of one block whose flag inputs agree edge by
  // edge (locals that become bound together, such as a loop variable and the
  // indexes computed from it) get one flag Phi.  Partition refinement, since
  // flag Phis refer to each other around loops.  Signature entries: -1 is the
  // constant 0 (unbound), -2 the constant 1 (bound), otherwise a class id.
  std::vector<int> flagged;
  for (int i : chosen) {
    if (cands[i].maybe_unbound) {
      flagged.push_back(i);
    }
  }
  auto signature = [&](const Candidate& cand) {
    std::vector<int> sig;
    for (auto& input : cand.inputs) {
      switch (input.kind) {
        case InputKind::kUnbound:
          sig.push_back(-1);
          break;
        case InputKind::kBox:
        case InputKind::kConst:
          sig.push_back(-2);
          break;
        case InputKind::kPhi: {
          const Candidate& in = cands[input.phi];
          sig.push_back(in.maybe_unbound ? in.flag_class : -2);
          break;
        }
      }
    }
    return sig;
  };
  {
    std::map<BasicBlock*, int> by_block;
    for (int i : flagged) {
      auto [it, inserted] = by_block.emplace(
          cands[i].phi->block(), static_cast<int>(by_block.size()));
      cands[i].flag_class = it->second;
    }
    std::size_t nclasses = by_block.size();
    for (;;) {
      std::map<std::pair<int, std::vector<int>>, int> refined;
      std::vector<int> next(flagged.size());
      for (std::size_t k = 0; k < flagged.size(); ++k) {
        const Candidate& cand = cands[flagged[k]];
        auto key = std::make_pair(cand.flag_class, signature(cand));
        auto [it, inserted] =
            refined.emplace(std::move(key), static_cast<int>(refined.size()));
        next[k] = it->second;
      }
      for (std::size_t k = 0; k < flagged.size(); ++k) {
        cands[flagged[k]].flag_class = next[k];
      }
      if (refined.size() == nclasses) {
        break;
      }
      nclasses = refined.size();
    }
  }

  // 8. Materialize.  Constants go at the end of the predecessor, one per
  // (block, kind, value).
  std::map<std::tuple<BasicBlock*, bool, int64_t>, Register*> consts;
  auto constOf = [&](BasicBlock* pred, bool is_double, int64_t value) {
    auto key = std::make_tuple(pred, is_double, value);
    auto it = consts.find(key);
    if (it != consts.end()) {
      return it->second;
    }
    Register* reg = irfunc.env.AllocateRegister();
    Instr* term = pred->GetTerminator();
    Type type = is_double ? Type::fromCDouble(bit_cast<double>(value))
                          : Type::fromCInt(value, TCInt64);
    auto* load = LoadConst::create(reg, type);
    load->copyBytecodeOffset(*term);
    load->InsertBefore(*term);
    consts.emplace(key, reg);
    return reg;
  };
  // Flags are 0/1 int64s whatever the web's kind.
  auto constIn = [&](BasicBlock* pred, int64_t value) {
    return constOf(pred, false, value);
  };

  for (int i : chosen) {
    cands[i].unboxed = irfunc.env.AllocateRegister();
  }
  std::vector<Register*> flag_regs;
  std::vector<int> class_rep;
  for (int i : flagged) {
    int c = cands[i].flag_class;
    if (static_cast<std::size_t>(c) >= flag_regs.size()) {
      flag_regs.resize(c + 1, nullptr);
      class_rep.resize(c + 1, -1);
    }
    if (flag_regs[c] == nullptr) {
      flag_regs[c] = irfunc.env.AllocateRegister();
      class_rep[c] = i;
    }
  }

  auto insertPhi = [](Phi* phi, const Phi& like) {
    phi->copyBytecodeOffset(like);
    Instr* at = firstNonPhi(like.block());
    if (at == nullptr) {
      at = like.block()->GetTerminator();
    }
    phi->InsertBefore(*at);
  };

  for (int i : chosen) {
    auto& cand = cands[i];
    std::unordered_map<BasicBlock*, Register*> args;
    for (auto& input : cand.inputs) {
      switch (input.kind) {
        case InputKind::kBox:
          args[input.pred] = input.unboxed;
          break;
        case InputKind::kConst:
          args[input.pred] = constOf(input.pred, cand.is_double, input.value);
          break;
        case InputKind::kUnbound:
          // The value is never read while the flag says unbound.
          args[input.pred] = constOf(input.pred, cand.is_double, 0);
          break;
        case InputKind::kPhi:
          args[input.pred] = cands[input.phi].unboxed;
          break;
      }
    }
    insertPhi(Phi::create(cand.unboxed, args), *cand.phi);
  }

  for (std::size_t c = 0; c < flag_regs.size(); ++c) {
    if (flag_regs[c] == nullptr) {
      continue;
    }
    const Candidate& rep = cands[class_rep[c]];
    std::unordered_map<BasicBlock*, Register*> args;
    for (auto& input : rep.inputs) {
      Register* arg = nullptr;
      switch (input.kind) {
        case InputKind::kUnbound:
          arg = constIn(input.pred, 0);
          break;
        case InputKind::kBox:
        case InputKind::kConst:
          arg = constIn(input.pred, 1);
          break;
        case InputKind::kPhi: {
          const Candidate& in = cands[input.phi];
          arg = in.maybe_unbound ? flag_regs[in.flag_class]
                                 : constIn(input.pred, 1);
          break;
        }
      }
      args[input.pred] = arg;
    }
    insertPhi(Phi::create(flag_regs[c], args), *rep.phi);
  }

  // 9. Rewrite the references: frame-state slots take the unboxed value (and
  // locals the flag), operand uses read the unboxed value directly.
  std::unordered_map<Register*, int> chosen_index;
  for (int i : chosen) {
    chosen_index.emplace(cands[i].phi->output(), i);
  }
  std::vector<std::unique_ptr<Instr>> removed;
  for (auto& block : irfunc.cfg.blocks) {
    for (auto it = block.begin(); it != block.end();) {
      Instr& instr = *it;
      ++it;
      for (FrameState* fs = get_frame_state(instr); fs != nullptr;
           fs = fs->parent) {
        for (std::size_t j = 0; j < fs->localsplus.size(); ++j) {
          auto found = chosen_index.find(fs->localsplus[j]);
          if (found == chosen_index.end()) {
            continue;
          }
          const Candidate& cand = cands[found->second];
          fs->localsplus[j] = cand.unboxed;
          if (cand.maybe_unbound) {
            if (fs->local_bound.empty()) {
              fs->local_bound.assign(fs->localsplus.size(), nullptr);
            }
            fs->local_bound[j] = flag_regs[cand.flag_class];
          }
        }
        for (auto& reg : fs->stack) {
          auto found = chosen_index.find(reg);
          if (found != chosen_index.end()) {
            JIT_DCHECK(
                !cands[found->second].maybe_unbound,
                "an unbound local cannot be on the operand stack");
            reg = cands[found->second].unboxed;
          }
        }
      }
      if (instr.IsPhi() || instr.NumOperands() != 1) {
        continue;
      }
      auto found = chosen_index.find(instr.GetOperand(0));
      if (found == chosen_index.end()) {
        continue;
      }
      Register* unboxed = cands[found->second].unboxed;
      if (instr.IsIsCompactLong()) {
        instr.SetOperand(0, unboxed);
      } else if (instr.IsUseType()) {
        instr.unlink();
        removed.emplace_back(&instr);
      } else {
        // CompactLongUnbox / PrimitiveUnbox<CInt64> / PrimitiveUnbox<CDouble>
        auto* assign = Assign::create(instr.output(), unboxed);
        assign->copyBytecodeOffset(instr);
        instr.ReplaceWith(*assign);
        removed.emplace_back(&instr);
      }
    }
  }
  for (int i : chosen) {
    cands[i].phi->unlink();
    removed.emplace_back(cands[i].phi);
  }

  CopyPropagation{}.Run(irfunc);
  reflowTypes(irfunc);
#endif
}

} // namespace jit::hir
