// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/simplify.h"

#include "pycore_long.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/dict.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/property.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/spike_env.h"
#include "cinderx/Common/type.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/array_specialize.h"
#include "cinderx/Jit/hir/clean_cfg.h"
#include "cinderx/Jit/hir/copy_propagation.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/hir/type.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/threaded_compile.h"
#include "cinderx/StaticPython/strictmoduleobject.h"

#include <fmt/ostream.h>

#include <cfloat>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace jit::hir {

// This file contains the Simplify pass, which is a collection of
// strength-reduction optimizations. An optimization should be added as a case
// in Simplify rather than a standalone pass if and only if it meets these
// criteria:
// - It operates on one instruction at a time, with no global analysis or
//   state.
// - Optimizable instructions are replaced with 0 or more new instructions that
//   define an equivalent value while doing less work.
//
// To add support for a new instruction Foo, add a function simplifyFoo(Env&
// env, const Foo* instr) (env can be left out if you don't need it) containing
// the optimization and call it from a new case in
// simplifyInstr(). simplifyFoo() should analyze the given instruction, then do
// one of the following:
// - If the instruction is not optimizable, return nullptr and do not call any
//   functions on env.
// - If the instruction is redundant and can be elided, return the existing
//   value that should replace its output (this is often one of the
//   instruction's inputs).
// - If the instruction can be replaced with a cheaper sequence of
//   instructions, emit those instructions using env.emit<T>(...). For
//   instructions that define an output, emit<T> will allocate and return an
//   appropriately-typed Register* for you, to ease chaining multiple
//   instructions. As with the previous case, return the Register* that should
//   replace the current output of the instruction.
// - If the instruction can be elided but does not produce an output, set
//   env.optimized = true and return nullptr.
//
// Do not modify, unlink, or delete the existing instruction; all of those
// details are handled by existing code outside of the individual optimization
// functions.

namespace {

struct Env {
  explicit Env(Function& f)
      : func{f},
        type_object(
            Type::fromObject(reinterpret_cast<PyObject*>(&PyType_Type))) {}

  // The current function.
  Function& func;

  // The current block being emitted into. Might not be the block originally
  // containing the instruction being optimized, if more blocks have been
  // inserted by the simplify function.
  BasicBlock* block{nullptr};

  // Insertion cursor for new instructions. Must belong to block's Instr::List,
  // and except for brief critical sections during emit functions on Env,
  // should always point to the original, unoptimized instruction.
  Instr::List::iterator cursor;

  // Bytecode instruction of the instruction being optimized, automatically set
  // on all replacement instructions.
  BCOffset bc_off{-1};

  // Set to true by emit<T>() to indicate that the original instruction should
  // be removed.
  bool optimized{false};

  // The object that corresponds to "type".
  Type type_object{TTop};

  // Number of new basic blocks added by the simplifier.
  size_t new_blocks{0};

  // True while simplifying an instruction whose exception exit is a CFG
  // edge; the replacement's generic (raising) arm then carries the mark
  // instead of a deopt, so its NULL result reaches the edge.
  bool exc_edge{false};
  // SPIKE(INT64ARITH): |v| < 2^bits known for CInt64 registers produced by
  // the int64 lowerings (64 = unknown); see int64Bits311.
  std::unordered_map<const Register*, int> int64_bits;

  // Create and insert the specified instruction. If the instruction has an
  // output, a new Register* will be created and returned.
  template <typename T, typename... Args>
  Register* emit(Args&&... args) {
    return emitInstr<T>(std::forward<Args>(args)...)->output();
  }

  // Similar to emit(), but returns the instruction itself. Useful for
  // instructions with no output, when you need to manipulate the instruction
  // after creation.
  template <typename T, typename... Args>
  T* emitInstr(Args&&... args) {
    if constexpr (T::has_output) {
      return emitRawInstr<T>(
          func.env.AllocateRegister(), std::forward<Args>(args)...);
    } else {
      return emitRawInstr<T>(std::forward<Args>(args)...);
    }
  }

  // Similar to emitRawInstr<T>(), but does not automatically create an output
  // Create and insert the specified instruction. If the instruction has an
  // output, a new Register* will be created and returned.
  template <typename T, typename... Args>
  Register* emitVariadic(std::size_t arity, Args&&... args) {
    if constexpr (T::has_output) {
      return emitRawInstr<T>(
                 arity,
                 func.env.AllocateRegister(),
                 std::forward<Args>(args)...)
          ->output();
    } else {
      return emitRawInstr<T>(arity, std::forward<Args>(args)...)->output();
    }
  }

  // Similar to emit<T>(), but does not automatically create an output
  // register.
  template <typename T, typename... Args>
  T* emitRawInstr(Args&&... args) {
    optimized = true;
    T* instr = T::create(std::forward<Args>(args)...);
    instr->setBytecodeOffset(bc_off);
    block->insert(instr, cursor);

    if constexpr (T::has_output) {
      Register* output = instr->output();
      switch (instr->opcode()) {
        case Opcode::kVectorCall:
          // We don't know the exact output type until its operands are
          // populated.
          output->set_type(TObject);
          break;
        default:
          output->set_type(outputType(*instr));
          break;
      }
    }

    return instr;
  }

  // Create and return a conditional value. Expects three callables:
  // - do_branch is given two BasicBlock* and should emit a conditional branch
  //   instruction using them.
  // - do_bb1 should emit code for the first successor, returning the computed
  //   value.
  // - do_bb2 should do the same for the second successor.
  template <typename BranchFn, typename Bb1Fn, typename Bb2Fn>
  Register* emitCond(BranchFn do_branch, Bb1Fn do_bb1, Bb2Fn do_bb2) {
    // bb1, bb2, and the new tail block that's split from the original.
    new_blocks += 3;

    BasicBlock* bb1 = func.cfg.AllocateBlock();
    BasicBlock* bb2 = func.cfg.AllocateBlock();
    do_branch(bb1, bb2);
    JIT_CHECK(
        cursor != block->begin(),
        "block should not be empty after calling do_branch()");
    BasicBlock* tail = func.cfg.splitAfter(*std::prev(cursor));

    block = bb1;
    cursor = bb1->end();
    Register* bb1_reg = do_bb1();
    // do_bb1() might have created more blocks, use the final one.
    BasicBlock* bb1_end = block;
    emit<Branch>(tail);

    block = bb2;
    cursor = bb2->end();
    Register* bb2_reg = do_bb2();
    BasicBlock* bb2_end = block;
    emit<Branch>(tail);

    block = tail;
    cursor = tail->begin();
    std::unordered_map<BasicBlock*, Register*> phi_srcs{
        {bb1_end, bb1_reg},
        {bb2_end, bb2_reg},
    };
    return emit<Phi>(phi_srcs);
  }

  // Create and return a conditional value that could go through a slow path if
  // it matches a certain condition. Expects two callables:
  //
  // - do_branch is given a BasicBlock* and it is expected that it will
  //   conditionally branch to that block if it needs to. The true_bb will be
  //   patched after the fast path is split. It should return the branch
  //   instruction so that it can be patched.
  // - do_slow_path should emit code for the slow path, returning the computed
  //   value.
  //
  // It is expected that the slow path will jump back to the default path at the
  // end of its block.
  template <typename BranchFn, typename SlowPathFn>
  Phi* emitCondSlowPath(
      Register* output,
      Register* previous_path_value,
      BranchFn do_branch,
      SlowPathFn do_slow_path) {
    new_blocks += 2;

    BasicBlock* previous_path = block;
    BasicBlock* slow_path = func.cfg.AllocateBlock();

    auto branch = do_branch(slow_path);
    BasicBlock* fast_path = func.cfg.splitAfter(*branch);
    branch->set_true_bb(fast_path);

    block = slow_path;
    cursor = slow_path->begin();
    auto slow_path_value = do_slow_path();
    // do_slow_path() might have created more blocks, use the final one.
    BasicBlock* slow_path_end = block;
    emit<Branch>(fast_path);

    block = fast_path;
    cursor = fast_path->begin();
    std::unordered_map<BasicBlock*, Register*> args{
        {previous_path, previous_path_value},
        {slow_path_end, slow_path_value},
    };

    return emitRawInstr<Phi>(output, args);
  }

  // Last-use information for the current function, computed lazily on first use
  // and cached for the remainder of the current Simplify iteration.
  // Recomputing liveness once per iteration keeps liveness-dependent rewrites
  // (e.g. the isinstance-if simplification) from being quadratic in function
  // size.  invalidateLastUses() must be called whenever the cache may have gone
  // stale, i.e. between iterations after the CopyPropagation/CleanCFG cleanup
  // runs.
  const LivenessAnalysis::LastUses& lastUses() {
    if (!last_uses_.has_value()) {
      LivenessAnalysis liveness{func};
      liveness.Run();
      last_uses_ = liveness.GetLastUses();
    }
    return *last_uses_;
  }

  void invalidateLastUses() {
    last_uses_.reset();
  }

 private:
  std::optional<LivenessAnalysis::LastUses> last_uses_;
};

Register* simplifyCheck(const CheckBase* instr) {
  // These all check their input for null.
  if (instr->GetOperand(0)->isA(TObject)) {
    // No UseType is necessary because we never guard potentially-null values.
    return instr->GetOperand(0);
  }
  return nullptr;
}

Register* tryEmitInlineBoundsCheck(
    Env& env,
    const FrameState* dominating_fs,
    Register* container,
    Register* unboxed_index);
bool spikeArrIdx();

Register* simplifyCheckSequenceBounds(
    Env& env,
    const CheckSequenceBounds* instr) {
  Register* sequence = instr->GetOperand(0);
  Register* idx = instr->GetOperand(1);
  if (sequence->isA(TTupleExact) && sequence->instr()->IsMakeTuple() &&
      idx->isA(TCInt) && idx->type().hasIntSpec()) {
    size_t length = static_cast<const MakeTuple*>(sequence->instr())->nvalues();
    intptr_t idx_value = idx->type().intSpec();
    bool adjusted = false;
    if (idx_value < 0) {
      idx_value += length;
      adjusted = true;
    }
    if (static_cast<size_t>(idx_value) < length) {
      env.emit<UseType>(sequence, sequence->type());
      env.emit<UseType>(idx, idx->type());
      if (adjusted) {
        return env.emit<LoadConst>(Type::fromCInt(idx_value, TCInt64));
      } else {
        return idx;
      }
    }
  }
  // SPIKE(ARRIDX): the builder's array.array store path (and any other
  // helper-form bounds check on a var-sized exact container) lowers to a
  // JITRT_CheckSequenceBounds call; replace it with the inline form.
  PyTypeObject* stdlib_array = getStdlibArrayType();
  if (spikeArrIdx() && idx->isA(TCInt64) &&
      ((stdlib_array != nullptr &&
        sequence->isA(Type::fromTypeExact(stdlib_array))) ||
       sequence->isA(TListExact) || sequence->isA(TTupleExact))) {
    Register* normalized = tryEmitInlineBoundsCheck(
        env, instr->getDominatingFrameState(), sequence, idx);
    if (normalized != nullptr) {
      env.emit<UseType>(sequence, sequence->type().unspecialized());
      return normalized;
    }
  }
  return nullptr;
}

Register* simplifyGuardType(Env& env, const GuardType* instr) {
  Register* input = instr->GetOperand(0);
  Type type = instr->target();
  if (input->isA(type)) {
    // We don't need a UseType: If an instruction cares about the type of this
    // GuardType's output, it will express that through its operand type
    // constraints. Once this GuardType is removed, those constraints will
    // apply to input's instruction rather than this GuardType, and any
    // downstream instructions will still be satisfied.
    return input;
  }
  if (type == TNoneType) {
    return env.emit<GuardIs>(Py_None, input);
  }
  return nullptr;
}

Register* simplifyRefineType(const RefineType* instr) {
  Register* input = instr->GetOperand(0);
  if (input->isA(instr->type())) {
    // No UseType for the same reason as GuardType above: RefineType itself
    // doesn't care about the input's type, only users of its output do, and
    // they're unchanged.
    return input;
  }
  return nullptr;
}

Register* simplifyCast(const Cast* instr) {
  Register* input = instr->GetOperand(0);
  Type type = instr->exact() ? Type::fromTypeExact(instr->pytype())
                             : Type::fromType(instr->pytype());
  if (instr->optional()) {
    type |= TNoneType;
  }
  if (input->isA(type)) {
    // No UseType for the same reason as GuardType above: Cast itself
    // doesn't care about the input's type, only users of its output do, and
    // they're unchanged.
    return input;
  }
  return nullptr;
}

Register* emitGetLengthInt64(Env& env, Register* obj) {
  Type ty = obj->type();

  // Constant folding.
  if (ty.hasObjectSpec()) {
    PyObject* spec = ty.objectSpec();
    if (ty <= TTupleExact) {
      env.emit<UseType>(obj, ty);
      Py_ssize_t len = PyTuple_GET_SIZE(spec);
      return env.emit<LoadConst>(Type::fromCInt(len, TCInt64));
    }
    if (ty <= TUnicodeExact) {
      env.emit<UseType>(obj, ty);
      Py_ssize_t len = PyUnicode_GET_LENGTH(spec);
      return env.emit<LoadConst>(Type::fromCInt(len, TCInt64));
    }
  }

  if (ty <= TTupleExact ||
      // TODO(T255264007). Enable this again. See P2169677410.
      (!kFreeThreadedBuild && (ty <= TListExact || ty <= TArray))) {
    env.emit<UseType>(obj, ty.unspecialized());
    return env.emit<LoadField>(
        obj, "ob_size", offsetof(PyVarObject, ob_size), TCInt64);
  }
  if (ty <= TUnicodeExact ||
      // TODO(T255264007). Enable this again. See P2169677410.
      (!kFreeThreadedBuild && (ty <= TDictExact || ty <= TSetExact))) {
    std::size_t offset = 0;
    const char* name = nullptr;
    if (ty <= TDictExact) {
      offset = offsetof(PyDictObject, ma_used);
      name = "ma_used";
    } else if (ty <= TSetExact) {
      offset = offsetof(PySetObject, used);
      name = "used";
    } else if (ty <= TUnicodeExact) {
      // Note: In debug mode, the interpreter has an assert that ensures the
      // string is "ready", check PyUnicode_GET_LENGTH for strings.
      offset = offsetof(PyASCIIObject, length);
      name = "length";
    } else {
      JIT_ABORT("unexpected type");
    }
    env.emit<UseType>(obj, ty.unspecialized());
    return env.emit<LoadField>(obj, name, offset, TCInt64);
  }
  return nullptr;
}

Register* simplifyGetLength(Env& env, const GetLength* instr) {
  Register* obj = instr->GetOperand(0);
  if (Register* size = emitGetLengthInt64(env, obj)) {
    return env.emit<PrimitiveBox>(size, TCInt64, *instr->frameState());
  }
  return nullptr;
}

Register* simplifyPrimitiveConvert(Env& env, const PrimitiveConvert* instr) {
  Register* src = instr->GetOperand(0);
  // Source and dest types already match.
  if (src->isA(instr->type())) {
    // Intentionally widen the source to the destination type, theoretically
    // this could relax a guard.
    env.emit<UseType>(src, instr->type());
    return instr->GetOperand(0);
  }
  // Constant CInt --> CDouble.
  if (instr->type() <= TCDouble && src->type().hasIntSpec()) {
    env.emit<UseType>(src, src->type());
    return env.emit<LoadConst>(
        Type::fromCDouble(static_cast<double>(src->type().intSpec())));
  }
  // Constant CInt --> CInt.
  if (src->type().hasIntSpec()) {
    int64_t val = src->type().intSpec();
    Type dst_type = instr->type();
    if (dst_type <= TCSigned) {
      env.emit<UseType>(src, src->type());
      return env.emit<LoadConst>(Type::truncatedCInt(val, dst_type));
    } else if (dst_type <= TCUnsigned) {
      env.emit<UseType>(src, src->type());
      return env.emit<LoadConst>(
          Type::truncatedCUInt(static_cast<uint64_t>(val), dst_type));
    }
    return nullptr;
  }
  return nullptr;
}

#if PY_VERSION_HEX < 0x030C0000
// SPIKE(FLOATSPEC feedback): only the evidence-free `all` mode carries the
// hit/miss counters and the takedown.  In the evidence mode a stuck site
// that turns out int-only would otherwise be recompiled during the run
// (raytrace: 8 takedowns, +3.7% in a short benchmark) for no gain.
bool spikeFloatSpecFeedback311() {
  static const bool on = [] {
    const char* v = Ci_SpikeGetenv("SPIKE_FLOATSPEC");
    return v != nullptr && strcmp(v, "all") == 0;
  }();
  return on;
}

// Bump the per-site hit counter in a speculative fast arm (load / add /
// store through the counter cell's fixed address).
void emitFloatSpecHit311(Env& env, const DeoptBase* instr) {
  uint64_t* cell = Ci_SpikeFloatSpecSiteCounters311(
      env.func.code.get(),
      static_cast<int64_t>(instr->bytecodeOffset().value()));
  Register* code_reg = env.emit<LoadConst>(
      Type::fromObject(reinterpret_cast<PyObject*>(env.func.code.get())));
  Register* cell_reg = env.emit<LoadConst>(Type::fromCPtr(cell));
  Register* zero = env.emit<LoadConst>(Type::fromCInt(0, TCInt64));
  Register* hits = env.emit<LoadArrayItem>(
      cell_reg, zero, code_reg, static_cast<intptr_t>(0), TCInt64);
  Register* one = env.emit<LoadConst>(Type::fromCInt(1, TCInt64));
  Register* bumped = env.emit<IntBinaryOp>(BinaryOpKind::kAdd, hits, one);
  env.emit<StoreArrayItem>(cell_reg, zero, bumped, code_reg, TCInt64);
}

// Report a miss of an evidence-free float diamond to its per-site counter,
// which eventually recompiles the site without the speculation.  Emitted
// before the arm's Snapshot: bindGuards treats a non-replayable instruction
// as ending the Snapshot's reach.
void emitFloatSpecMiss311(Env& env, const DeoptBase* instr) {
  Register* code_reg = env.emit<LoadConst>(
      Type::fromObject(reinterpret_cast<PyObject*>(env.func.code.get())));
  Register* off_reg = env.emit<LoadConst>(Type::fromCInt(
      static_cast<int64_t>(instr->bytecodeOffset().value()), TCInt64));
  auto* miss = env.emitRawInstr<CallStaticRetVoid>(
      2, reinterpret_cast<void*>(Ci_SpikeFloatSpecMiss311));
  miss->SetOperand(0, code_reg);
  miss->SetOperand(1, off_reg);
}

// The instruction defining `reg`.  The simplifier driver rewrites a replaced
// instruction's output as an Assign of the new value, so look through Assign
// chains to the real definition.
const Instr* definingInstr311(Register* reg) {
  const Instr* def = reg->instr();
  while (def != nullptr && def->IsAssign()) {
    def = def->GetOperand(0)->instr();
  }
  return def;
}

// True while `reg` is still an intermediate arithmetic form (FloatBinaryOp /
// LongBinaryOp, or a BinaryOp / InPlaceOp whose operands are both `exact`).
// The next simplifier iteration lowers it to an unboxed op plus PrimitiveBox,
// which is what a diamond's slow-arm re-boxing needs to see, so a diamond
// over such an operand waits for that iteration.
bool stillLowering311(Register* reg, Type exact) {
  const Instr* def = definingInstr311(reg);
  if (def == nullptr) {
    return false;
  }
  if (def->IsFloatBinaryOp() || def->IsLongBinaryOp()) {
    return true;
  }
  if (def->IsBinaryOp() || def->IsInPlaceOp()) {
    return def->GetOperand(0)->isA(exact) && def->GetOperand(1)->isA(exact);
  }
  return false;
}

// True when a frame of `fs` (the frame itself or a caller it was inlined
// into) names `box` in a local or a stack slot.
bool frameStateNamesBox311(const FrameState* fs, const Instr* box) {
  for (const FrameState* frame = fs; frame != nullptr; frame = frame->parent) {
    for (Register* reg : frame->localsplus) {
      if (reg != nullptr && definingInstr311(reg) == box) {
        return true;
      }
    }
    for (Register* reg : frame->stack) {
      if (reg != nullptr && definingInstr311(reg) == box) {
        return true;
      }
    }
  }
  return false;
}

// True when an instruction that can run after `user` keeps `box` in its
// FrameState.  Once the slow arm has handed a re-boxed object to the generic
// operation (a reflected method may keep it), a later traceback or deopt
// rebuilt from such a FrameState would show the original box, or a fresh
// one rematerialised from the primitive, instead of that object.  Only the
// user's own FrameState is retargeted (slowArmFrameState311).
bool boxNamedAfter311(const Instr* box, const Instr* user) {
  BasicBlock* start = user->block();
  if (start == nullptr) {
    return true;
  }
  std::unordered_set<const BasicBlock*> seen;
  std::vector<BasicBlock*> work;
  auto push_succs = [&](const BasicBlock* block) {
    const Instr* term = block->GetTerminator();
    for (std::size_t i = 0; term != nullptr && i < term->numEdges(); ++i) {
      BasicBlock* succ = term->successor(i);
      if (succ != nullptr && seen.insert(succ).second) {
        work.push_back(succ);
      }
    }
  };
  auto names_box = [&](const Instr& instr) {
    const FrameState* fs = nullptr;
    if (const DeoptBase* deopt = instr.asDeoptBase()) {
      fs = deopt->frameState();
    } else if (instr.IsSnapshot()) {
      fs = static_cast<const Snapshot&>(instr).frameState();
    }
    return frameStateNamesBox311(fs, box);
  };
  bool after_user = false;
  for (const Instr& instr : *start) {
    if (after_user && names_box(instr)) {
      return true;
    }
    after_user = after_user || &instr == user;
  }
  push_succs(start);
  while (!work.empty()) {
    BasicBlock* block = work.back();
    work.pop_back();
    for (const Instr& instr : *block) {
      if (&instr != user && names_box(instr)) {
        return true;
      }
    }
    push_succs(block);
  }
  return false;
}

// True when an instruction other than `user` reads `box`, directly or through
// an Assign copy, or the box stays observable after `user` through a
// FrameState (boxNamedAfter311).
bool boxHasOtherReader311(Env& env, const Instr* box, const Instr* user) {
  if (boxNamedAfter311(box, user)) {
    return true;
  }
  // A caller frame the user was inlined into is reified from its own
  // FrameState, which the slow arm cannot retarget (slowArmFrameState311
  // rewrites only the innermost frame): a local there holding the box keeps
  // the original object.
  if (const DeoptBase* deopt = user->asDeoptBase()) {
    const FrameState* fs = deopt->frameState();
    for (const FrameState* frame = fs != nullptr ? fs->parent : nullptr;
         frame != nullptr;
         frame = frame->parent) {
      for (Register* reg : frame->localsplus) {
        if (reg != nullptr && definingInstr311(reg) == box) {
          return true;
        }
      }
    }
  }
  for (auto& block : env.func.cfg.blocks) {
    for (const Instr& other : block) {
      if (&other == user || other.IsAssign()) {
        continue;
      }
      for (size_t i = 0; i < other.NumOperands(); ++i) {
        if (definingInstr311(other.GetOperand(i)) == box) {
          return true;
        }
      }
    }
  }
  return false;
}

// A diamond operand that is a PrimitiveBox read only by `user` is re-boxed
// inside the slow arm, so the main-path box can die once the fast arm's
// unbox(box(x)) folds; otherwise every execution would materialise an object
// only the rare generic path needs.  If anything else reads the box, the slow
// arm must pass that same object: a user-defined __radd__ etc. can return or
// keep it, and a fresh equal float would break `is`.  Call this before
// emitting the diamond, whose fast arm also reads the box.
const PrimitiveBox*
reboxableForSlowArm311(Env& env, Register* reg, const Instr* user) {
  const Instr* def = definingInstr311(reg);
  if (def == nullptr || !def->IsPrimitiveBox() ||
      boxHasOtherReader311(env, def, user)) {
    return nullptr;
  }
  return static_cast<const PrimitiveBox*>(def);
}

Register* slowArmOperand311(
    Env& env,
    Register* reg,
    const PrimitiveBox* rebox,
    const FrameState& fs) {
  if (rebox == nullptr) {
    return reg;
  }
  return env.emit<PrimitiveBox>(rebox->value(), rebox->type(), fs);
}

// The generic operation's FrameState in a slow arm that re-boxed operands:
// a local or stack slot of the frame that holds a re-boxed box names the new
// object instead, so a traceback or sys._getframe() taken during the generic
// operation (a reflected method that raises or inspects its caller) sees the
// very object the operation received.  Caller frames are not rewritten; a
// box one of them holds is never re-boxed (boxHasOtherReader311).
FrameState slowArmFrameState311(
    const FrameState& fs,
    const PrimitiveBox* l_rebox,
    Register* l_slow,
    const PrimitiveBox* r_rebox,
    Register* r_slow) {
  FrameState slow_fs = fs;
  auto retarget = [&](Register*& reg) {
    if (reg == nullptr) {
      return;
    }
    const Instr* def = definingInstr311(reg);
    if (l_rebox != nullptr && def == l_rebox) {
      reg = l_slow;
    } else if (r_rebox != nullptr && def == r_rebox) {
      reg = r_slow;
    }
  };
  for (Register*& reg : slow_fs.localsplus) {
    retarget(reg);
  }
  for (Register*& reg : slow_fs.stack) {
    retarget(reg);
  }
  return slow_fs;
}

namespace {
// SPIKE(INT64ARITH): defined with the int64 lowerings below simplifySubscript.
bool int64WideOperand311(Env& env, Register* reg);
bool int64OperandPair311(
    Env& env,
    Register* lhs,
    Register* rhs,
    const FrameState* fs,
    bool& guarded,
    Register*& a,
    Register*& b);
} // namespace

// SPIKE(CMPTYPED, FLOATSPEC): pin every operand not already known to be
// `exact` with a CondBranchCheckType; the innermost fast arm runs `fast(l, r)`
// on the refined registers, every miss runs `slow(l, r)` on the registers as
// they are at that point.  Returns nullptr when a pin is impossible (an
// operand provably not `exact`) or unnecessary (both already exact).  Serves
// the typed compares and the float arithmetic diamonds.
template <typename FastFn, typename SlowFn>
Register* emitSpecializedCompareDiamond(
    Env& env,
    Type exact,
    Register* left,
    Register* right,
    const FrameState& /* dominating_fs */,
    FastFn fast,
    SlowFn slow) {
  bool l_known = left->isA(exact);
  bool r_known = right->isA(exact);
  if ((l_known && r_known) || !left->type().couldBe(exact) ||
      !right->type().couldBe(exact)) {
    return nullptr;
  }
  if (l_known || r_known) {
    Register* unknown = l_known ? right : left;
    return env.emitCond(
        [&](BasicBlock* t, BasicBlock* f) {
          env.emit<CondBranchCheckType>(unknown, exact, t, f);
        },
        [&] {
          Register* refined = env.emit<RefineType>(exact, unknown);
          return l_known ? fast(left, refined) : fast(refined, right);
        },
        [&] { return slow(left, right); });
  }
  return env.emitCond(
      [&](BasicBlock* t, BasicBlock* f) {
        env.emit<CondBranchCheckType>(left, exact, t, f);
      },
      [&] {
        Register* l = env.emit<RefineType>(exact, left);
        return env.emitCond(
            [&](BasicBlock* t, BasicBlock* f) {
              env.emit<CondBranchCheckType>(right, exact, t, f);
            },
            [&] {
              Register* r = env.emit<RefineType>(exact, right);
              return fast(l, r);
            },
            [&] { return slow(l, right); });
      },
      [&] { return slow(left, right); });
}
#endif

Register* simplifyCompare(Env& env, const Compare* instr) {
  Register* left = instr->GetOperand(0);
  Register* right = instr->GetOperand(1);
  CompareOp op = instr->op();

  if (left->isA(TNoneType) && right->isA(TNoneType)) {
    if (op == CompareOp::kEqual || op == CompareOp::kNotEqual) {
      env.emit<UseType>(left, TNoneType);
      env.emit<UseType>(right, TNoneType);
      return env.emit<LoadConst>(
          Type::fromObject(op == CompareOp::kEqual ? Py_True : Py_False));
    }
  }

  // Can compare booleans for equality with primitive operations.
  if (left->isA(TBool) && right->isA(TBool) &&
      (op == CompareOp::kEqual || op == CompareOp::kNotEqual)) {
    if (auto prim_op = toPrimitiveCompareOp(op)) {
      env.emit<UseType>(left, TBool);
      env.emit<UseType>(right, TBool);
      Register* result = env.emit<PrimitiveCompare>(*prim_op, left, right);
      return env.emit<PrimitiveBoxBool>(result);
    }
  }

  // Emit FloatCompare if both args are FloatExact and the op is supported
  // between two longs.
  if (left->isA(TFloatExact) && right->isA(TFloatExact) &&
      !(op == CompareOp::kIn || op == CompareOp::kNotIn ||
        op == CompareOp::kExcMatch)) {
    return env.emit<FloatCompare>(instr->op(), left, right);
  }

  // Emit LongCompare if both args are LongExact and the op is supported between
  // two longs.
  if (left->isA(TLongExact) && right->isA(TLongExact) &&
      !(op == CompareOp::kIn || op == CompareOp::kNotIn ||
        op == CompareOp::kExcMatch)) {
    return env.emit<LongCompare>(instr->op(), left, right);
  }

  // Emit UnicodeCompare if both args are UnicodeExact and the op is supported
  // between two strings.
  if (left->isA(TUnicodeExact) && right->isA(TUnicodeExact) &&
      !(op == CompareOp::kIn || op == CompareOp::kNotIn ||
        op == CompareOp::kExcMatch)) {
    return env.emit<UnicodeCompare>(instr->op(), left, right);
  }

#if PY_VERSION_HEX < 0x030C0000
  // SPIKE(CMPTYPED): the builder tagged this site with the exact type the
  // 3.11 specialiser observed but withheld the deopting guards.  If the
  // result feeds an IsTruthy in this block, leave it to
  // DynamicComparisonElimination (the diamond is then built around the fused
  // CompareBool and yields a CBool directly); otherwise pin the unknown
  // operand(s) with a type-check diamond whose fast arm is a typed Compare
  // (lowered to the compact-long path on the next iteration).
  if (instr->specializedExact() != TBottom &&
      toPrimitiveCompareOp(op).has_value()) {
    Type exact = instr->specializedExact();
    auto next = std::next(env.cursor);
    while (next != env.block->end() && next->IsSnapshot()) {
      ++next;
    }
    bool feeds_truthy = next != env.block->end() && next->IsIsTruthy() &&
        next->GetOperand(0) == instr->output();
    const FrameState* dominating_fs = instr->getDominatingFrameState();
    // Not under an exception edge: the diamond's result is a Phi, which
    // cannot inherit the edge (see simplifyInPlaceOp).
    if (!feeds_truthy && dominating_fs != nullptr && !env.exc_edge) {
      return emitSpecializedCompareDiamond(
          env,
          exact,
          left,
          right,
          *dominating_fs,
          [&](Register* l, Register* r) -> Register* {
            env.emit<Snapshot>(*dominating_fs);
            return env.emit<Compare>(op, l, r, *instr->frameState());
          },
          [&](Register* l, Register* r) -> Register* {
            return env.emit<Compare>(op, l, r, *instr->frameState());
          });
    }
  }
#endif

  return nullptr;
}

#if PY_VERSION_HEX < 0x030C0000
// SPIKE(STRLITEQ): `x == "lit"` / `x != "lit"` with an exact-str x and a
// short compact-ASCII literal.  Pointer identity answers the interned hit;
// otherwise the length and the compact+ascii state bits are checked and the
// character bytes are compared in at most two chunk loads (the NUL terminator
// pads the last chunk, so every load stays inside the object).  A string
// that is not compact ASCII goes to the helper before its length is used,
// since legacy storage can still need PyUnicode_READY.
Register* trySimplifyStrLiteralCompare(Env& env, const CompareBool* instr) {
  static const bool enabled = Ci_SpikeGetenv("SPIKE_STRLITEQ") != nullptr;
  if (!enabled || instr->inlineLiteralDone()) {
    return nullptr;
  }
  CompareOp op = instr->op();
  if (op != CompareOp::kEqual && op != CompareOp::kNotEqual) {
    return nullptr;
  }
  auto literal_of = [](Register* r) -> PyObject* {
    Type t = r->type();
    if (!t.hasObjectSpec()) {
      return nullptr;
    }
    PyObject* o = t.objectSpec();
    return PyUnicode_CheckExact(o) ? o : nullptr;
  };
  Register* x = instr->left();
  Register* c = instr->right();
  PyObject* lit = literal_of(c);
  if (lit == nullptr) {
    lit = literal_of(x);
    if (lit == nullptr) {
      return nullptr;
    }
    std::swap(x, c);
  }
  if (!x->isA(TUnicodeExact)) {
    return nullptr;
  }
  if (PyObject* xl = literal_of(x)) {
    int r = PyObject_RichCompareBool(
        xl, lit, op == CompareOp::kEqual ? Py_EQ : Py_NE);
    if (r < 0) {
      PyErr_Clear();
      return nullptr;
    }
    env.emit<UseType>(x, x->type());
    env.emit<UseType>(c, c->type());
    return env.emit<LoadConst>(Type::fromCBool(r != 0));
  }
  if (!PyUnicode_IS_COMPACT_ASCII(lit)) {
    return nullptr;
  }
  Py_ssize_t len = PyUnicode_GET_LENGTH(lit);
  if (len < 1 || len > 8) {
    return nullptr;
  }
  // len + 1 bytes are valid: the compact ASCII data is NUL terminated.
  const auto* bytes = static_cast<const unsigned char*>(PyUnicode_DATA(lit));
  struct Chunk {
    Py_ssize_t off;
    int size;
    uint64_t value;
    Type type;
  };
  std::vector<Chunk> chunks;
  for (Py_ssize_t off = 0; off < len;) {
    int size = 1;
    Type type = TCUInt8;
    if (off + 8 <= len + 1) {
      size = 8;
      type = TCUInt64;
    } else if (off + 4 <= len + 1) {
      size = 4;
      type = TCUInt32;
    } else if (off + 2 <= len + 1) {
      size = 2;
      type = TCUInt16;
    }
    uint64_t value = 0;
    for (int i = 0; i < size; ++i) {
      value |= static_cast<uint64_t>(bytes[off + i]) << (8 * i);
    }
    chunks.push_back(Chunk{off, size, value, type});
    off += size;
  }
  env.emit<UseType>(x, TUnicodeExact);
  Register* same = env.emit<PrimitiveCompare>(PrimitiveCompareOp::kEqual, x, c);
  Register* eq = env.emitCond(
      [&](BasicBlock* t, BasicBlock* f) { env.emit<CondBranch>(same, t, f); },
      [&] { return env.emit<LoadConst>(Type::fromCBool(true)); },
      [&] {
        Register* state = env.emit<LoadField>(
            x, "state", offsetof(PyASCIIObject, state), TCUInt8);
        Register* mask = env.emit<LoadConst>(Type::fromCUInt(0x60, TCUInt8));
        Register* bits = env.emit<IntBinaryOp>(BinaryOpKind::kAnd, state, mask);
        Register* compact_ascii =
            env.emit<PrimitiveCompare>(PrimitiveCompareOp::kEqual, bits, mask);
        return env.emitCond(
            [&](BasicBlock* t, BasicBlock* f) {
              env.emit<CondBranch>(compact_ascii, t, f);
            },
            [&] {
              // A legacy, non-ready string has no valid length yet. Only
              // compact ASCII permits reading length without PyUnicode_READY.
              Register* length = env.emit<LoadField>(
                  x, "length", offsetof(PyASCIIObject, length), TCInt64);
              Register* want =
                  env.emit<LoadConst>(Type::fromCInt(len, TCInt64));
              Register* len_ok = env.emit<PrimitiveCompare>(
                  PrimitiveCompareOp::kEqual, length, want);
              return env.emitCond(
                  [&](BasicBlock* t, BasicBlock* f) {
                    env.emit<CondBranch>(len_ok, t, f);
                  },
                  [&] {
                    Register* acc = nullptr;
                    for (const Chunk& ch : chunks) {
                      Register* v = env.emit<LoadField>(
                          x, "data", sizeof(PyASCIIObject) + ch.off, ch.type);
                      Register* k = env.emit<LoadConst>(
                          Type::fromCUInt(ch.value, ch.type));
                      Register* e = env.emit<PrimitiveCompare>(
                          PrimitiveCompareOp::kEqual, v, k);
                      acc = acc == nullptr
                          ? e
                          : env.emit<IntBinaryOp>(BinaryOpKind::kAnd, acc, e);
                    }
                    return acc;
                  },
                  [&] { return env.emit<LoadConst>(Type::fromCBool(false)); });
            },
            [&] {
              auto* helper = env.emitInstr<CompareBool>(
                  CompareOp::kEqual, x, c, *instr->frameState());
              helper->setInlineLiteralDone();
              return helper->output();
            });
      });
  if (op == CompareOp::kNotEqual) {
    Register* no = env.emit<LoadConst>(Type::fromCBool(false));
    return env.emit<PrimitiveCompare>(PrimitiveCompareOp::kEqual, eq, no);
  }
  return eq;
}

Register* simplifyCompareBool(Env& env, const CompareBool* instr) {
  if (Register* literal = trySimplifyStrLiteralCompare(env, instr)) {
    return literal;
  }
  Type exact = instr->specializedExact();
  // Under an exception edge the generic form keeps it (see simplifyCompare).
  if (exact == TBottom || env.exc_edge) {
    return nullptr;
  }
  Register* left = instr->GetOperand(0);
  Register* right = instr->GetOperand(1);
  auto prim_op = toPrimitiveCompareOp(instr->op());
  const FrameState* dominating_fs = instr->getDominatingFrameState();
  if (!prim_op.has_value() || dominating_fs == nullptr) {
    return nullptr;
  }
  // SPIKE(INT64ARITH): a site whose inline guards kept failing (operands of
  // three or more digits) stays on the generic helper.  The fuse is keyed by
  // the frame the guards bind to, which is what a deopt reports: for a
  // compare fused with its jump that is the COMPARE_OP, while the CompareBool
  // itself carries the offset of the POP_JUMP.
  static const bool spike_int64arith_fuse =
      Ci_SpikeGetenv("SPIKE_INT64ARITH") != nullptr;
  PyCodeObject* fuse_code = dominating_fs->code.get();
  int64_t fuse_site = dominating_fs->cur_instr_offs.value();
  if (spike_int64arith_fuse && fuse_code != nullptr &&
      Ci_SpikeNoLongSpec311(fuse_code, fuse_site)) {
    return nullptr;
  }
  return emitSpecializedCompareDiamond(
      env,
      exact,
      left,
      right,
      *dominating_fs,
      [&](Register* l, Register* r) -> Register* {
        // Inline compact-long comparison; the Guard binds to this Snapshot
        // (operands still on the stack) so a deopt re-executes the compare.
        env.emit<Snapshot>(*dominating_fs);
        // SPIKE(INT64ARITH): two-digit operands compare exactly in int64.
        static const bool spike_int64arith =
            Ci_SpikeGetenv("SPIKE_INT64ARITH") != nullptr;
        if (spike_int64arith && l->isA(TLongExact) && r->isA(TLongExact) &&
            (int64WideOperand311(env, l) || int64WideOperand311(env, r))) {
          bool guarded = false;
          Register* a = nullptr;
          Register* b = nullptr;
          if (int64OperandPair311(env, l, r, dominating_fs, guarded, a, b)) {
            if (guarded && fuse_code != nullptr) {
              Ci_SpikeLongSpecRegister311(fuse_code, fuse_site);
            }
            return env.emit<PrimitiveCompare>(*prim_op, a, b);
          }
        }
        if (spike_int64arith && fuse_code != nullptr) {
          // The compact form keeps its shape; it joins the per-site fuse.
          Ci_SpikeLongSpecRegister311(fuse_code, fuse_site);
        }
        Register* l_compact = env.emit<IsCompactLong>(l);
        Register* r_compact = env.emit<IsCompactLong>(r);
        Register* both =
            env.emit<IntBinaryOp>(BinaryOpKind::kAnd, l_compact, r_compact);
        env.emitInstr<Guard>(both);
        Register* l_val = env.emit<CompactLongUnbox>(l);
        Register* r_val = env.emit<CompactLongUnbox>(r);
        return env.emit<PrimitiveCompare>(*prim_op, l_val, r_val);
      },
      [&](Register* l, Register* r) -> Register* {
        return env.emit<CompareBool>(instr->op(), l, r, *instr->frameState());
      });
}
#endif

Register* simplifyLongCompare(Env& env, const LongCompare* instr) {
  Register* left = instr->GetOperand(0);
  Register* right = instr->GetOperand(1);
  CompareOp op = instr->op();

  // TODO: Constant folding.

  auto prim_op = toPrimitiveCompareOp(op);
  if (!prim_op.has_value()) {
    return nullptr;
  }

#if PY_VERSION_HEX < 0x030C0000
  // SPIKE(INT64ARITH): a comparison is exact for any int64, so the operands
  // take the two-digit unbox (boxes looked through, constants folded) and
  // the site joins the per-site fuse; the guards bind to the dominating
  // Snapshot as the compact form's did.
  static const bool spike_int64arith =
      Ci_SpikeGetenv("SPIKE_INT64ARITH") != nullptr;
  if (spike_int64arith) {
    // Keyed by the frame the guards bind to, as in simplifyCompareBool.
    const FrameState* dominating_fs = instr->getDominatingFrameState();
    PyCodeObject* code = dominating_fs != nullptr ? dominating_fs->code.get()
                                                  : env.func.code.get();
    int64_t site = dominating_fs != nullptr
        ? dominating_fs->cur_instr_offs.value()
        : instr->bytecodeOffset().value();
    if (code != nullptr && Ci_SpikeNoLongSpec311(code, site)) {
      // Fused site: the generic LongCompare stays.
      return nullptr;
    }
    if (code != nullptr && dominating_fs != nullptr &&
        (int64WideOperand311(env, left) || int64WideOperand311(env, right))) {
      bool guarded = false;
      Register* a = nullptr;
      Register* b = nullptr;
      if (int64OperandPair311(env, left, right, dominating_fs, guarded, a, b)) {
        if (guarded) {
          Ci_SpikeLongSpecRegister311(code, site);
        }
        Register* unboxed_result = env.emit<PrimitiveCompare>(*prim_op, a, b);
        return env.emit<PrimitiveBoxBool>(unboxed_result);
      }
    }
    if (code != nullptr) {
      // The compact form below keeps its shape; it joins the per-site fuse.
      Ci_SpikeLongSpecRegister311(code, site);
    }
  }
#endif

  // Guard that both sides are compact longs.
  Register* is_left_compact = env.emit<IsCompactLong>(left);
  Register* is_right_compact = env.emit<IsCompactLong>(right);
  Register* both_compact = env.emit<IntBinaryOp>(
      BinaryOpKind::kAnd, is_left_compact, is_right_compact);
  env.emitInstr<Guard>(both_compact);

  Register* compact_left = env.emit<CompactLongUnbox>(left);
  Register* compact_right = env.emit<CompactLongUnbox>(right);
  Register* unboxed_result =
      env.emit<PrimitiveCompare>(*prim_op, compact_left, compact_right);
  return env.emit<PrimitiveBoxBool>(unboxed_result);
}

Register* simplifyCondBranch(Env& env, const CondBranch* instr) {
  Register* cond = instr->GetOperand(0);
  Type cond_type = cond->type();
  // Constant condition folds into an unconditional jump.
  if (cond_type.hasIntSpec()) {
    auto spec = cond_type.intSpec();
    return env.emit<Branch>(spec ? instr->true_bb() : instr->false_bb());
  }
  // Common pattern of CondBranch getting its condition from a PrimitiveConvert,
  // which had been simplified down from an IsTruthy.  Can forward the value
  // only if it's being widened.  Narrowing an integer might change it from
  // non-zero to zero.
  if (cond->instr()->IsPrimitiveConvert()) {
    auto convert = static_cast<PrimitiveConvert*>(cond->instr());
    Register* src = convert->src();
    if (convert->type().sizeInBytes() >= src->type().sizeInBytes()) {
      return env.emit<CondBranch>(src, instr->true_bb(), instr->false_bb());
    }
  }
  if (cond->instr()->IsPrimitiveUnaryOp()) {
    auto unary = static_cast<PrimitiveUnaryOp*>(cond->instr());
    auto unary_op = unary->op();
    if (unary_op == PrimitiveUnaryOpKind::kNotInt) {
      return env.emit<CondBranch>(
          unary->GetOperand(0), instr->false_bb(), instr->true_bb());
    }
  }
  return nullptr;
}

Register* simplifyCondBranchCheckType(
    Env& env,
    const CondBranchCheckType* instr) {
  Register* value = instr->GetOperand(0);
  Type actual_type = value->type();
  Type expected_type = instr->type();
  if (actual_type <= expected_type) {
    env.emit<UseType>(value, actual_type);
    return env.emit<Branch>(instr->true_bb());
  }
  if (!actual_type.couldBe(expected_type)) {
    env.emit<UseType>(value, actual_type);
    return env.emit<Branch>(instr->false_bb());
  }
  return nullptr;
}

Register* simplifyIsTruthy(Env& env, const IsTruthy* instr) {
  Type ty = instr->GetOperand(0)->type();
  PyObject* obj = ty.asObject();
  if (obj != nullptr) {
    // Should only consider immutable Objects
    static const std::unordered_set<PyTypeObject*> kTrustedTypes{
        &PyBool_Type,
        &PyFloat_Type,
        &PyLong_Type,
        &PyFrozenSet_Type,
        &PySlice_Type,
        &PyTuple_Type,
        &PyUnicode_Type,
        Py_TYPE(Py_None),
    };
    if (kTrustedTypes.contains(Py_TYPE(obj))) {
      int res = PyObject_IsTrue(obj);
      JIT_CHECK(res >= 0, "PyObject_IsTrue failed on trusted type");
      // Since we no longer use instr->GetOperand(0), we need to make sure that
      // we don't lose any associated type checks
      env.emit<UseType>(instr->GetOperand(0), ty);
      return env.emit<LoadConst>(Type::fromCBool(res));
    }
  }
#if PY_VERSION_HEX < 0x030C0000
  // SPIKE(CMPFUSE): an exact-str compare whose boxed bool feeds this truth
  // test.  CompareBool keeps the typed lowering (JITRT_UnicodeEquals) and
  // yields the CBool directly, so the answer is never boxed, incref'd and
  // unboxed again.  The UnicodeCompare is pure and stays behind; it dies
  // with its last use.  LongCompare is deliberately left alone: this
  // IsTruthy is visited in the same pass that created the LongCompare, and
  // fusing here would pre-empt simplifyLongCompare's unboxed
  // PrimitiveCompare on compact operands with a helper call.
  {
    // Simplify keeps the replaced compare's output alive through an Assign
    // until copy propagation, so look through those.
    const Instr* def = instr->GetOperand(0)->instr();
    while (def != nullptr && def->IsAssign()) {
      def = def->GetOperand(0)->instr();
    }
    if (def != nullptr && def->IsUnicodeCompare()) {
      auto* cmp = static_cast<const UnicodeCompare*>(def);
      return env.emit<CompareBool>(
          cmp->op(), cmp->left(), cmp->right(), *instr->frameState());
    }
  }
#endif
  if (ty <= TBool) {
    Register* left = instr->GetOperand(0);
    env.emit<UseType>(left, TBool);
    Register* right = env.emit<LoadConst>(Type::fromObject(Py_True));
    Register* result =
        env.emit<PrimitiveCompare>(PrimitiveCompareOp::kEqual, left, right);
    return result;
  }
  if (Register* size = emitGetLengthInt64(env, instr->GetOperand(0))) {
    return env.emit<CIntToCBool>(size);
  }
  if (ty <= TLongExact) {
    Register* left = instr->GetOperand(0);
#if PY_VERSION_HEX < 0x030C0000
    // SPIKE(INTBITSPEC): the truth of a boxed CInt64 is the value itself;
    // `if cells & mask:` then never allocates the int it only tests.  The
    // box is left with frame-state uses only and is rematerialised on deopt.
    static const bool spike_intbitspec =
        Ci_SpikeGetenv("SPIKE_INTBITSPEC") != nullptr;
    if (spike_intbitspec) {
      const Instr* def = left->instr();
      while (def != nullptr && def->IsAssign()) {
        def = def->GetOperand(0)->instr();
      }
      if (def != nullptr && def->IsPrimitiveBox() &&
          static_cast<const PrimitiveBox*>(def)->type() <= TCInt64) {
        return env.emit<CIntToCBool>(
            static_cast<const PrimitiveBox*>(def)->value());
      }
    }
#endif
    env.emit<UseType>(left, ty);
    Register* right = env.emit<LoadConst>(Type::fromObject(_PyLong_GetZero()));
    Register* result =
        env.emit<PrimitiveCompare>(PrimitiveCompareOp::kNotEqual, left, right);
    return result;
  }
  return nullptr;
}

Register* simplifyIsCompactLong(Env& env, const IsCompactLong* instr) {
  Type ty = instr->GetOperand(0)->type();
  JIT_CHECK(
      ty <= TLongExact || ty <= TCInt64,
      "IsCompactLong generated for invalid type '{}'",
      ty);
  if (ty.hasObjectSpec()) {
    env.emit<UseType>(instr->GetOperand(0), ty);
    bool compact =
        _PyLong_IsCompact(reinterpret_cast<PyLongObject*>(ty.objectSpec()));
    return env.emit<LoadConst>(Type::fromCBool(compact));
  }

  // IsCompactLong(box(n)) --> IsCompactLong(n).
  Register* operand = instr->GetOperand(0);
  if (operand->instr()->IsPrimitiveBox()) {
    auto* box = static_cast<const PrimitiveBox*>(operand->instr());
    if (box->type() <= TCInt64) {
      return env.emit<IsCompactLong>(box->value());
    }
  }

  return nullptr;
}

Register* simplifyCompactLongUnbox(Env& env, const CompactLongUnbox* instr) {
  Type ty = instr->GetOperand(0)->type();
  JIT_CHECK(
      ty <= TLongExact, "CompactLongUnbox generated for invalid type '{}'", ty);
  if (ty.hasObjectSpec()) {
    auto* long_obj = reinterpret_cast<PyLongObject*>(ty.objectSpec());
    if (!_PyLong_IsCompact(long_obj)) {
      // Should be unreachable.
      return nullptr;
    }
    env.emit<UseType>(instr->GetOperand(0), ty);
    Py_ssize_t value = _PyLong_CompactValue(long_obj);
    return env.emit<LoadConst>(Type::fromCInt(value, TCInt64));
  }

  // CompactLongUnbox(box(n)) --> n.
  Register* operand = instr->GetOperand(0);
  if (operand->instr()->IsPrimitiveBox()) {
    auto* box = static_cast<const PrimitiveBox*>(operand->instr());
    if (box->type() <= TCInt64) {
      return box->value();
    }
  }

  return nullptr;
}

Register* simplifyLoadTupleItem(Env& env, const LoadTupleItem* instr) {
  Register* src = instr->GetOperand(0);
  Type src_ty = src->type();
  if (!src_ty.hasValueSpec(TTuple)) {
    return nullptr;
  }
  env.emit<UseType>(src, src_ty);
  BorrowedRef<> item = PyTuple_GET_ITEM(src_ty.objectSpec(), instr->idx());
  return env.emit<LoadConst>(Type::fromObject(env.func.env.addReference(item)));
}

Register* simplifyLoadArrayItem(Env& env, const LoadArrayItem* instr) {
  Register* src = instr->seq();
  if (!instr->idx()->type().hasIntSpec()) {
    return nullptr;
  }
  intptr_t idx_signed = instr->idx()->type().intSpec();
  JIT_CHECK(idx_signed >= 0, "LoadArrayItem should not have negative index");
  uintptr_t idx = static_cast<uintptr_t>(idx_signed);
  // We can only do this for tuples because lists and arrays, the other
  // sequence types, are mutable. A more general LoadElimination pass could
  // accomplish that, though.
  if (src->instr()->IsMakeTuple()) {
    size_t length = static_cast<const MakeTuple*>(src->instr())->nvalues();
    if (idx < length) {
      // Find the InitTupleElements that fills this tuple.
      auto* block = src->instr()->block();
      for (auto it = block->iterator_to(*src->instr()); it != block->end();
           ++it) {
        if (it->IsInitTupleElements() && it->GetOperand(0) == src) {
          env.emit<UseType>(src, TTupleExact);
          env.emit<UseType>(instr->idx(), instr->idx()->type());
          return it->GetOperand(idx + 1);
        }
      }
    }
  }
  if (src->type().hasValueSpec(TTupleExact)) {
    if (idx_signed < PyTuple_GET_SIZE(src->type().objectSpec())) {
      env.emit<UseType>(src, src->type());
      env.emit<UseType>(instr->idx(), instr->idx()->type());
      BorrowedRef<> item = PyTuple_GET_ITEM(src->type().objectSpec(), idx);
      return env.emit<LoadConst>(
          Type::fromObject(env.func.env.addReference(item)));
    }
  }
  return nullptr;
}

Register* simplifyLoadVarObjectSize(Env& env, const LoadVarObjectSize* instr) {
  Register* obj_reg = instr->GetOperand(0);
  Type type = obj_reg->type();
  // We can only do this for tuples because lists and arrays, the other
  // sequence types, are mutable. A more general LoadElimination pass could
  // accomplish that, though.
  if (obj_reg->instr()->IsMakeTuple()) {
    env.emit<UseType>(obj_reg, type);
    size_t size = static_cast<const MakeTuple*>(obj_reg->instr())->nvalues();
    Type output_type = instr->output()->type();
    return env.emit<LoadConst>(Type::fromCInt(size, output_type));
  }
  if (type.hasValueSpec(TTupleExact) || type.hasValueSpec(TBytesExact)) {
    PyVarObject* obj = reinterpret_cast<PyVarObject*>(type.asObject());
    Py_ssize_t size = obj->ob_size;
    env.emit<UseType>(obj_reg, type);
    Type output_type = instr->output()->type();
    return env.emit<LoadConst>(Type::fromCInt(size, output_type));
  }
  return nullptr;
}

Register* simplifyLoadModuleMethodCached(
    Env& env,
    const LoadMethod* load_meth) {
  Register* receiver = load_meth->GetOperand(0);
  int name_idx = load_meth->name_idx();
  return env.emit<LoadModuleMethodCached>(
      receiver, name_idx, *load_meth->frameState());
}

Register* simplifyLoadTypeMethodCached(Env& env, const LoadMethod* load_meth) {
  Register* receiver = load_meth->GetOperand(0);
  const int cache_id = env.func.env.allocateLoadTypeMethodCache();
  env.emit<UseType>(receiver, TType);
#if PY_VERSION_HEX < 0x030C0000
  // On 3.11 the entry-value arm is pull-validated in getValueHelper, which
  // re-runs the full lookup when stale -- and that lookup can raise
  // (attribute deleted from the class).  LoadTypeMethodCacheEntryValue
  // carries no exception check, so the pair {NULL, NULL} would flow into
  // the call.  Route every load through the deopt-checked fill instruction;
  // its 3.11 lowering still serves validated hits without the MRO walk.
  return env.emit<FillTypeMethodCache>(
      receiver, load_meth->name_idx(), cache_id, *load_meth->frameState());
#endif
  Register* guard = env.emit<LoadTypeMethodCacheEntryType>(cache_id);
  Register* type_matches =
      env.emit<PrimitiveCompare>(PrimitiveCompareOp::kEqual, guard, receiver);
  return env.emitCond(
      [&](BasicBlock* fast_path, BasicBlock* slow_path) {
        env.emit<CondBranch>(type_matches, fast_path, slow_path);
      },
      [&] { // Fast path
        return env.emit<LoadTypeMethodCacheEntryValue>(cache_id, receiver);
      },
      [&] { // Slow path
        int name_idx = load_meth->name_idx();
        return env.emit<FillTypeMethodCache>(
            receiver, name_idx, cache_id, *load_meth->frameState());
      });
}

Register* simplifyLoadMethod(Env& env, const LoadMethod* load_meth) {
  if (!getConfig().attr_caches) {
    return nullptr;
  }
  Register* receiver = load_meth->GetOperand(0);
  Type ty = receiver->type();
  if (receiver->isA(TType)) {
    // On 3.11 the entry is pull-validated in getValueHelper against the
    // owner type's version AND the metaclass identity/version, so the
    // inline "type pointer matches" arm cannot serve a stale answer.
    return simplifyLoadTypeMethodCached(env, load_meth);
  }
  BorrowedRef<PyTypeObject> type{ty.runtimePyType()};
  if (type == &PyModule_Type || type == &Ci_StrictModule_Type) {
    return simplifyLoadModuleMethodCached(env, load_meth);
  }
  return env.emit<LoadMethodCached>(
      load_meth->GetOperand(0),
      load_meth->name_idx(),
      *load_meth->frameState());
}

bool hasArraySubscrFastPathEvidence(
    Register* container,
    Register* sub,
    Type array_guard) {
  return container->isA(array_guard) || sub->isA(TLongExact);
}

Register*
checkConstantListOrTupleIndex(Env& env, Register* container, Register* index) {
  int overflow;
  Py_ssize_t index_val =
      PyLong_AsLongAndOverflow(index->type().objectSpec(), &overflow);
  if (overflow) {
    return nullptr;
  }

  if (container->isA(TTupleExact) && container->instr()->IsMakeTuple()) {
    size_t length =
        static_cast<const MakeTuple*>(container->instr())->nvalues();
    if (index_val < 0) {
      index_val += length;
    }
    if (static_cast<size_t>(index_val) < length) {
      env.emit<UseType>(container, container->type());
      env.emit<UseType>(index, index->type());
      return env.emit<LoadConst>(Type::fromCInt(index_val, TCInt64));
    }
    return nullptr;
  }

  env.emit<UseType>(container, container->type());
  env.emit<UseType>(index, index->type());

  Register* length = emitGetLengthInt64(env, container);
  if (index_val < 0) {
    Register* tmp = env.emit<LoadConst>(Type::fromCInt(index_val, TCInt64));
    index = env.emit<IntBinaryOp>(BinaryOpKind::kAdd, tmp, length);
  } else {
    index = env.emit<LoadConst>(Type::fromCInt(index_val, TCInt64));
  }
  // Unsigned comparison rejects too-large positive indices and normalized
  // indices that remain negative (for example len=3, index=-4).
  Register* in_bounds = env.emit<PrimitiveCompare>(
      PrimitiveCompareOp::kLessThanUnsigned, index, length);
  env.emit<Guard>(in_bounds);
  return index;
}

Register* unboxAndCheckListOrTupleIndex(
    Env& env,
    const DeoptBase* instr,
    Register* container,
    Register* index) {
  JIT_CHECK(
      container->isA(TListExact) || container->isA(TTupleExact),
      "container must be a TListExact or TTupleExact, not a {}",
      container->type());
  JIT_CHECK(
      index->isA(TLongExact),
      "index must be a TLongExact, not a {}",
      index->type());
  JIT_CHECK(!kFreeThreadedBuild, "only valid for the default build");

  if (index->type().hasObjectSpec()) {
    return checkConstantListOrTupleIndex(env, container, index);
  }

  env.emit<UseType>(
      container, container->isA(TListExact) ? TListExact : TTupleExact);
  env.emit<UseType>(index, TLongExact);

  Register* is_compact_long = env.emit<IsCompactLong>(index);
  env.emit<Guard>(is_compact_long);
  Register* unboxed_index = env.emit<CompactLongUnbox>(index);

  Register* length = emitGetLengthInt64(env, container);
  Register* zero = env.emit<LoadConst>(Type::fromCInt(0, TCInt64));
  Register* is_negative = env.emit<PrimitiveCompare>(
      PrimitiveCompareOp::kLessThan, unboxed_index, zero);

  // Capture state before emitCond splits the block. Some HIR shapes have no
  // separately dominating Snapshot, so use the instruction's own FrameState
  // as the safe replay point in that case.
  const FrameState* dominating_fs = instr->getDominatingFrameState();
  if (dominating_fs == nullptr) {
    dominating_fs = instr->frameState();
  }
  JIT_CHECK(
      dominating_fs != nullptr,
      "no FrameState available for list/tuple index bounds check");

  Register* normalized_index = env.emitCond(
      [&](BasicBlock* true_bb, BasicBlock* false_bb) {
        env.emit<CondBranch>(is_negative, true_bb, false_bb);
      },
      [&] {
        return env.emit<IntBinaryOp>(BinaryOpKind::kAdd, unboxed_index, length);
      },
      [&] { return unboxed_index; });

  env.emit<Snapshot>(*dominating_fs);
  Register* in_bounds = env.emit<PrimitiveCompare>(
      PrimitiveCompareOp::kLessThanUnsigned, normalized_index, length);
  env.emit<Guard>(in_bounds);
  return normalized_index;
}

// Emit the array.array('d') BINARY_SUBSCR fast path. When the container's
// static type is already array.array, or the index is already known to be a
// Python int, speculatively guard on array.array('d') and lower the load to an
// inlined LoadArrayItem(TCDouble) + PrimitiveBox. Otherwise fall back to a
// generic BinaryOp<Subscript>. Returns the merged value, or nullptr if the fast
// path does not apply (caller continues with the normal subscript lowering).
//
// This runs in the Simplify pass rather than the HIR builder so that it only
// fires after earlier type propagation has had a chance to prove either the
// container or index shape. Subscripts on statically-typed list/tuple/dict/str
// containers are already lowered by simplifyBinaryOp and never reach here.
// SPIKE(ARRIDX): inline bounds check for a list/tuple/array.array container
// and an unboxed CInt64 index: normalise a negative index by ob_size, then
// Guard on unsigned index < length.  The Guard binds to the Snapshot emitted
// here from the dominating FrameState (operands still on the stack), so a
// deopt re-executes the subscript in the interpreter and raises the native
// IndexError.  Returns nullptr when no dominating FrameState is available.
Register* tryEmitInlineBoundsCheck(
    Env& env,
    const FrameState* dominating_fs,
    Register* container,
    Register* unboxed_index) {
  if (dominating_fs == nullptr) {
    return nullptr;
  }
  Register* length = nullptr;
  PyTypeObject* array_type = getStdlibArrayType();
  if (array_type != nullptr &&
      container->isA(Type::fromTypeExact(array_type))) {
    // array.array is a PyVarObject; TArray in emitGetLengthInt64 is the
    // static-Python array type, not the stdlib one.
    length = env.emit<LoadField>(
        container, "ob_size", offsetof(PyVarObject, ob_size), TCInt64);
  } else {
    length = emitGetLengthInt64(env, container);
  }
  if (length == nullptr) {
    return nullptr;
  }
  Register* zero = env.emit<LoadConst>(Type::fromCInt(0, TCInt64));
  Register* is_negative = env.emit<PrimitiveCompare>(
      PrimitiveCompareOp::kLessThan, unboxed_index, zero);
  Register* normalized_index = env.emitCond(
      [&](BasicBlock* true_bb, BasicBlock* false_bb) {
        env.emit<CondBranch>(is_negative, true_bb, false_bb);
      },
      [&] {
        return env.emit<IntBinaryOp>(BinaryOpKind::kAdd, unboxed_index, length);
      },
      [&] { return unboxed_index; });
  env.emit<Snapshot>(*dominating_fs);
  Register* in_bounds = env.emit<PrimitiveCompare>(
      PrimitiveCompareOp::kLessThanUnsigned, normalized_index, length);
  env.emit<Guard>(in_bounds);
  return normalized_index;
}

bool spikeArrIdx() {
  static const bool on = Ci_SpikeGetenv("SPIKE_ARRIDX") != nullptr;
  return on;
}

#if PY_VERSION_HEX < 0x030C0000
// SPIKE(STRCHR): str[i] on a compact-ASCII string with an in-range index is
// a byte load and a table lookup.  The table holds CPython's own latin-1
// character singletons (permanent objects), so the fetched value is a
// borrowed reference and the refcount pass makes it owned only where it
// escapes, exactly as for a list item.  Other storage kinds keep sq_item.
PyObject* const* spikeLatin1Table() {
  // Compile workers share the main thread's GIL, but must serialize writes
  // with each other. Protect both table publication and the reference-count
  // writes in PyUnicode_FromOrdinal with the same lock used by the compiler.
  ThreadedCompileSerialize guard;
  static PyObject* table[256];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 256; ++i) {
      // New references to the singletons, held for the life of the process.
      table[i] = PyUnicode_FromOrdinal(i);
      JIT_CHECK(table[i] != nullptr, "latin-1 singleton table");
    }
    init = true;
  }
  return table;
}

Register* emitUnicodeCharFetch(
    Env& env,
    Register* str,
    Register* adjusted_idx,
    const FrameState& frame) {
  static const bool spike_strchr = Ci_SpikeGetenv("SPIKE_STRCHR") != nullptr;
  if (!spike_strchr || !adjusted_idx->isA(TCInt64)) {
    return env.emit<UnicodeSubscr>(str, adjusted_idx, frame);
  }
  Register* state = env.emit<LoadField>(
      str, "state", offsetof(PyASCIIObject, state), TCUInt8);
  Register* mask = env.emit<LoadConst>(Type::fromCUInt(0x60, TCUInt8));
  Register* bits = env.emit<IntBinaryOp>(BinaryOpKind::kAnd, state, mask);
  Register* compact_ascii =
      env.emit<PrimitiveCompare>(PrimitiveCompareOp::kEqual, bits, mask);
  return env.emitCond(
      [&](BasicBlock* t, BasicBlock* f) {
        env.emit<CondBranch>(compact_ascii, t, f);
      },
      [&] {
        Register* data_off =
            env.emit<LoadConst>(Type::fromCInt(sizeof(PyASCIIObject), TCInt64));
        Register* data = env.emit<LoadFieldAddress>(str, data_off);
        Register* ch = env.emit<LoadArrayItem>(
            data, adjusted_idx, str, static_cast<intptr_t>(0), TCUInt8);
        Register* ch_idx = env.emit<PrimitiveConvert>(ch, TCInt64);
        Register* table = env.emit<LoadConst>(
            Type::fromCPtr(const_cast<PyObject**>(spikeLatin1Table())));
        return env.emit<LoadArrayItem>(
            table, ch_idx, str, static_cast<intptr_t>(0), TUnicodeExact);
      },
      [&] { return env.emit<UnicodeSubscr>(str, adjusted_idx, frame); });
}
#else
Register* emitUnicodeCharFetch(
    Env& env,
    Register* str,
    Register* adjusted_idx,
    const FrameState& frame) {
  return env.emit<UnicodeSubscr>(str, adjusted_idx, frame);
}
#endif

#if PY_VERSION_HEX < 0x030C0000
// SPIKE(ARRTC): the interpreter's record says this site has only seen exact
// array.array containers of one typecode.  Speculate exactly that, as a
// straight line of guards bound to the dominating (pre-pop) frame state -- a
// failing guard re-executes the subscript in the interpreter -- so that the
// element comes out as a box of a primitive with no join: the diamond's Phi
// would keep the arithmetic downstream from seeing through the box (the
// lesson recorded at the BINTYPED fuse).  The container type is tested by an
// ob_type compare rather than GuardType, which guard removal would drop since
// only raw field reads follow.  A site whose guards keep failing is
// recompiled with the diamond (Ci_SpikeNoArrTc311).  Returns nullptr, having
// emitted nothing, when the site does not qualify.
Register* trySpeculateArrayTypecode311(
    Env& env,
    const BinaryOp* instr,
    Register* container,
    Register* sub,
    PyTypeObject* array_type,
    const FrameState* dominating_fs,
    const FrameState& frame) {
  static const bool spike_arrtc = Ci_SpikeGetenv("SPIKE_ARRTC") != nullptr;
  char tc = instr->subscrArrayTypecode();
  if (!spike_arrtc || (tc != 'd' && tc != 'i') || dominating_fs == nullptr ||
      dominating_fs->parent != nullptr || env.exc_edge || !spikeArrIdx()) {
    return nullptr;
  }
  PyCodeObject* code = dominating_fs->code;
  int64_t site = dominating_fs->cur_instr_offs.value();
  if (code == nullptr || Ci_SpikeNoArrTc311(code, site)) {
    return nullptr;
  }
  auto guard = [&](Register* cond) {
    auto* g = env.emitInstr<Guard>(cond);
    g->setFrameState(*dominating_fs);
  };

  Register* ob_type = env.emit<LoadField>(
      container, "ob_type", offsetof(PyObject, ob_type), TType);
  Register* want_type = env.emit<LoadConst>(
      Type::fromObject(reinterpret_cast<PyObject*>(array_type)));
  guard(env.emit<PrimitiveCompare>(
      PrimitiveCompareOp::kEqual, ob_type, want_type));
  Register* arr =
      env.emit<RefineType>(Type::fromTypeExact(array_type), container);
  Register* descr = env.emit<LoadField>(
      arr, "ob_descr", offsetof(StdlibArrayObject, ob_descr), TCPtr);
  Register* typecode = env.emit<LoadField>(
      descr, "typecode", offsetof(StdlibArrayDescr, typecode), TCInt8);
  Register* want_tc = env.emit<LoadConst>(Type::fromCInt(tc, TCInt8));
  guard(env.emit<PrimitiveCompare>(
      PrimitiveCompareOp::kEqual, typecode, want_tc));

  // The index: an exact int with a single digit (a box of an int64 folds to
  // the int64 itself).
  Register* idx = sub;
  if (!sub->isA(TLongExact)) {
    Register* sub_type =
        env.emit<LoadField>(sub, "ob_type", offsetof(PyObject, ob_type), TType);
    Register* long_type = env.emit<LoadConst>(
        Type::fromObject(reinterpret_cast<PyObject*>(&PyLong_Type)));
    guard(env.emit<PrimitiveCompare>(
        PrimitiveCompareOp::kEqual, sub_type, long_type));
    idx = env.emit<RefineType>(TLongExact, sub);
  }
  guard(env.emit<IsCompactLong>(idx));
  Register* unboxed_idx = env.emit<CompactLongUnbox>(idx);
  Register* adjusted_idx =
      tryEmitInlineBoundsCheck(env, dominating_fs, arr, unboxed_idx);
  JIT_CHECK(
      adjusted_idx != nullptr,
      "an exact array.array always has an inline length");

  Register* ob_item = env.emit<LoadField>(
      arr, "ob_item", offsetof(StdlibArrayObject, ob_item), TCPtr);
  Register* result = nullptr;
  if (tc == 'd') {
    Register* raw = env.emit<LoadArrayItem>(
        ob_item, adjusted_idx, arr, static_cast<intptr_t>(0), TCDouble);
    result = env.emit<PrimitiveBox>(raw, TCDouble, frame);
  } else {
    Register* raw = env.emit<LoadArrayItem>(
        ob_item, adjusted_idx, arr, static_cast<intptr_t>(0), TCInt32);
    Register* widened = env.emit<PrimitiveConvert>(raw, TCInt64);
    result = env.emit<PrimitiveBox>(widened, TCInt64, frame);
  }
  Ci_SpikeArrTcRegister311(code, site);
  return result;
}
#endif

Register* trySimplifyArraySubscr(Env& env, const BinaryOp* instr) {
  if (!getConfig().specialized_opcodes) {
    return nullptr;
  }
  // The generic BinaryOp we leave on the slow path must not be re-specialized.
  if (instr->isArraySubscrSlowPath()) {
    return nullptr;
  }
  PyTypeObject* array_type = getStdlibArrayType();
  if (array_type == nullptr) {
    return nullptr;
  }

  Register* container = instr->left();
  Register* sub = instr->right();
  Type array_guard = Type::fromTypeExact(array_type);
  // Skip containers whose type already rules out array.array (list, tuple,
  // dict, str, or any other known-exact type).
  if (!container->type().couldBe(array_guard)) {
    return nullptr;
  }
  // Likewise skip an index whose type already rules out int (a Slice from a
  // pinned `slice(...)` constructor call, for instance): the diamond's
  // CondBranchCheckType<LongExact> could only fold to the slow arm.
  if (!sub->type().couldBe(TLongExact)) {
    return nullptr;
  }
  if (!hasArraySubscrFastPathEvidence(container, sub, array_guard)) {
    return nullptr;
  }

  const FrameState& frame = *instr->frameState();
  // SPIKE(ARRIDX): capture the dominating FrameState before the block is
  // split below -- afterwards the instruction heads the tail block and has no
  // preceding Snapshot to find.
  const FrameState* dominating_fs = instr->getDominatingFrameState();
  CFG& cfg = env.func.cfg;

#if PY_VERSION_HEX < 0x030C0000
  if (Register* result = trySpeculateArrayTypecode311(
          env, instr, container, sub, array_type, dominating_fs, frame)) {
    return result;
  }
#endif

  BasicBlock* arr_ok = cfg.AllocateBlock();
  BasicBlock* idx_ok = cfg.AllocateBlock();
  BasicBlock* tc_ok = cfg.AllocateBlock();
  BasicBlock* slow = cfg.AllocateBlock();
  env.new_blocks += 5; // arr_ok, idx_ok, tc_ok, slow, and the split-off tail.

  // Guard: container is array.array. Split the continuation into `done`.
  env.emit<CondBranchCheckType>(container, array_guard, arr_ok, slow);
  BasicBlock* done = cfg.splitAfter(*std::prev(env.cursor));

  // --- arr_ok: container confirmed array.array, guard the index type ---
  env.block = arr_ok;
  env.cursor = arr_ok->end();
  Register* arr = env.emit<RefineType>(array_guard, container);
  env.emit<CondBranchCheckType>(sub, TLongExact, idx_ok, slow);

  // --- idx_ok: index is an int, unbox it and check the typecode == 'd' ---
  env.block = idx_ok;
  env.cursor = idx_ok->end();
  Register* idx = env.emit<RefineType>(TLongExact, sub);
  Register* unboxed_idx;
  if (spikeArrIdx()) {
    // SPIKE(ARRIDX): a compact int unboxes inline; anything wider takes the
    // generic subscript (no deopt, no PyLong_AsSsize_t call).
    BasicBlock* idx_compact = cfg.AllocateBlock();
    env.new_blocks += 1;
    Register* is_compact = env.emit<IsCompactLong>(idx);
    env.emit<CondBranch>(is_compact, idx_compact, slow);
    env.block = idx_compact;
    env.cursor = idx_compact->end();
    unboxed_idx = env.emit<CompactLongUnbox>(idx);
  } else {
    // Match array indexing's IndexError for integers outside Py_ssize_t.
    unboxed_idx = env.emit<IndexUnbox>(idx);
    env.emit<IsNegativeAndErrOccurred>(unboxed_idx, frame);
  }
  Register* descr = env.emit<LoadField>(
      arr, "ob_descr", offsetof(StdlibArrayObject, ob_descr), TCPtr);
  Register* typecode = env.emit<LoadField>(
      descr, "typecode", offsetof(StdlibArrayDescr, typecode), TCInt8);
  Register* expected_tc = env.emit<LoadConst>(Type::fromCInt('d', TCInt8));
  Register* tc_match = env.emit<PrimitiveCompare>(
      PrimitiveCompareOp::kEqual, typecode, expected_tc);
  env.emit<CondBranch>(tc_match, tc_ok, slow);

  // --- tc_ok: bounds check, then inlined native load + box ---
  env.block = tc_ok;
  env.cursor = tc_ok->end();
  Register* adjusted_idx = nullptr;
  if (spikeArrIdx()) {
    adjusted_idx =
        tryEmitInlineBoundsCheck(env, dominating_fs, arr, unboxed_idx);
  }
  if (adjusted_idx == nullptr) {
    adjusted_idx = env.emit<CheckSequenceBounds>(arr, unboxed_idx, frame);
  }
  Register* ob_item = env.emit<LoadField>(
      arr, "ob_item", offsetof(StdlibArrayObject, ob_item), TCPtr);
  Register* raw_double = env.emit<LoadArrayItem>(
      ob_item, adjusted_idx, arr, static_cast<intptr_t>(0), TCDouble);
  Register* fast_result = env.emit<PrimitiveBox>(raw_double, TCDouble, frame);
  // The inline bounds check may have split tc_ok; the block that actually
  // branches to `done` is wherever the cursor ended up.
  BasicBlock* tc_ok_end = env.block;
  env.emit<Branch>(done);

  // --- slow: generic subscript (PyObject_GetItem) ---
  env.block = slow;
  env.cursor = slow->end();
  std::unordered_map<BasicBlock*, Register*> phi_srcs{
      {tc_ok_end, fast_result},
  };
  BasicBlock* generic = slow;
  // SPIKE(STRSPEC): a site the 3.11 specializer tried and gave up on names
  // exact str as readily as array.array (it has no str[int] form either), so
  // on that evidence the array miss tries str next: exact str container,
  // compact int index, inline bounds check, direct character fetch.
  // Anything else takes the generic subscript.  No deopt is involved; a miss
  // costs the type checks.  The weaker int-index-only evidence does not get
  // the extra arm: it would add blocks to every such site for nothing.
  static const bool spike_strspec = Ci_SpikeGetenv("SPIKE_STRSPEC") != nullptr;
  if (spike_strspec && spikeArrIdx() && instr->isSubscrAdaptiveStuck() &&
      container->type().couldBe(TUnicodeExact)) {
    BasicBlock* str_ok = cfg.AllocateBlock();
    BasicBlock* str_idx_ok = cfg.AllocateBlock();
    BasicBlock* str_compact = cfg.AllocateBlock();
    generic = cfg.AllocateBlock();
    env.new_blocks += 4;
    env.emit<CondBranchCheckType>(container, TUnicodeExact, str_ok, generic);

    env.block = str_ok;
    env.cursor = str_ok->end();
    Register* str = env.emit<RefineType>(TUnicodeExact, container);
    env.emit<CondBranchCheckType>(sub, TLongExact, str_idx_ok, generic);

    env.block = str_idx_ok;
    env.cursor = str_idx_ok->end();
    Register* str_idx = env.emit<RefineType>(TLongExact, sub);
    Register* str_idx_compact = env.emit<IsCompactLong>(str_idx);
    env.emit<CondBranch>(str_idx_compact, str_compact, generic);

    env.block = str_compact;
    env.cursor = str_compact->end();
    Register* str_unboxed = env.emit<CompactLongUnbox>(str_idx);
    Register* str_adjusted = nullptr;
    if (dominating_fs != nullptr) {
      str_adjusted =
          tryEmitInlineBoundsCheck(env, dominating_fs, str, str_unboxed);
    }
    if (str_adjusted == nullptr) {
      str_adjusted = env.emit<CheckSequenceBounds>(str, str_unboxed, frame);
    }
    Register* str_result = emitUnicodeCharFetch(env, str, str_adjusted, frame);
    phi_srcs.emplace(env.block, str_result);
    env.emit<Branch>(done);

    env.block = generic;
    env.cursor = generic->end();
  }

  // --- generic subscript (PyObject_GetItem) ---
  BinaryOp* slow_instr = env.emitInstr<BinaryOp>(
      BinaryOpKind::kSubscript,
      container,
      sub,
      frame,
      /* array_subscr_slow_path= */ true);
  phi_srcs.emplace(generic, slow_instr->output());
  env.emit<Branch>(done);

  // --- done: merge the fast and slow results ---
  env.block = done;
  env.cursor = done->begin();
  return env.emit<Phi>(phi_srcs);
}

Register* simplifySubscript(
    Env& env,
    Register* lhs,
    Register* rhs,
    const FrameState* frame_state,
    const BinaryOp* instr) {
  if (lhs->isA(TDictExact)) {
    return env.emit<DictSubscr>(lhs, rhs, *frame_state);
  }
#if PY_VERSION_HEX < 0x030C0000
  if (env.exc_edge) {
    // Inside a compiled try block the subscript keeps its generic form: the
    // inline list/tuple/str fast paths raise their IndexError through a
    // bounds-check deopt, which would skip the compiled handler.  The
    // generic form raises natively and the edge takes it.
    return nullptr;
  }
#endif
  if (Register* array_result = trySimplifyArraySubscr(env, instr)) {
    return array_result;
  }
  if (!rhs->isA(TLongExact)) {
    return nullptr;
  }
  Type lhs_type = lhs->type();
  Type rhs_type = rhs->type();
  if (lhs_type <= TTupleExact && lhs_type.hasObjectSpec() &&
      rhs_type.hasObjectSpec()) {
    int overflow;
    Py_ssize_t index =
        PyLong_AsLongAndOverflow(rhs_type.objectSpec(), &overflow);
    if (!overflow) {
      PyObject* lhs_obj = lhs_type.objectSpec();
      if (index >= 0 && index < PyTuple_GET_SIZE(lhs_obj)) {
        BorrowedRef<> item = PyTuple_GET_ITEM(lhs_obj, index);
        env.emit<UseType>(lhs, lhs_type);
        env.emit<UseType>(rhs, rhs_type);
        return env.emit<LoadConst>(
            Type::fromObject(env.func.env.addReference(item)));
      }
      // Fallthrough
    }
    // Fallthrough
  }
  // TODO(T255264263). Enable this again. See P2169673256.
  if (!kFreeThreadedBuild) {
    if (lhs->isA(TListExact) || lhs->isA(TTupleExact)) {
      Register* adjusted_idx =
          unboxAndCheckListOrTupleIndex(env, instr, lhs, rhs);
      if (adjusted_idx == nullptr) {
        return nullptr;
      }
      Py_ssize_t offset = offsetof(PyTupleObject, ob_item);
      Register* array = lhs;
      // Lists carry a nested array of ob_item whereas tuples are variable-sized
      // structs.
      if (lhs->isA(TListExact)) {
        array = env.emit<LoadField>(
            lhs, "ob_item", offsetof(PyListObject, ob_item), TCPtr);
        offset = 0;
      }
      return env.emit<LoadArrayItem>(array, adjusted_idx, lhs, offset, TObject);
    }
  }
  if (lhs_type <= TUnicodeExact && rhs_type <= TLongExact) { // Unicode subscr
    if (lhs_type.hasObjectSpec() && rhs_type.hasObjectSpec()) {
      // This isn't safe in the multi-threaded compilation on 3.12 because
      // we don't hold the GIL which is required for
      // PyUnicode_InternInPlace.
      RETURN_MULTITHREADED_COMPILE(nullptr);

      // Constant propagation
      Py_ssize_t idx = PyLong_AsSsize_t(rhs_type.objectSpec());
      if (idx == -1 && PyErr_Occurred()) {
        PyErr_Clear();
        return nullptr;
      }
      Py_ssize_t n = PyUnicode_GetLength(lhs_type.objectSpec());

      if (idx < -n || idx >= n) {
        return nullptr;
      }

      if (idx < 0) {
        idx += n;
      }

      ThreadedCompileSerialize guard;
      Py_UCS4 c = PyUnicode_ReadChar(lhs_type.objectSpec(), idx);
      PyObject* substr = PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, &c, 1);
      if (substr == nullptr) {
        return nullptr;
      }
      PyUnicode_InternInPlace(&substr);
      Ref<> result = Ref<>::steal(substr);

      // Use exact types since we're relying on the object specializations.
      env.emit<UseType>(lhs, lhs_type);
      env.emit<UseType>(rhs, rhs_type);
      return env.emit<LoadConst>(
          Type::fromObject(env.func.env.addReference(std::move(result))));
    } else {
      env.emit<UseType>(lhs, TUnicodeExact);
      env.emit<UseType>(rhs, TLongExact);
      Register* unboxed_idx = env.emit<IndexUnbox>(rhs);
      env.emit<IsNegativeAndErrOccurred>(unboxed_idx, *frame_state);
      // SPIKE(ARRIDX): str[int] takes the same inline bounds check as list
      // and tuple (the length sits in PyASCIIObject); the helper call stays
      // for the cases the inline form cannot express.
      Register* adjusted_idx = nullptr;
      if (spikeArrIdx()) {
        // The inline form's Guard replays the whole subscript, so it needs
        // the Snapshot taken before the operands were popped; the
        // instruction's own FrameState has them popped.  Without one the
        // helper form stays.
        const FrameState* dominating_fs = instr->getDominatingFrameState();
        if (dominating_fs != nullptr) {
          adjusted_idx =
              tryEmitInlineBoundsCheck(env, dominating_fs, lhs, unboxed_idx);
        }
      }
      if (adjusted_idx == nullptr) {
        adjusted_idx =
            env.emit<CheckSequenceBounds>(lhs, unboxed_idx, *frame_state);
      }
      return emitUnicodeCharFetch(env, lhs, adjusted_idx, *frame_state);
    }
  }
  return nullptr;
}

#if PY_VERSION_HEX < 0x030C0000
// SPIKE(INTBITSPEC): `& | ^ << >>` on exact ints of at most two digits
// (|v| < 2^60) lowered to register operations behind deopting guards.
//
// The 3.11 specialiser has no quickened family for these operators, so a
// site carries no static type evidence: an operand of unknown type is
// speculated exact int and the site is registered with the LONGSPEC per-site
// fuse (eight guard failures recompile the function with this site generic).
// Unlike the compact-long add/subtract forms no operand has to be compact:
// any int64 is exact under and/or/xor and arithmetic right shift, and a left
// shift is checked for overflow by shifting back.  Chained expressions fold
// through the result box -- an operand that is itself a PrimitiveBox is used
// unboxed, and the box, left with frame-state uses only, is rematerialised
// on deopt -- so `T1[(t >> 24) & 0xFF] ^ T2[...] ^ ...` runs on registers
// with one allocation at the end.  Cipher state words are commonly 32-bit,
// which is why two digits rather than one are unboxed here.
namespace {

bool isIntBitwiseOp311(BinaryOpKind op) {
  switch (op) {
    case BinaryOpKind::kAnd:
    case BinaryOpKind::kOr:
    case BinaryOpKind::kXor:
    case BinaryOpKind::kLShift:
    case BinaryOpKind::kRShift:
      return true;
    default:
      return false;
  }
}

std::optional<BinaryOpKind> intBitwiseOpOfInPlace311(InPlaceOpKind op) {
  switch (op) {
    case InPlaceOpKind::kAnd:
      return BinaryOpKind::kAnd;
    case InPlaceOpKind::kOr:
      return BinaryOpKind::kOr;
    case InPlaceOpKind::kXor:
      return BinaryOpKind::kXor;
    case InPlaceOpKind::kLShift:
      return BinaryOpKind::kLShift;
    case InPlaceOpKind::kRShift:
      return BinaryOpKind::kRShift;
    default:
      return std::nullopt;
  }
}

// The instruction defining `reg`, looking through the Assign chains the
// simplifier driver leaves behind for replaced instructions.
const Instr* intBitDefiningInstr311(Register* reg) {
  const Instr* def = reg->instr();
  while (def != nullptr && def->IsAssign()) {
    def = def->GetOperand(0)->instr();
  }
  return def;
}

// The int64 value of an exact-int constant operand, when it has one that
// fits.
std::optional<int64_t> intBitConstant311(Register* reg) {
  Type ty = reg->type();
  if (!ty.hasObjectSpec() || !PyLong_CheckExact(ty.objectSpec())) {
    return std::nullopt;
  }
  int overflow = 0;
  long long value = PyLong_AsLongLongAndOverflow(ty.objectSpec(), &overflow);
  if (overflow != 0) {
    return std::nullopt;
  }
  if (value == -1 && PyErr_Occurred()) {
    PyErr_Clear();
    return std::nullopt;
  }
  return static_cast<int64_t>(value);
}

// SPIKE(INT64ARITH): |v| < 2^bits for a CInt64 register, from its
// definition (64 = unknown).  A two-digit unbox yields 60, a compact unbox 30,
// a constant its own width; the int64 operations propagate bounds so that an
// add/subtract of bounded operands needs no overflow guard.
int int64Bits311(Env& env, Register* reg, int depth = 0) {
  auto it = env.int64_bits.find(reg);
  if (it != env.int64_bits.end()) {
    return it->second;
  }
  Type ty = reg->type();
  if (ty.hasIntSpec()) {
    int64_t v = static_cast<int64_t>(ty.intSpec());
    if (v == INT64_MIN) {
      return 64;
    }
    uint64_t mag = v < 0 ? static_cast<uint64_t>(-v) : static_cast<uint64_t>(v);
    int bits = 0;
    while (mag != 0) {
      ++bits;
      mag >>= 1;
    }
    return bits;
  }
  if (depth > 12) {
    return 64;
  }
  const Instr* def = intBitDefiningInstr311(reg);
  if (def == nullptr) {
    return 64;
  }
  if (def->IsCompactLongUnbox()) {
    return PyLong_SHIFT;
  }
  if (def->IsPrimitiveConvert()) {
    Type src = def->GetOperand(0)->type();
    if (src <= TCBool || src <= TCUInt8 || src <= TCInt8) {
      return 8;
    }
    if (src <= TCUInt16 || src <= TCInt16) {
      return 16;
    }
    if (src <= TCUInt32 || src <= TCInt32) {
      return 32;
    }
    return 64;
  }
  if (!def->IsIntBinaryOp()) {
    return 64;
  }
  auto* bin = static_cast<const IntBinaryOp*>(def);
  Register* l = bin->left();
  Register* r = bin->right();
  int lb = int64Bits311(env, l, depth + 1);
  int rb = int64Bits311(env, r, depth + 1);
  auto clamp = [](int b) { return b > 64 ? 64 : b; };
  auto non_negative_const = [](Register* x) {
    return x->type().hasIntSpec() &&
        static_cast<int64_t>(x->type().intSpec()) >= 0;
  };
  switch (bin->op()) {
    // The bitwise operations act on the two's-complement form: operands with
    // |v| < 2^n are (n+1)-bit signed values, and so is the result, which can
    // reach -2^n (-3 & -2 == -4, 3 ^ -1 == -4).  A non-negative mask bounds
    // the result by itself whatever the sign of the other operand
    // (-4 & INT64_MAX == INT64_MAX - 3); the other operand's width does not.
    case BinaryOpKind::kAnd:
      if (non_negative_const(r)) {
        return rb;
      }
      if (non_negative_const(l)) {
        return lb;
      }
      return clamp(std::max(lb, rb) + 1);
    case BinaryOpKind::kOr:
    case BinaryOpKind::kXor:
      return clamp(std::max(lb, rb) + 1);
    case BinaryOpKind::kAdd:
    case BinaryOpKind::kSubtract:
      return clamp(std::max(lb, rb) + 1);
    case BinaryOpKind::kMultiply:
      return clamp(lb + rb);
    case BinaryOpKind::kRShift:
      // An arithmetic shift rounds toward -inf, so a negative value can keep
      // one more bit than |v| >> k (-7 >> 1 == -4).
      if (non_negative_const(r)) {
        int k = static_cast<int>(std::min<int64_t>(r->type().intSpec(), 64));
        return std::min(lb, std::max(lb - k + 1, 1));
      }
      return lb;
    case BinaryOpKind::kLShift:
      if (non_negative_const(r)) {
        int k = static_cast<int>(std::min<int64_t>(r->type().intSpec(), 64));
        return clamp(lb + k);
      }
      return 64;
    default:
      return 64;
  }
}

// The unboxed int64 value of an operand.  A constant folds and a
// PrimitiveBox of a CInt64 is looked through; anything else is guarded exact
// int of at most two digits and unboxed without a branch:
//   size = ob_size;                       guard (size + 2) <u 5
//   two  = |size| == 2  (size * size >> 2)
//   hi   = ob_digit[two] & -two           (digit 1 is read only when it
//                                          exists; a one-digit int re-reads
//                                          digit 0 and masks it away)
//   mag  = (ob_digit[0] | hi << 30) & -(size != 0)
//   v    = (mag ^ (size >> 63)) - (size >> 63)
// `guarded` is set when a deopting guard was emitted.  With `fs` null the
// guards bind to the dominating Snapshot (the compare lowerings); an operand
// that would then need a GuardType is refused (nullptr).
// Branch-free unbox of an exact int of at most two digits (the guard deopts
// on more); `obj` is TLongExact.
Register* emitTwoDigitUnbox311(
    Env& env,
    Register* obj,
    const FrameState* fs,
    bool& guarded) {
  Register* size = env.emit<LoadField>(
      obj, "ob_size", offsetof(PyVarObject, ob_size), TCInt64);
  Register* two = env.emit<LoadConst>(Type::fromCInt(2, TCInt64));
  Register* biased = env.emit<IntBinaryOp>(BinaryOpKind::kAdd, size, two);
  Register* five = env.emit<LoadConst>(Type::fromCInt(5, TCInt64));
  Register* small = env.emit<PrimitiveCompare>(
      PrimitiveCompareOp::kLessThanUnsigned, biased, five);
  auto guard = env.emitInstr<Guard>(small);
  if (fs != nullptr) {
    guard->setFrameState(*fs);
  }
  guarded = true;
  // size in [-2, 2]: size*size is 4 for two digits, 1 for one, 0 for zero.
  Register* sq = env.emit<IntBinaryOp>(BinaryOpKind::kMultiply, size, size);
  Register* hi_idx = env.emit<IntBinaryOp>(BinaryOpKind::kRShift, sq, two);
  Register* three = env.emit<LoadConst>(Type::fromCInt(3, TCInt64));
  Register* sq3 = env.emit<IntBinaryOp>(BinaryOpKind::kAdd, sq, three);
  Register* nonzero = env.emit<IntBinaryOp>(BinaryOpKind::kRShift, sq3, two);
  Register* zero = env.emit<LoadConst>(Type::fromCInt(0, TCInt64));
  Register* nonzero_mask =
      env.emit<IntBinaryOp>(BinaryOpKind::kSubtract, zero, nonzero);
  Register* digits_off = env.emit<LoadConst>(
      Type::fromCInt(offsetof(PyLongObject, ob_digit), TCInt64));
  Register* digits = env.emit<LoadFieldAddress>(obj, digits_off);
  Register* d0 = env.emit<LoadArrayItem>(
      digits, zero, obj, static_cast<intptr_t>(0), TCUInt32);
  Register* d1 = env.emit<LoadArrayItem>(
      digits, hi_idx, obj, static_cast<intptr_t>(0), TCUInt32);
  Register* d0w = env.emit<PrimitiveConvert>(d0, TCInt64);
  Register* d1w = env.emit<PrimitiveConvert>(d1, TCInt64);
  Register* hi_mask =
      env.emit<IntBinaryOp>(BinaryOpKind::kSubtract, zero, hi_idx);
  d1w = env.emit<IntBinaryOp>(BinaryOpKind::kAnd, d1w, hi_mask);
  Register* shift = env.emit<LoadConst>(Type::fromCInt(PyLong_SHIFT, TCInt64));
  Register* hi = env.emit<IntBinaryOp>(BinaryOpKind::kLShift, d1w, shift);
  Register* mag = env.emit<IntBinaryOp>(BinaryOpKind::kOr, d0w, hi);
  mag = env.emit<IntBinaryOp>(BinaryOpKind::kAnd, mag, nonzero_mask);
  Register* sixty_three = env.emit<LoadConst>(Type::fromCInt(63, TCInt64));
  Register* sign =
      env.emit<IntBinaryOp>(BinaryOpKind::kRShift, size, sixty_three);
  Register* flipped = env.emit<IntBinaryOp>(BinaryOpKind::kXor, mag, sign);
  Register* value =
      env.emit<IntBinaryOp>(BinaryOpKind::kSubtract, flipped, sign);
  env.int64_bits[value] = 2 * PyLong_SHIFT;
  return value;
}

// A constant folds to a CInt64 and a PrimitiveBox of a CInt64 is looked
// through (the box is then left with frame-state uses and rematerialised on
// deopt); nullptr for any other operand.
Register* int64Direct311(Env& env, Register* reg) {
  if (auto value = intBitConstant311(reg)) {
    env.emit<UseType>(reg, reg->type());
    return env.emit<LoadConst>(Type::fromCInt(*value, TCInt64));
  }
  const Instr* def = intBitDefiningInstr311(reg);
  if (def != nullptr && def->IsPrimitiveBox()) {
    auto* box = static_cast<const PrimitiveBox*>(def);
    if (box->type() <= TCInt64) {
      return box->value();
    }
  }
  return nullptr;
}

// The unboxed int64 value of an operand.  A constant folds and a CInt64 box
// is looked through.  A boxed object is guarded exact int and unboxed: `wide`
// takes the branch-free two-digit sequence (the bitwise sites, whose values
// are 32-bit words); otherwise the compact form (SPIKE(INT64ARITH): the
// add/subtract/compare sites are dominated by small ints, where a cold
// two-digit arm only costs code size -- the int64 semantics are for the
// values already in registers).  `guarded` is set
// when a deopting guard was emitted; with `fs` null the guards bind to the
// dominating Snapshot and an operand needing a GuardType is refused.
Register* int64Operand311(
    Env& env,
    Register* reg,
    const FrameState* fs,
    bool& guarded,
    bool wide) {
  if (Register* direct = int64Direct311(env, reg)) {
    return direct;
  }
  Register* obj = reg;
  if (!obj->isA(TLongExact)) {
    if (fs == nullptr) {
      return nullptr;
    }
    obj = env.emit<GuardType>(TLongExact, obj, *fs);
    guarded = true;
  }
  // The raw field loads accept any object, so the exact-int requirement has
  // to be pinned here or GuardTypeRemoval drops the guard as unused.
  env.emit<UseType>(obj, TLongExact);
  if (wide) {
    return emitTwoDigitUnbox311(env, obj, fs, guarded);
  }
  Register* compact = env.emit<IsCompactLong>(obj);
  auto guard = env.emitInstr<Guard>(compact);
  if (fs != nullptr) {
    guard->setFrameState(*fs);
  }
  guarded = true;
  return env.emit<CompactLongUnbox>(obj);
}

// SPIKE(INT64ARITH) applies only where the compact-long form would have
// deopted: a CInt64 box whose bound exceeds 61 bits (a shift or a two-digit
// unbox feeding the operation) or a constant outside the compact range.  A
// site whose register values are provably narrow keeps the compact form
// untouched, instruction for instruction: its HIR, register allocation and
// frame layout stay what they were.
bool int64WideOperand311(Env& env, Register* reg) {
  if (auto value = intBitConstant311(reg)) {
    return *value >= (int64_t{1} << PyLong_SHIFT) ||
        *value <= -(int64_t{1} << PyLong_SHIFT);
  }
  const Instr* def = intBitDefiningInstr311(reg);
  if (def != nullptr && def->IsPrimitiveBox()) {
    auto* box = static_cast<const PrimitiveBox*>(def);
    if (box->type() <= TCInt64) {
      return int64Bits311(env, box->value()) > 61;
    }
  }
  return false;
}

// Both operands of an add/subtract/compare on exact ints: constants and
// CInt64 boxes are taken directly, the boxed objects share one compactness
// guard (the shape of the compact-long form this extends).  False when an
// operand is not an exact int.
bool int64OperandPair311(
    Env& env,
    Register* lhs,
    Register* rhs,
    const FrameState* fs,
    bool& guarded,
    Register*& a,
    Register*& b) {
  a = int64Direct311(env, lhs);
  b = lhs == rhs ? a : int64Direct311(env, rhs);
  Register* objs[2] = {nullptr, nullptr};
  int n = 0;
  if (a == nullptr) {
    if (!lhs->isA(TLongExact)) {
      return false;
    }
    objs[n++] = lhs;
  }
  if (b == nullptr && lhs != rhs) {
    if (!rhs->isA(TLongExact)) {
      return false;
    }
    objs[n++] = rhs;
  }
  for (int i = 0; i < n; ++i) {
    env.emit<UseType>(objs[i], TLongExact);
  }
  if (n > 0) {
    Register* cond = env.emit<IsCompactLong>(objs[0]);
    if (n == 2) {
      Register* second = env.emit<IsCompactLong>(objs[1]);
      cond = env.emit<IntBinaryOp>(BinaryOpKind::kAnd, cond, second);
    }
    auto guard = env.emitInstr<Guard>(cond);
    if (fs != nullptr) {
      guard->setFrameState(*fs);
    }
    guarded = true;
    if (a == nullptr) {
      a = env.emit<CompactLongUnbox>(lhs);
    }
    if (b == nullptr) {
      b = lhs == rhs ? a : env.emit<CompactLongUnbox>(rhs);
    }
  }
  return true;
}

Register*
intBitOperand311(Env& env, Register* reg, const FrameState& fs, bool& guarded) {
  return int64Operand311(env, reg, &fs, guarded, true);
}

// SPIKE(INT64ARITH): `lhs +/- rhs` on exact ints in int64 registers.  The
// operands are unboxed as above (constants fold, CInt64 boxes are looked
// through, objects of at most two digits are unboxed), so a two-digit sum
// cannot overflow; an operand of unknown magnitude (a shift result, a
// loop-carried value) gets a signed-overflow guard instead: for a + b the
// sum overflowed iff (a ^ s) & (b ^ s) < 0, for a - b iff (a ^ b) & (a ^ d) <
// 0. The compact-long form this replaces deopted on every two-digit operand
// ((1 << n) - 1, (x << 24) + y, a 32-bit accumulator), which then took the
// site to the generic helper through the fuse.
//
// `guard_fs` is the dominating frame state (the builder's Snapshot before the
// operation, operands still on the stack): a deopt from a guard re-executes
// the operation.  An operation's own frame state is the post-pop exception
// state and is only right for the box (`box_fs`); the guards sit in the
// operation's own block, where bindGuards rebinds them to that Snapshot.
Register* emitInt64AddSub311(
    Env& env,
    BinaryOpKind op,
    Register* lhs,
    Register* rhs,
    const FrameState& guard_fs,
    const FrameState& box_fs,
    PyCodeObject* code,
    int64_t site) {
  if (!int64WideOperand311(env, lhs) && !int64WideOperand311(env, rhs)) {
    return nullptr;
  }
  bool guarded = false;
  Register* a = nullptr;
  Register* b = nullptr;
  if (!int64OperandPair311(env, lhs, rhs, &guard_fs, guarded, a, b)) {
    return nullptr;
  }
  int bits = std::max(int64Bits311(env, a), int64Bits311(env, b));
  Register* result = env.emit<IntBinaryOp>(op, a, b);
  if (bits >= 63) {
    Register* x1 = env.emit<IntBinaryOp>(BinaryOpKind::kXor, a, result);
    Register* x2 = op == BinaryOpKind::kAdd
        ? env.emit<IntBinaryOp>(BinaryOpKind::kXor, b, result)
        : env.emit<IntBinaryOp>(BinaryOpKind::kXor, a, b);
    Register* m = env.emit<IntBinaryOp>(BinaryOpKind::kAnd, x1, x2);
    Register* zero = env.emit<LoadConst>(Type::fromCInt(0, TCInt64));
    Register* ok = env.emit<PrimitiveCompare>(
        PrimitiveCompareOp::kGreaterThanEqual, m, zero);
    auto guard = env.emitInstr<Guard>(ok);
    guard->setFrameState(guard_fs);
    guarded = true;
    env.int64_bits[result] = 64;
  } else {
    env.int64_bits[result] = bits + 1;
  }
  if (guarded) {
    Ci_SpikeLongSpecRegister311(code, site);
  }
  return env.emit<PrimitiveBox>(result, TCInt64, box_fs);
}

// True when `reg` is known to hold a small int: an int64 constant, or the
// unboxed result of an earlier int operation.
bool hasSmallIntEvidence311(Register* reg) {
  if (intBitConstant311(reg).has_value()) {
    return true;
  }
  const Instr* def = intBitDefiningInstr311(reg);
  return def != nullptr && def->IsPrimitiveBox() &&
      static_cast<const PrimitiveBox*>(def)->type() <= TCInt64;
}

// Lower `lhs op rhs` at bytecode `site` of `code`; nullptr leaves the generic
// operation in place.  At least one operand must be known small: two ints of
// unknown origin (`int(a) & int(b)`, `self._ip & mask`) may be arbitrarily
// wide, and a guard on them would deopt on every wide value until the fuse
// blows.
Register* emitIntBitwise311(
    Env& env,
    BinaryOpKind op,
    Register* lhs,
    Register* rhs,
    const FrameState& fs,
    PyCodeObject* code,
    int64_t site) {
  if (code == nullptr || Ci_SpikeNoLongSpec311(code, site)) {
    return nullptr;
  }
  if (!lhs->type().couldBe(TLongExact) || !rhs->type().couldBe(TLongExact)) {
    return nullptr;
  }
  std::optional<int64_t> lhs_const = intBitConstant311(lhs);
  std::optional<int64_t> rhs_const = intBitConstant311(rhs);
  // Two constants fold through LongBinaryOp; a known object that is not an
  // int64 constant (a three-digit mask) stays generic.
  if ((lhs_const.has_value() && rhs_const.has_value()) ||
      (lhs->type().hasObjectSpec() && !lhs_const.has_value()) ||
      (rhs->type().hasObjectSpec() && !rhs_const.has_value())) {
    return nullptr;
  }
  if (!hasSmallIntEvidence311(lhs) && !hasSmallIntEvidence311(rhs)) {
    return nullptr;
  }
  bool is_shift = op == BinaryOpKind::kLShift || op == BinaryOpKind::kRShift;
  if (is_shift && rhs_const.has_value() &&
      (*rhs_const < 0 || *rhs_const > 63)) {
    // A negative count raises; a larger one needs the arbitrary-precision
    // result.
    return nullptr;
  }
  bool guarded = false;
  Register* l = intBitOperand311(env, lhs, fs, guarded);
  Register* r = lhs == rhs ? l : intBitOperand311(env, rhs, fs, guarded);
  if (is_shift && !rhs_const.has_value()) {
    Register* limit = env.emit<LoadConst>(Type::fromCInt(64, TCInt64));
    Register* in_range = env.emit<PrimitiveCompare>(
        PrimitiveCompareOp::kLessThanUnsigned, r, limit);
    auto guard = env.emitInstr<Guard>(in_range);
    guard->setFrameState(fs);
    guarded = true;
  }
  Register* result = env.emit<IntBinaryOp>(op, l, r);
  if (op == BinaryOpKind::kLShift) {
    // Shifting back recovers the operand exactly when nothing overflowed.
    Register* back = env.emit<IntBinaryOp>(BinaryOpKind::kRShift, result, r);
    Register* exact =
        env.emit<PrimitiveCompare>(PrimitiveCompareOp::kEqual, back, l);
    auto guard = env.emitInstr<Guard>(exact);
    guard->setFrameState(fs);
    guarded = true;
  }
  if (guarded) {
    Ci_SpikeLongSpecRegister311(code, site);
  }
  return env.emit<PrimitiveBox>(result, TCInt64, fs);
}

} // namespace
#endif

#if PY_VERSION_HEX < 0x030C0000
// SPIKE(MIXEDNUM): the float speculation diamond as a numeric dispatch.
// The float/float arm is the caller's (FloatBinaryOp, lowered on the next
// iteration).  A compact exact int paired with an exact float is converted
// to double in place -- a compact int is one 30-bit digit, so the conversion
// is exact and the result equals float_<op>'s CONVERT_TO_DOUBLE path -- and
// computed with the same DoubleBinaryOp lowering (true division keeps its
// divide-by-zero Guard).  Two compact exact ints take the int64 form of the
// LONGSPEC lowering behind a branch rather than its deopting Guard: two
// 30-bit digits cannot overflow a 64-bit add, subtract or multiply, so the
// arm carries no guard at all (int/int true division stays generic).
// Every other pair takes the caller's slow arm.  A partner that is statically
// an exact int (an int literal) needs one check on the other side; its
// IsCompactLong folds.  Vector arithmetic with int-seeded components mixes
// ints and floats at the same sites, which otherwise all take binary_op1.
template <typename FastFF, typename SlowFn, typename OnFast>
Register* emitMixedNumericDiamond311(
    Env& env,
    BinaryOpKind op,
    Register* lhs,
    Register* rhs,
    const FrameState& dominating_fs,
    const FrameState& fs,
    FastFF fast_ff,
    SlowFn slow,
    OnFast on_fast) {
  auto check_type = [&](Register* reg, Type ty) {
    return [&env, reg, ty](BasicBlock* t, BasicBlock* f) {
      env.emit<CondBranchCheckType>(reg, ty, t, f);
    };
  };
  auto slow_arm = [&]() -> Register* { return slow(lhs, rhs); };
  auto mixed =
      [&](Register* f_reg, Register* i_reg, bool float_left) -> Register* {
    on_fast();
    env.emit<Snapshot>(dominating_fs);
    Register* fd = env.emit<PrimitiveUnbox>(f_reg, TCDouble);
    Register* iv = env.emit<CompactLongUnbox>(i_reg);
    Register* id = env.emit<PrimitiveConvert>(iv, TCDouble);
    Register* l = float_left ? fd : id;
    Register* r = float_left ? id : fd;
    if (op == BinaryOpKind::kTrueDivide) {
      Register* zero = env.emit<LoadConst>(Type::fromCDouble(0.0));
      Register* is_nonzero =
          env.emit<PrimitiveCompare>(PrimitiveCompareOp::kNotEqual, r, zero);
      env.emitInstr<Guard>(is_nonzero);
    }
    Register* result = env.emit<DoubleBinaryOp>(op, l, r);
    return env.emit<PrimitiveBox>(result, TCDouble, fs);
  };
  // `i_reg` is an exact int: compact ones take the mixed arm.
  auto compact_arm =
      [&](Register* f_reg, Register* i_reg, bool float_left) -> Register* {
    return env.emitCond(
        [&](BasicBlock* t, BasicBlock* f) {
          Register* compact = env.emit<IsCompactLong>(i_reg);
          env.emit<CondBranch>(compact, t, f);
        },
        [&] { return mixed(f_reg, i_reg, float_left); },
        slow_arm);
  };
  // `other` is not an exact float: an exact int takes the compact arm.
  auto int_arm =
      [&](Register* f_reg, Register* other, bool float_left) -> Register* {
    if (other->isA(TLongExact)) {
      return compact_arm(f_reg, other, float_left);
    }
    return env.emitCond(
        check_type(other, TLongExact),
        [&] {
          Register* ri = env.emit<RefineType>(TLongExact, other);
          return compact_arm(f_reg, ri, float_left);
        },
        slow_arm);
  };
  // Both exact ints: the compact pair computes in int64.
  auto fast_ii = [&](Register* li, Register* ri) -> Register* {
    if (op == BinaryOpKind::kTrueDivide) {
      return slow_arm();
    }
    return env.emitCond(
        [&](BasicBlock* t, BasicBlock* f) {
          Register* l_compact = env.emit<IsCompactLong>(li);
          Register* r_compact = env.emit<IsCompactLong>(ri);
          Register* both =
              env.emit<IntBinaryOp>(BinaryOpKind::kAnd, l_compact, r_compact);
          env.emit<CondBranch>(both, t, f);
        },
        [&] {
          Register* lv = env.emit<CompactLongUnbox>(li);
          Register* rv = env.emit<CompactLongUnbox>(ri);
          Register* result = env.emit<IntBinaryOp>(op, lv, rv);
          return env.emit<PrimitiveBox>(result, TCInt64, fs);
        },
        slow_arm);
  };
  // One side is an exact int and the other is not an exact float: exact
  // int on the other side too takes the int/int arm.
  auto ii_arm = [&](Register* l, Register* r) -> Register* {
    Register* unknown = l->isA(TLongExact) ? r : l;
    if (unknown->isA(TLongExact)) {
      return fast_ii(l, r);
    }
    return env.emitCond(
        check_type(unknown, TLongExact),
        [&] {
          Register* ri = env.emit<RefineType>(TLongExact, unknown);
          return unknown == l ? fast_ii(ri, r) : fast_ii(l, ri);
        },
        slow_arm);
  };
  bool l_known = lhs->isA(TFloatExact);
  bool r_known = rhs->isA(TFloatExact);
  bool l_int = lhs->isA(TLongExact);
  bool r_int = rhs->isA(TLongExact);
  if (l_known) {
    if (r_int) {
      return compact_arm(lhs, rhs, true);
    }
    return env.emitCond(
        check_type(rhs, TFloatExact),
        [&] {
          Register* r = env.emit<RefineType>(TFloatExact, rhs);
          return fast_ff(lhs, r);
        },
        [&] { return int_arm(lhs, rhs, true); });
  }
  if (r_known) {
    if (l_int) {
      return compact_arm(rhs, lhs, false);
    }
    return env.emitCond(
        check_type(lhs, TFloatExact),
        [&] {
          Register* l = env.emit<RefineType>(TFloatExact, lhs);
          return fast_ff(l, rhs);
        },
        [&] { return int_arm(rhs, lhs, false); });
  }
  if (l_int) {
    return env.emitCond(
        check_type(rhs, TFloatExact),
        [&] {
          Register* r = env.emit<RefineType>(TFloatExact, rhs);
          return compact_arm(r, lhs, false);
        },
        [&] { return ii_arm(lhs, rhs); });
  }
  if (r_int) {
    return env.emitCond(
        check_type(lhs, TFloatExact),
        [&] {
          Register* l = env.emit<RefineType>(TFloatExact, lhs);
          return compact_arm(l, rhs, true);
        },
        [&] { return ii_arm(lhs, rhs); });
  }
  return env.emitCond(
      check_type(lhs, TFloatExact),
      [&] {
        Register* l = env.emit<RefineType>(TFloatExact, lhs);
        return env.emitCond(
            check_type(rhs, TFloatExact),
            [&] {
              Register* r = env.emit<RefineType>(TFloatExact, rhs);
              return fast_ff(l, r);
            },
            [&] { return int_arm(l, rhs, true); });
      },
      [&] {
        return env.emitCond(
            check_type(lhs, TLongExact),
            [&] {
              Register* li = env.emit<RefineType>(TLongExact, lhs);
              return env.emitCond(
                  check_type(rhs, TFloatExact),
                  [&] {
                    Register* r = env.emit<RefineType>(TFloatExact, rhs);
                    return compact_arm(r, li, false);
                  },
                  [&] { return ii_arm(li, rhs); });
            },
            slow_arm);
      });
}

bool spikeMixedNum311() {
  static const bool on = Ci_SpikeGetenv("SPIKE_MIXEDNUM") != nullptr;
  return on;
}
#endif

Register* simplifyBinaryOp(Env& env, const BinaryOp* instr) {
  BinaryOpKind op = instr->op();
  Register* lhs = instr->left();
  Register* rhs = instr->right();

#if PY_VERSION_HEX < 0x030C0000
  // SPIKE(INTBITSPEC): see emitIntBitwise311.
  static const bool spike_intbitspec =
      Ci_SpikeGetenv("SPIKE_INTBITSPEC") != nullptr;
  if (spike_intbitspec && isIntBitwiseOp311(op) && !instr->noSpeculation()) {
    if (Register* result = emitIntBitwise311(
            env,
            op,
            lhs,
            rhs,
            *instr->frameState(),
            env.func.code.get(),
            instr->bytecodeOffset().value())) {
      return result;
    }
  }
#endif

  if (op == BinaryOpKind::kSubscript) {
    return simplifySubscript(env, lhs, rhs, instr->frameState(), instr);
  }

  if (lhs->isA(TLongExact) && rhs->isA(TLongExact)) {
    // All binary ops on TLong's return mutable so can be freely simplified with
    // no explicit check.
    if (op == BinaryOpKind::kMatrixMultiply || op == BinaryOpKind::kSubscript) {
      // These will generate an error at runtime.
      return nullptr;
    }
    env.emit<UseType>(lhs, TLongExact);
    env.emit<UseType>(rhs, TLongExact);
    return env.emit<LongBinaryOp>(op, lhs, rhs, *instr->frameState());
  }

  if (lhs->isA(TFloatExact) && rhs->isA(TFloatExact) &&
      ((instr->op() == BinaryOpKind::kPower) ||
       FloatBinaryOp::slotMethod(instr->op()))) {
    env.emit<UseType>(lhs, TFloatExact);
    env.emit<UseType>(rhs, TFloatExact);
    return env.emit<FloatBinaryOp>(instr->op(), lhs, rhs, *instr->frameState());
  }

  // Mixed float/int binary ops where the int is a known constant: convert the
  // int to a double at compile time and emit an unboxed DoubleBinaryOp.  This
  // avoids going through CPython's generic binary op dispatch which would do
  // the int-to-float conversion at runtime.
  if ((op == BinaryOpKind::kAdd || op == BinaryOpKind::kSubtract ||
       op == BinaryOpKind::kMultiply || op == BinaryOpKind::kTrueDivide ||
       op == BinaryOpKind::kPower)) {
    Register* float_reg = nullptr;
    Register* int_reg = nullptr;
    bool int_on_right = false;

    if (lhs->isA(TFloatExact) && rhs->isA(TLongExact) &&
        rhs->type().hasObjectSpec()) {
      float_reg = lhs;
      int_reg = rhs;
      int_on_right = true;
    } else if (
        lhs->isA(TLongExact) && rhs->isA(TFloatExact) &&
        lhs->type().hasObjectSpec()) {
      float_reg = rhs;
      int_reg = lhs;
      int_on_right = false;
    }

    if (float_reg != nullptr) {
      int overflow;
      long long_val =
          PyLong_AsLongAndOverflow(int_reg->type().objectSpec(), &overflow);
      if (!overflow) {
        auto double_val = static_cast<double>(long_val);
        env.emit<UseType>(float_reg, TFloatExact);
        env.emit<UseType>(int_reg, int_reg->type());
        Register* unbox_float = env.emit<PrimitiveUnbox>(float_reg, TCDouble);
        if (op == BinaryOpKind::kPower && long_val == 2) {
          Register* result = env.emit<DoubleBinaryOp>(
              BinaryOpKind::kMultiply, unbox_float, unbox_float);
          Register* max_double =
              env.emit<LoadConst>(Type::fromCDouble(DBL_MAX));
          Register* is_finite = env.emit<PrimitiveCompare>(
              PrimitiveCompareOp::kLessThan, result, max_double);
          env.emitInstr<Guard>(is_finite);
          return env.emit<PrimitiveBox>(result, TCDouble, *instr->frameState());
        }
        Register* const_double =
            env.emit<LoadConst>(Type::fromCDouble(double_val));
        Register* unbox_left = int_on_right ? unbox_float : const_double;
        Register* unbox_right = int_on_right ? const_double : unbox_float;
        // Need to guard against division by zero.
        if (op == BinaryOpKind::kTrueDivide) {
          Register* zero = env.emit<LoadConst>(Type::fromCDouble(0.0));
          Register* is_nonzero = env.emit<PrimitiveCompare>(
              PrimitiveCompareOp::kNotEqual, unbox_right, zero);
          env.emitInstr<Guard>(is_nonzero);
        }
        Register* result =
            env.emit<DoubleBinaryOp>(op, unbox_left, unbox_right);
        return env.emit<PrimitiveBox>(result, TCDouble, *instr->frameState());
      }
    }
  }

  if (op == BinaryOpKind::kPower && rhs->isA(TLongExact) &&
      rhs->type().hasObjectSpec()) {
    int overflow;
    long long_val = PyLong_AsLongAndOverflow(
        rhs->type().objectSpec(), &overflow);
    if (!overflow && long_val == 2) {
      Register* guarded_left =
          env.emit<GuardType>(TFloatExact, lhs, *instr->frameState());
      env.emit<UseType>(guarded_left, TFloatExact);
      Register* unbox_left =
          env.emit<PrimitiveUnbox>(guarded_left, TCDouble);
      Register* result = env.emit<DoubleBinaryOp>(
          BinaryOpKind::kMultiply, unbox_left, unbox_left);
      Register* max_double =
          env.emit<LoadConst>(Type::fromCDouble(DBL_MAX));
      Register* is_finite = env.emit<PrimitiveCompare>(
          PrimitiveCompareOp::kLessThan, result, max_double);
      env.emitInstr<Guard>(is_finite);
      return env.emit<PrimitiveBox>(
          result, TCDouble, *instr->frameState());
    }
  }

  // Mixed float/int true divide where the int is NOT a known constant and is
  // on the left: use a helper that handles arbitrary-size ints without a
  // compact-long guard. This avoids repeated deopts for elapsed nanosecond
  // values in logging.LogRecord.__init__.
  if (op == BinaryOpKind::kTrueDivide && lhs->isA(TLongExact) &&
      rhs->isA(TFloatExact) && !lhs->type().hasObjectSpec()) {
    env.emit<UseType>(lhs, TLongExact);
    env.emit<UseType>(rhs, TFloatExact);
    Register* result = env.emitVariadic<CallStatic>(
        2,
        reinterpret_cast<void*>(JITRT_LongFloatTrueDivide),
        TFloatExact | TNullptr,
        lhs,
        rhs);
    return env.emit<CheckExc>(result, *instr->frameState());
  }

  // Mixed float/int binary ops where the int is NOT a known constant: guard
  // that the int is compact (fits in a single 30-bit digit, thus losslessly
  // convertible to double), unbox both to CDouble, and emit DoubleBinaryOp.
  // For true divide, int / float is handled above with a helper because values
  // such as elapsed nanoseconds in logging.LogRecord.__init__ commonly exceed
  // the compact-long range. Keep the compact-long fast path for float / int.
  if ((op == BinaryOpKind::kAdd || op == BinaryOpKind::kSubtract ||
       op == BinaryOpKind::kMultiply || op == BinaryOpKind::kTrueDivide ||
       op == BinaryOpKind::kPower)) {
    Register* float_reg = nullptr;
    Register* int_reg = nullptr;
    bool int_on_right = false;

    if (lhs->isA(TFloatExact) && rhs->isA(TLongExact) &&
        !rhs->type().hasObjectSpec()) {
      float_reg = lhs;
      int_reg = rhs;
      int_on_right = true;
    } else if (
        lhs->isA(TLongExact) && rhs->isA(TFloatExact) &&
        !lhs->type().hasObjectSpec()) {
      float_reg = rhs;
      int_reg = lhs;
      int_on_right = false;
    }

    if (float_reg != nullptr) {
      env.emit<UseType>(float_reg, TFloatExact);
      env.emit<UseType>(int_reg, TLongExact);
      // Guard that the long is compact (at most one digit, fits in double).
      Register* is_compact = env.emit<IsCompactLong>(int_reg);
      env.emitInstr<Guard>(is_compact);
      // Unbox the compact long to CInt64 and convert to CDouble.
      Register* unbox_int = env.emit<CompactLongUnbox>(int_reg);
      Register* int_as_double = env.emit<PrimitiveConvert>(unbox_int, TCDouble);
      // Unbox the float to CDouble.
      Register* unbox_float = env.emit<PrimitiveUnbox>(float_reg, TCDouble);
      Register* unbox_left = int_on_right ? unbox_float : int_as_double;
      Register* unbox_right = int_on_right ? int_as_double : unbox_float;
      // Need to guard against division by zero.
      if (op == BinaryOpKind::kTrueDivide) {
        Register* zero = env.emit<LoadConst>(Type::fromCDouble(0.0));
        Register* is_nonzero = env.emit<PrimitiveCompare>(
            PrimitiveCompareOp::kNotEqual, unbox_right, zero);
        env.emitInstr<Guard>(is_nonzero);
      }
      Register* result = env.emit<DoubleBinaryOp>(op, unbox_left, unbox_right);
      return env.emit<PrimitiveBox>(result, TCDouble, *instr->frameState());
    }
  }

  if ((lhs->isA(TUnicodeExact) && rhs->isA(TLongExact)) &&
      (op == BinaryOpKind::kMultiply)) {
    Register* unboxed_rhs = env.emit<IndexUnbox>(rhs, PyExc_OverflowError);
    env.emit<IsNegativeAndErrOccurred>(unboxed_rhs, *instr->frameState());
    return env.emit<UnicodeRepeat>(lhs, unboxed_rhs, *instr->frameState());
  }

  if ((lhs->isA(TUnicodeExact) && rhs->isA(TUnicodeExact)) &&
      (op == BinaryOpKind::kAdd)) {
    return env.emit<UnicodeConcat>(lhs, rhs, *instr->frameState());
  }

  // Unsupported case.
#if PY_VERSION_HEX < 0x030C0000
  // SPIKE(FLOATSPEC): speculate float/float behind a type-check diamond --
  // one operand already exact float (single check), or a stuck adaptive
  // site with both operands unknown (double check).  The fast arm is the
  // FloatBinaryOp lowering, the slow arm the generic BinaryOp; a known
  // operand that is a PrimitiveBox read only by this site is re-boxed inside
  // the slow arm so the main-path box can fold away (same discipline as
  // IPTYPED; see reboxableForSlowArm311).
  static const bool spike_floatspec =
      Ci_SpikeGetenv("SPIKE_FLOATSPEC") != nullptr;
  // Not under an exception edge: the diamond's result is a Phi, which cannot
  // inherit the edge, so a raising generic arm would deopt every time
  // instead of reaching the compiled handler (see simplifyInPlaceOp).
  if (spike_floatspec && !instr->noSpeculation() && !env.exc_edge &&
      (op == BinaryOpKind::kAdd || op == BinaryOpKind::kSubtract ||
       op == BinaryOpKind::kMultiply || op == BinaryOpKind::kTrueDivide)) {
    bool l_known = lhs->isA(TFloatExact);
    bool r_known = rhs->isA(TFloatExact);
    bool eligible = false;
    if (l_known != r_known) {
      eligible =
          lhs->type().couldBe(TFloatExact) && rhs->type().couldBe(TFloatExact);
    } else if (!l_known && !r_known) {
      // SPIKE_FLOATSPEC=all speculates on every unknown/unknown site: the
      // adaptive counter is still zero when a function compiles at its 4th
      // call (quickened at the same call, the adaptive instruction has not
      // executed yet), so stuck-site evidence is mostly unavailable.
      // The builder decides per site (stuck evidence, or `all` mode minus
      // the sites the specialiser proved int / str).
      eligible = instr->speculateFloat() && lhs->type().couldBe(TFloatExact) &&
          rhs->type().couldBe(TFloatExact);
      // SPIKE(MIXEDNUM): a statically exact-int partner (an int literal)
      // with an unknown other side: the other side is checked for exact
      // float, int/int is re-emitted with both types pinned.
      if (!eligible && spikeMixedNum311() && instr->speculateFloat()) {
        eligible = (lhs->isA(TLongExact) && rhs->type().couldBe(TFloatExact)) ||
            (rhs->isA(TLongExact) && lhs->type().couldBe(TFloatExact));
      }
    }
    if (eligible) {
      const FrameState* dominating_fs = instr->getDominatingFrameState();
      if (dominating_fs != nullptr &&
          !((l_known && stillLowering311(lhs, TFloatExact)) ||
            (r_known && stillLowering311(rhs, TFloatExact)) ||
            (lhs->isA(TLongExact) && stillLowering311(lhs, TFloatExact)) ||
            (rhs->isA(TLongExact) && stillLowering311(rhs, TFloatExact)))) {
        const FrameState fs = *instr->frameState();
        const PrimitiveBox* l_rebox = reboxableForSlowArm311(env, lhs, instr);
        const PrimitiveBox* r_rebox = reboxableForSlowArm311(env, rhs, instr);
        auto fast_ff = [&](Register* l, Register* r) -> Register* {
          // Side effects first: bindGuards treats a non-replayable
          // instruction as ending the Snapshot's reach.  The Snapshot is
          // what a true division's zero Guard binds to.
          if (!l_known && !r_known && spikeFloatSpecFeedback311()) {
            emitFloatSpecHit311(env, instr);
          }
          env.emit<Snapshot>(*dominating_fs);
          return env.emit<FloatBinaryOp>(op, l, r, fs);
        };
        auto slow_fn = [&](Register*, Register*) -> Register* {
          // Always the ORIGINAL operands: a refined operand would let
          // the guard-based mixed float/int paths fire on the generic
          // arm (logging's `ct - int(ct)`: non-compact int -> a deopt
          // per call, 2.3x slower).
          if (!l_known && !r_known && spikeFloatSpecFeedback311()) {
            emitFloatSpecMiss311(env, instr);
          }
          env.emit<Snapshot>(*dominating_fs);
          Register* l_slow = slowArmOperand311(env, lhs, l_rebox, fs);
          Register* r_slow = slowArmOperand311(env, rhs, r_rebox, fs);
          auto* slow_op = env.emitInstr<BinaryOp>(
              op,
              l_slow,
              r_slow,
              slowArmFrameState311(fs, l_rebox, l_slow, r_rebox, r_slow));
          slow_op->setNoSpeculation(true);
          return slow_op->output();
        };
        if (spikeMixedNum311()) {
          auto on_fast = [&]() {
            if (!l_known && !r_known && spikeFloatSpecFeedback311()) {
              emitFloatSpecHit311(env, instr);
            }
          };
          return emitMixedNumericDiamond311(
              env, op, lhs, rhs, *dominating_fs, fs, fast_ff, slow_fn, on_fast);
        }
        return emitSpecializedCompareDiamond(
            env, TFloatExact, lhs, rhs, fs, fast_ff, slow_fn);
      }
    }
  }
#endif
  return nullptr;
}

// Keep failed float predictions inside the compiled function. In particular,
// an accumulator initialized with integer zero needs the generic operation on
// its first iteration, not a deopt of the whole loop.
Register* trySimplifyFloatInPlace(Env& env, const InPlaceOp* instr) {
#if PY_VERSION_HEX < 0x030C0000
  if (!instr->hasFloatFastPath() || !getConfig().specialized_opcodes ||
      (instr->op() != InPlaceOpKind::kAdd &&
       instr->op() != InPlaceOpKind::kSubtract)) {
    return nullptr;
  }
  Register* left = instr->left();
  Register* right = instr->right();
  if (!left->type().couldBe(TFloatExact) ||
      !right->type().couldBe(TFloatExact)) {
    return nullptr;
  }
  const FrameState& frame = *instr->frameState();
  CFG& cfg = env.func.cfg;
  BasicBlock* check_right = cfg.AllocateBlock();
  BasicBlock* fast = cfg.AllocateBlock();
  BasicBlock* slow = cfg.AllocateBlock();
  env.new_blocks += 4; // Three paths plus the split-off continuation.

  env.emit<CondBranchCheckType>(left, TFloatExact, check_right, slow);
  BasicBlock* done = cfg.splitAfter(*std::prev(env.cursor));
  env.block = check_right;
  env.cursor = check_right->end();
  env.emit<CondBranchCheckType>(right, TFloatExact, fast, slow);

  env.block = fast;
  env.cursor = fast->end();
  Register* float_left = env.emit<RefineType>(TFloatExact, left);
  Register* float_right = env.emit<RefineType>(TFloatExact, right);
  BinaryOpKind op = instr->op() == InPlaceOpKind::kAdd
      ? BinaryOpKind::kAdd
      : BinaryOpKind::kSubtract;
  Register* fast_result =
      env.emit<FloatBinaryOp>(op, float_left, float_right, frame);
  env.emit<Branch>(done);

  env.block = slow;
  env.cursor = slow->end();
  Register* slow_result = env.emit<InPlaceOp>(instr->op(), left, right, frame);
  env.emit<Branch>(done);

  env.block = done;
  env.cursor = done->begin();
  return env.emit<Phi>(std::unordered_map<BasicBlock*, Register*>{
      {fast, fast_result}, {slow, slow_result}});
#else
  return nullptr;
#endif
}

Register* simplifyInPlaceOp(Env& env, const InPlaceOp* instr) {
#if PY_VERSION_HEX < 0x030C0000
  if (env.exc_edge) {
    // The generic helper leaves NULL for the compiled handler's branch.
    // Numeric fast paths guard their own errors (or merge through a Phi),
    // so they cannot inherit this exception edge on just the final result.
    return nullptr;
  }
#endif
  Register* lhs = instr->left();
  Register* rhs = instr->right();
#if PY_VERSION_HEX < 0x030C0000
  // SPIKE(INTBITSPEC): the in-place forms of `& | ^ << >>` have the binary
  // semantics for exact ints (no nb_inplace_* slot), and any other operand
  // type deopts to the interpreter's in-place operation.
  static const bool spike_intbitspec =
      Ci_SpikeGetenv("SPIKE_INTBITSPEC") != nullptr;
  if (spike_intbitspec && !instr->noSpeculation()) {
    if (auto bit_op = intBitwiseOpOfInPlace311(instr->op())) {
      if (Register* result = emitIntBitwise311(
              env,
              *bit_op,
              lhs,
              rhs,
              *instr->frameState(),
              env.func.code.get(),
              instr->bytecodeOffset().value())) {
        return result;
      }
    }
  }
#endif
  if (lhs->isA(TLongExact) && rhs->isA(TLongExact)) {
#if PY_VERSION_HEX < 0x030C0000
    // The compact-long fast path for in-place add/subtract is part of the
    // CPython 3.11 delivery only.  On 3.12+ the generic long helper path
    // below stays the sole lowering, exactly as before the 3.11 port.
    std::optional<BinaryOpKind> compact_op;
    switch (instr->op()) {
      case InPlaceOpKind::kAdd:
        compact_op = BinaryOpKind::kAdd;
        break;
      case InPlaceOpKind::kSubtract:
        compact_op = BinaryOpKind::kSubtract;
        break;
      default:
        break;
    }

    auto frame_state_size = instr->frameState()->localsplus.size() +
        instr->frameState()->stack.size();
    PyCodeObject* code = env.func.code.get();
    int64_t site = instr->bytecodeOffset().value();
    // A site the per-site fuse took down keeps the generic LongInPlaceOp
    // below, as simplifyLongBinaryOp does: the compactness guard would keep
    // failing on the operands that failed the int64 bounds.
    bool site_fused = code != nullptr && Ci_SpikeNoLongSpec311(code, site);
    if (!site_fused && compact_op.has_value() && frame_state_size <= 8) {
      // SPIKE(INT64ARITH): same int64 form as simplifyLongBinaryOp, with the
      // per-site fuse this in-place path did not have.
      static const bool spike_int64arith =
          Ci_SpikeGetenv("SPIKE_INT64ARITH") != nullptr;
      const FrameState* dominating_fs = instr->getDominatingFrameState();
      if (spike_int64arith && code != nullptr && dominating_fs != nullptr &&
          !(lhs->type().hasObjectSpec() && rhs->type().hasObjectSpec())) {
        if (Register* result = emitInt64AddSub311(
                env,
                *compact_op,
                lhs,
                rhs,
                *dominating_fs,
                *instr->frameState(),
                code,
                site)) {
          return result;
        }
      }
      // The compact form joins the same per-site fuse, as in
      // simplifyLongBinaryOp: operands of two or more digits fail its
      // guard every time.
      if (code != nullptr) {
        Ci_SpikeLongSpecRegister311(code, site);
      }
      env.emit<UseType>(lhs, TLongExact);
      env.emit<UseType>(rhs, TLongExact);
      Register* is_lhs_compact = env.emit<IsCompactLong>(lhs);
      Register* is_rhs_compact = env.emit<IsCompactLong>(rhs);
      Register* both_compact = env.emit<IntBinaryOp>(
          BinaryOpKind::kAnd, is_lhs_compact, is_rhs_compact);
      auto guard = env.emitInstr<Guard>(both_compact);
      guard->setFrameState(*instr->frameState());
      Register* unboxed_lhs = env.emit<CompactLongUnbox>(lhs);
      Register* unboxed_rhs = env.emit<CompactLongUnbox>(rhs);
      Register* result =
          env.emit<IntBinaryOp>(*compact_op, unboxed_lhs, unboxed_rhs);
      return env.emit<PrimitiveBox>(result, TCInt64, *instr->frameState());
    }
#endif

    // Remaining exact-int inplace ops still use the generic long helper path.
    switch (instr->op()) {
      case InPlaceOpKind::kAdd:
      case InPlaceOpKind::kAnd:
      case InPlaceOpKind::kFloorDivide:
      case InPlaceOpKind::kLShift:
      case InPlaceOpKind::kModulo:
      case InPlaceOpKind::kMultiply:
      case InPlaceOpKind::kOr:
      case InPlaceOpKind::kRShift:
      case InPlaceOpKind::kSubtract:
      case InPlaceOpKind::kXor:
      case InPlaceOpKind::kPower:
      case InPlaceOpKind::kTrueDivide:
        env.emit<UseType>(lhs, TLongExact);
        env.emit<UseType>(rhs, TLongExact);
        return env.emit<LongInPlaceOp>(
            instr->op(), lhs, rhs, *instr->frameState());
      case InPlaceOpKind::kMatrixMultiply:
        // These will generate an error at runtime.
        break;
    }
  }

  // Convert `x += y` into `x = x + y` because floats are immutable, there are
  // further simplification cases for FloatBinaryOp.
  if (lhs->isA(TFloatExact) && rhs->isA(TFloatExact)) {
    std::optional<BinaryOpKind> binop;
    switch (instr->op()) {
      case InPlaceOpKind::kAdd:
        binop = BinaryOpKind::kAdd;
        break;
      case InPlaceOpKind::kSubtract:
        binop = BinaryOpKind::kSubtract;
        break;
      case InPlaceOpKind::kMultiply:
        binop = BinaryOpKind::kMultiply;
        break;
      case InPlaceOpKind::kTrueDivide:
        binop = BinaryOpKind::kTrueDivide;
        break;
      case InPlaceOpKind::kFloorDivide:
        binop = BinaryOpKind::kFloorDivide;
        break;
      case InPlaceOpKind::kModulo:
        binop = BinaryOpKind::kModulo;
        break;
      case InPlaceOpKind::kPower:
        binop = BinaryOpKind::kPower;
        break;
      default:
        break;
    }
    if (binop) {
      env.emit<UseType>(lhs, TFloatExact);
      env.emit<UseType>(rhs, TFloatExact);
      return env.emit<FloatBinaryOp>(*binop, lhs, rhs, *instr->frameState());
    }
  }

#if PY_VERSION_HEX < 0x030C0000
  // SPIKE(IPTYPED): the builder tagged this site with the exact type the
  // 3.11 specialiser observed for both operands.  Pin the still-unknown
  // operand(s) with a type-check diamond: the fast arm carries the refined
  // types and lowers to the unboxed paths above, the slow arm is the generic
  // in-place op.  A known-exact operand that is itself a PrimitiveBox is
  // re-boxed inside the slow arm so the main-path box can die once the fast
  // arm's unbox(box(x)) folds -- otherwise every site would materialise a
  // float object it only needs on the rare generic path.
  Type exact = instr->specializedExact();
  // SPIKE(FLOATSPEC): speculate float/float when one operand is already an
  // exact float (a float operand makes a float partner overwhelmingly
  // likely), or when the site is a stuck adaptive one (mixed types seen).
  // Both cases go through the same type-check diamond as IPTYPED: no deopt,
  // the generic op stays as the slow arm.
  static const bool spike_floatspec =
      Ci_SpikeGetenv("SPIKE_FLOATSPEC") != nullptr;
  if (exact == TBottom && spike_floatspec && !instr->noSpeculation() &&
      (instr->op() == InPlaceOpKind::kAdd ||
       instr->op() == InPlaceOpKind::kSubtract ||
       instr->op() == InPlaceOpKind::kMultiply ||
       instr->op() == InPlaceOpKind::kTrueDivide)) {
    bool l_float = lhs->isA(TFloatExact);
    bool r_float = rhs->isA(TFloatExact);
    if ((l_float != r_float && lhs->type().couldBe(TFloatExact) &&
         rhs->type().couldBe(TFloatExact)) ||
        (!l_float && !r_float && instr->speculateFloat() &&
         lhs->type().couldBe(TFloatExact) &&
         rhs->type().couldBe(TFloatExact))) {
      exact = TFloatExact;
    }
    // SPIKE(MIXEDNUM): see simplifyBinaryOp -- an exact-int partner with a
    // float or unknown other side.
    if (exact == TBottom && spikeMixedNum311() &&
        ((lhs->isA(TLongExact) && rhs->type().couldBe(TFloatExact) &&
          (r_float || instr->speculateFloat())) ||
         (rhs->isA(TLongExact) && lhs->type().couldBe(TFloatExact) &&
          (l_float || instr->speculateFloat())))) {
      exact = TFloatExact;
    }
  }
  if (exact != TBottom) {
    std::optional<BinaryOpKind> binop;
    switch (instr->op()) {
      case InPlaceOpKind::kAdd:
        binop = BinaryOpKind::kAdd;
        break;
      case InPlaceOpKind::kSubtract:
        binop = BinaryOpKind::kSubtract;
        break;
      case InPlaceOpKind::kMultiply:
        binop = BinaryOpKind::kMultiply;
        break;
      case InPlaceOpKind::kTrueDivide:
        binop = exact == TFloatExact
            ? std::optional<BinaryOpKind>(BinaryOpKind::kTrueDivide)
            : std::nullopt;
        break;
      default:
        break;
    }
    bool lhs_known = lhs->isA(exact);
    bool rhs_known = rhs->isA(exact);
    // SPIKE(MIXEDNUM): an exact-int side of a float diamond is a partner,
    // not a disqualifier.
    auto side_ok = [&](Register* reg) {
      return reg->type().couldBe(exact) ||
          (exact == TFloatExact && spikeMixedNum311() && reg->isA(TLongExact));
    };
    if (!binop.has_value() || (lhs_known && rhs_known) || !side_ok(lhs) ||
        !side_ok(rhs)) {
      return trySimplifyFloatInPlace(env, instr);
    }
    // Defer while a known operand is still being lowered (stillLowering311).
    if ((lhs_known && stillLowering311(lhs, exact)) ||
        (rhs_known && stillLowering311(rhs, exact)) ||
        (lhs->isA(TLongExact) && stillLowering311(lhs, exact)) ||
        (rhs->isA(TLongExact) && stillLowering311(rhs, exact))) {
      return trySimplifyFloatInPlace(env, instr);
    }
    // The compact-long fast arm carries a deopting Guard.  Its FrameState
    // must be the one *before* the in-place op (operands still on the
    // stack) so the interpreter re-executes the instruction after a deopt;
    // the op's own FrameState has them popped.  Without a dominating
    // Snapshot the int diamond is skipped.
    const FrameState* dominating_fs = instr->getDominatingFrameState();
    if (dominating_fs == nullptr) {
      return trySimplifyFloatInPlace(env, instr);
    }
    const FrameState fs = *instr->frameState();
    const PrimitiveBox* l_rebox = reboxableForSlowArm311(env, lhs, instr);
    const PrimitiveBox* r_rebox = reboxableForSlowArm311(env, rhs, instr);
    const bool evidence_free = instr->speculateFloat() && !lhs_known &&
        !rhs_known && spikeFloatSpecFeedback311();
    auto emit_fast = [&](Register* l, Register* r) -> Register* {
      if (exact == TFloatExact) {
        // The FloatBinaryOp lowering of a true division carries a
        // divide-by-zero Guard, which binds to the nearest preceding
        // Snapshot in its block; give the new arm one.
        if (evidence_free) {
          emitFloatSpecHit311(env, instr);
        }
        env.emit<Snapshot>(*dominating_fs);
        return env.emit<FloatBinaryOp>(*binop, l, r, fs);
      }
      // Exact ints: re-emitted with both types pinned, the compact-long
      // lowering above picks it up on the next simplifier iteration.  Its
      // Guard is bound to the nearest preceding Snapshot in the block
      // (bindGuards), so the new arm needs one.
      env.emit<Snapshot>(*dominating_fs);
      return env.emit<InPlaceOp>(instr->op(), l, r, fs);
    };
    auto emit_slow = [&]() -> Register* {
      // Either arm may be lowered further on a later simplifier iteration
      // (e.g. to the compact-long path with its Guard), so every arm starts
      // with a Snapshot for bindGuards to find.  Original operands only (see
      // the BinaryOp diamond).
      if (evidence_free) {
        emitFloatSpecMiss311(env, instr);
      }
      env.emit<Snapshot>(*dominating_fs);
      Register* l_slow = slowArmOperand311(env, lhs, l_rebox, fs);
      Register* r_slow = slowArmOperand311(env, rhs, r_rebox, fs);
      auto* slow_op = env.emitInstr<InPlaceOp>(
          instr->op(),
          l_slow,
          r_slow,
          slowArmFrameState311(fs, l_rebox, l_slow, r_rebox, r_slow));
      slow_op->setNoSpeculation(true);
      return slow_op->output();
    };
    if (exact == TFloatExact && spikeMixedNum311()) {
      auto on_fast = [&]() {
        if (instr->speculateFloat() && !lhs_known && !rhs_known &&
            spikeFloatSpecFeedback311()) {
          emitFloatSpecHit311(env, instr);
        }
      };
      auto slow_fn = [&](Register*, Register*) -> Register* {
        return emit_slow();
      };
      return emitMixedNumericDiamond311(
          env,
          *binop,
          lhs,
          rhs,
          *dominating_fs,
          fs,
          emit_fast,
          slow_fn,
          on_fast);
    }
    if (lhs_known || rhs_known) {
      Register* unknown = lhs_known ? rhs : lhs;
      return env.emitCond(
          [&](BasicBlock* fast, BasicBlock* slow) {
            env.emit<CondBranchCheckType>(unknown, exact, fast, slow);
          },
          [&] {
            Register* refined = env.emit<RefineType>(exact, unknown);
            return lhs_known ? emit_fast(lhs, refined)
                             : emit_fast(refined, rhs);
          },
          emit_slow);
    }
    return env.emitCond(
        [&](BasicBlock* fast, BasicBlock* slow) {
          env.emit<CondBranchCheckType>(lhs, exact, fast, slow);
        },
        [&] {
          Register* l = env.emit<RefineType>(exact, lhs);
          return env.emitCond(
              [&](BasicBlock* fast, BasicBlock* slow) {
                env.emit<CondBranchCheckType>(rhs, exact, fast, slow);
              },
              [&] {
                Register* r = env.emit<RefineType>(exact, rhs);
                return emit_fast(l, r);
              },
              emit_slow);
        },
        emit_slow);
  }
#endif
  return trySimplifyFloatInPlace(env, instr);
}

Register* simplifyLongBinaryOp(Env& env, const LongBinaryOp* instr) {
#if PY_VERSION_HEX < 0x030C0000
  // SPIKE(LONGSPEC): compact-long add/subtract/multiply behind a compactness
  // guard, mirroring the in-place form in simplifyInPlaceOp.  Two compact
  // longs are single 30-bit digits, so their sum/difference (< 2^31) and
  // product (< 2^60) always fit in int64: no overflow check is needed, only
  // the compactness guard (non-compact operands deopt; the despec fuse
  // covers a site that keeps deopting).  The payoff is not the operation
  // itself but the boxes it removes: a result consumed by another compact
  // op, an array/list index or an int-float conversion folds through
  // IsCompactLong(box(n)) -> IsCompactLong(n) and CompactLongUnbox(box(n))
  // -> n, and a box left with only frame-state uses is removed by
  // PrimitiveBoxRemat (deopt re-boxes the CInt64 value).  Index arithmetic
  // otherwise allocates a PyLong per operation.
  static const bool spike_longspec =
      Ci_SpikeGetenv("SPIKE_LONGSPEC") != nullptr;
  if (spike_longspec) {
    std::optional<BinaryOpKind> compact_op;
    switch (instr->op()) {
      case BinaryOpKind::kAdd:
      case BinaryOpKind::kSubtract:
      case BinaryOpKind::kMultiply:
        compact_op = instr->op();
        break;
      default:
        break;
    }
    Register* lhs = instr->left();
    Register* rhs = instr->right();
    PyCodeObject* code = env.func.code.get();
    int64_t site = instr->bytecodeOffset().value();
    bool site_open = code != nullptr && !Ci_SpikeNoLongSpec311(code, site);
    if (site_open && compact_op.has_value() && lhs->isA(TLongExact) &&
        rhs->isA(TLongExact) &&
        !(lhs->type().hasObjectSpec() && rhs->type().hasObjectSpec())) {
      Ci_SpikeLongSpecRegister311(code, site);
      // SPIKE(INT64ARITH): add/subtract in int64 with bounds instead of the
      // compactness guard; multiply keeps the compact form (a two-digit
      // product does not fit).
      static const bool spike_int64arith =
          Ci_SpikeGetenv("SPIKE_INT64ARITH") != nullptr;
      const FrameState* dominating_fs = instr->getDominatingFrameState();
      if (spike_int64arith && *compact_op != BinaryOpKind::kMultiply &&
          dominating_fs != nullptr) {
        if (Register* result = emitInt64AddSub311(
                env,
                *compact_op,
                lhs,
                rhs,
                *dominating_fs,
                *instr->frameState(),
                code,
                site)) {
          return result;
        }
      }
      env.emit<UseType>(lhs, TLongExact);
      env.emit<UseType>(rhs, TLongExact);
      Register* is_lhs_compact = env.emit<IsCompactLong>(lhs);
      Register* is_rhs_compact = env.emit<IsCompactLong>(rhs);
      Register* both_compact = env.emit<IntBinaryOp>(
          BinaryOpKind::kAnd, is_lhs_compact, is_rhs_compact);
      auto guard = env.emitInstr<Guard>(both_compact);
      guard->setFrameState(*instr->frameState());
      Register* unboxed_lhs = env.emit<CompactLongUnbox>(lhs);
      Register* unboxed_rhs = env.emit<CompactLongUnbox>(rhs);
      Register* result =
          env.emit<IntBinaryOp>(*compact_op, unboxed_lhs, unboxed_rhs);
      return env.emit<PrimitiveBox>(result, TCInt64, *instr->frameState());
    }
    // Floor division / modulo by a constant positive power of two: an
    // arithmetic shift right / a mask give Python's floor semantics for
    // negative dividends too (asr rounds toward -inf; `n & (c - 1)` is the
    // non-negative residue).  Covers spectral_norm's `... // 2`.
    if (site_open &&
        (instr->op() == BinaryOpKind::kFloorDivide ||
         instr->op() == BinaryOpKind::kModulo) &&
        lhs->isA(TLongExact) && !lhs->type().hasObjectSpec() &&
        rhs->type().hasObjectSpec() &&
        PyLong_CheckExact(rhs->type().objectSpec())) {
      int overflow = 0;
      long long divisor =
          PyLong_AsLongLongAndOverflow(rhs->type().objectSpec(), &overflow);
      if (overflow != 0) {
        PyErr_Clear();
      }
      if (overflow == 0 && divisor > 0 && divisor <= (1LL << 30) &&
          (divisor & (divisor - 1)) == 0) {
        Ci_SpikeLongSpecRegister311(code, site);
        env.emit<UseType>(rhs, rhs->type());
        // SPIKE(INT64ARITH): a shift / mask is exact for any int64, so the
        // dividend only needs the two-digit unbox.
        static const bool spike_int64arith_div =
            Ci_SpikeGetenv("SPIKE_INT64ARITH") != nullptr;
        Register* unboxed = nullptr;
        if (spike_int64arith_div && int64WideOperand311(env, lhs)) {
          bool guarded = false;
          unboxed = int64Operand311(
              env, lhs, instr->getDominatingFrameState(), guarded, false);
        }
        if (unboxed == nullptr) {
          env.emit<UseType>(lhs, TLongExact);
          Register* is_compact = env.emit<IsCompactLong>(lhs);
          auto guard = env.emitInstr<Guard>(is_compact);
          guard->setFrameState(*instr->frameState());
          unboxed = env.emit<CompactLongUnbox>(lhs);
        }
        Register* result;
        if (instr->op() == BinaryOpKind::kFloorDivide) {
          int shift = 0;
          while ((1LL << shift) < divisor) {
            ++shift;
          }
          Register* k = env.emit<LoadConst>(Type::fromCInt(shift, TCInt64));
          result = env.emit<IntBinaryOp>(BinaryOpKind::kRShift, unboxed, k);
        } else {
          Register* mask =
              env.emit<LoadConst>(Type::fromCInt(divisor - 1, TCInt64));
          result = env.emit<IntBinaryOp>(BinaryOpKind::kAnd, unboxed, mask);
        }
        return env.emit<PrimitiveBox>(result, TCInt64, *instr->frameState());
      }
    }
  }
#endif
  // This isn't safe in the multi-threaded compilation on 3.12 because
  // we don't hold the GIL which is required for allocation.
  RETURN_MULTITHREADED_COMPILE(nullptr);

  Type left_type = instr->left()->type();
  Type right_type = instr->right()->type();
  if (left_type.hasObjectSpec() && right_type.hasObjectSpec()) {
    ThreadedCompileSerialize guard;
    Ref<> result;
    if (instr->op() == BinaryOpKind::kPower) {
      result = Ref<>::steal(PyLong_Type.tp_as_number->nb_power(
          left_type.objectSpec(), right_type.objectSpec(), Py_None));
    } else {
      binaryfunc helper = instr->slotMethod();
      result = Ref<>::steal(
          (*helper)(left_type.objectSpec(), right_type.objectSpec()));
    }
    if (result == nullptr) {
      PyErr_Clear();
      return nullptr;
    }
    env.emit<UseType>(instr->left(), left_type);
    env.emit<UseType>(instr->right(), right_type);
    return env.emit<LoadConst>(
        Type::fromObject(env.func.env.addReference(std::move(result))));
  }
  return nullptr;
}

Register* simplifyFloatBinaryOp(Env& env, const FloatBinaryOp* instr) {
  BinaryOpKind op = instr->op();

  // Transform add/sub/mul to unboxed double operations.  These never raise
  // exceptions for any double inputs, guards aren't needed.
  if (op == BinaryOpKind::kAdd || op == BinaryOpKind::kSubtract ||
      op == BinaryOpKind::kMultiply) {
    Register* unbox_left = env.emit<PrimitiveUnbox>(instr->left(), TCDouble);
    Register* unbox_right = env.emit<PrimitiveUnbox>(instr->right(), TCDouble);
    Register* result = env.emit<DoubleBinaryOp>(op, unbox_left, unbox_right);
    return env.emit<PrimitiveBox>(result, TCDouble, *instr->frameState());
  }

  // True-divide is similar to add/sub/mul, but needs to guard against division
  // by zero as that would raise a ZeroDivisionError in the interpreter.
  if (op == BinaryOpKind::kTrueDivide) {
#if PY_VERSION_HEX < 0x030C0000
    const FrameState* frame = instr->frameState();
    if (frame != nullptr && frame->parent == nullptr &&
        frame->code == env.func.code &&
        env.func.excEdgeCapable311(instr->bytecodeOffset())) {
      // A zero-divide guard does not count as an exception at BINARY_OP.
      // Keep the helper's exception exit so this site can earn its grant,
      // including FloatBinaryOp produced by an in-place specialization.
      return nullptr;
    }
#endif
    Register* unbox_left = env.emit<PrimitiveUnbox>(instr->left(), TCDouble);
    Register* unbox_right = env.emit<PrimitiveUnbox>(instr->right(), TCDouble);
    Register* zero = env.emit<LoadConst>(Type::fromCDouble(0.0));
    Register* is_nonzero = env.emit<PrimitiveCompare>(
        PrimitiveCompareOp::kNotEqual, unbox_right, zero);
    env.emitInstr<Guard>(is_nonzero);
    Register* result = env.emit<DoubleBinaryOp>(op, unbox_left, unbox_right);
    return env.emit<PrimitiveBox>(result, TCDouble, *instr->frameState());
  }

#if PY_VERSION_HEX >= 0x030E0000
  // Keep the modern numerical path unboxed, but compute the same libm pow
  // as CPython instead of a sqrt/multiply/reciprocal approximation. Guards
  // leave exceptional and complex-valued inputs to the original bytecode.
  if (op == BinaryOpKind::kPower && !instr->left()->type().hasObjectSpec()) {
    Type exponent_type = instr->right()->type();
    if (exponent_type.hasObjectSpec() &&
        PyFloat_CheckExact(exponent_type.objectSpec())) {
      double exponent = PyFloat_AS_DOUBLE(exponent_type.objectSpec());
      if (exponent == 0.5 || exponent == 1.0 || exponent == 1.5 ||
          exponent == 2.0 || exponent == 3.0 || exponent == -0.5 ||
          exponent == -1.0 || exponent == -1.5 || exponent == -2.0) {
        auto* base = env.emit<PrimitiveUnbox>(instr->left(), TCDouble);
        auto* zero = env.emit<LoadConst>(Type::fromCDouble(0.0));
        auto* limit = env.emit<LoadConst>(Type::fromCDouble(DBL_MAX));
        auto guard = [&](PrimitiveCompareOp comparison,
                         Register* value,
                         Register* bound) {
          auto* valid = env.emit<PrimitiveCompare>(comparison, value, bound);
          env.emitInstr<Guard>(valid)->setFrameState(*instr->frameState());
        };
        guard(PrimitiveCompareOp::kGreaterThan, base, zero);
        auto* power = env.emit<DoubleBinaryOp>(
            BinaryOpKind::kPower,
            base,
            env.emit<LoadConst>(Type::fromCDouble(exponent)));
        guard(PrimitiveCompareOp::kLessThan, power, limit);
        return env.emit<PrimitiveBox>(power, TCDouble, *instr->frameState());
      }
    }
  }
#endif

  // Preserve CPython float_pow rounding and exceptions on every supported
  // version. sqrt/mul/div chains can differ even for finite positive inputs
  // (for example, 2.0 ** -1.5). Constant operands still fold below through
  // the same Python numeric slot used at runtime.
  if (op == BinaryOpKind::kPower && !instr->left()->type().hasObjectSpec()) {
    return nullptr;
  }

  // This isn't safe in the multi-threaded compilation on 3.12 because
  // we don't hold the GIL which is required for allocation.
  RETURN_MULTITHREADED_COMPILE(nullptr);

  Type left_type = instr->left()->type();
  Type right_type = instr->right()->type();

  if (!left_type.hasObjectSpec() || !right_type.hasObjectSpec()) {
    return nullptr;
  }

  ThreadedCompileSerialize guard;
  Ref<> result;

  if (instr->op() == BinaryOpKind::kPower) {
    result = Ref<>::steal(PyFloat_Type.tp_as_number->nb_power(
        left_type.objectSpec(), right_type.objectSpec(), Py_None));
  } else {
    binaryfunc helper = instr->slotMethod();
    result = Ref<>::steal(
        (*helper)(left_type.objectSpec(), right_type.objectSpec()));
  }

  if (result == nullptr) {
    PyErr_Clear();
    return nullptr;
  }

  env.emit<UseType>(instr->left(), left_type);
  env.emit<UseType>(instr->right(), right_type);
  return env.emit<LoadConst>(
      Type::fromObject(env.func.env.addReference(std::move(result))));
}

Register* simplifyUnaryOp(Env& env, const UnaryOp* instr) {
  Register* operand = instr->operand();

  if (instr->op() == UnaryOpKind::kNot && operand->isA(TBool)) {
    env.emit<UseType>(operand, TBool);
    Register* unboxed = env.emit<PrimitiveUnbox>(operand, TCBool);
    Register* negated =
        env.emit<PrimitiveUnaryOp>(PrimitiveUnaryOpKind::kNotInt, unboxed);
    return env.emit<PrimitiveBoxBool>(negated);
  }

  return nullptr;
}

Register* simplifyIntBinaryOp(Env& env, const IntBinaryOp* instr) {
  Register* lhs = instr->left();
  Register* rhs = instr->right();
  BinaryOpKind op = instr->op();

  // CBool & Const.
  if (op == BinaryOpKind::kAnd && lhs->isA(TCBool) && rhs->isA(TCBool)) {
    if (lhs->type().hasIntSpec()) {
      std::swap(lhs, rhs);
    }
    if (rhs->type().hasIntSpec()) {
      env.emit<UseType>(rhs, rhs->type());
      return rhs->type().intSpec() ? lhs : rhs;
    }
  }

  // CBool | Const.
  if (op == BinaryOpKind::kOr && lhs->isA(TCBool) && rhs->isA(TCBool)) {
    if (lhs->type().hasIntSpec()) {
      std::swap(lhs, rhs);
    }
    if (rhs->type().hasIntSpec()) {
      env.emit<UseType>(rhs, rhs->type());
      return rhs->type().intSpec() ? rhs : lhs;
    }
  }

  return nullptr;
}

Register* simplifyPrimitiveCompare(Env& env, const PrimitiveCompare* instr) {
  Register* left = instr->GetOperand(0);
  Register* right = instr->GetOperand(1);
  PrimitiveCompareOp op = instr->op();
  bool commutative =
      op == PrimitiveCompareOp::kEqual || op == PrimitiveCompareOp::kNotEqual;

  if (commutative) {
    auto do_cbool = [&](bool value) {
      env.emit<UseType>(left, left->type());
      env.emit<UseType>(right, right->type());
      return env.emit<LoadConst>(Type::fromCBool(
          op == PrimitiveCompareOp::kNotEqual ? !value : value));
    };
    if (!left->type().couldBe(right->type())) {
      return do_cbool(false);
    }
    if (left->type().hasIntSpec() && right->type().hasIntSpec()) {
      return do_cbool(left->type().intSpec() == right->type().intSpec());
    }
    if (left->type().hasObjectSpec() && right->type().hasObjectSpec()) {
      return do_cbool(left->type().objectSpec() == right->type().objectSpec());
    }
  }

  // Canonicalize boolean constants to the right for == and !=.
  if (commutative && left->isA(TBool) && right->isA(TBool) &&
      left->type().hasObjectSpec() && !right->type().hasObjectSpec()) {
    return env.emit<PrimitiveCompare>(op, right, left);
  }
  if (commutative && left->isA(TCBool) && right->isA(TCBool) &&
      left->type().hasIntSpec() && !right->type().hasIntSpec()) {
    return env.emit<PrimitiveCompare>(op, right, left);
  }

  // box(b) == True --> b
  if (op == PrimitiveCompareOp::kEqual && left->instr()->IsPrimitiveBoxBool() &&
      right->type().asObject() == Py_True) {
    return left->instr()->GetOperand(0);
  }
  // box(b) == False --> !b
  if (op == PrimitiveCompareOp::kEqual && left->instr()->IsPrimitiveBoxBool() &&
      right->type().asObject() == Py_False) {
    return env.emit<PrimitiveUnaryOp>(
        PrimitiveUnaryOpKind::kNotInt, left->instr()->GetOperand(0));
  }
  // box(b1) CMP box(b2) --> b1 CMP b2
  if (left->instr()->IsPrimitiveBoxBool() &&
      right->instr()->IsPrimitiveBoxBool()) {
    return env.emit<PrimitiveCompare>(
        op, left->instr()->GetOperand(0), right->instr()->GetOperand(0));
  }

  return nullptr;
}

Register* simplifyPrimitiveBox(Env& env, const PrimitiveBox* instr) {
  // This isn't safe in the multi-threaded compilation on 3.12 because
  // we don't hold the GIL which is required for allocation.
  RETURN_MULTITHREADED_COMPILE(nullptr);

  Register* input = instr->GetOperand(0);
  Type ty = instr->type();

  Ref<> boxed;
  if (ty.hasIntSpec()) {
    boxed = Ref<>::steal(
        ty <= TCSigned ? PyLong_FromSsize_t(ty.intSpec())
                       : PyLong_FromSize_t(static_cast<size_t>(ty.intSpec())));
  } else if (ty.hasDoubleSpec()) {
    boxed = Ref<>::steal(PyFloat_FromDouble(ty.doubleSpec()));
  } else {
    return nullptr;
  }

  if (boxed == nullptr) {
    PyErr_Clear();
    return nullptr;
  }

  env.emit<UseType>(input, ty);
  return env.emit<LoadConst>(
      Type::fromObject(env.func.env.addReference(std::move(boxed))));
}

Register* simplifyPrimitiveBoxBool(Env& env, const PrimitiveBoxBool* instr) {
  Register* input = instr->GetOperand(0);
  if (input->type().hasIntSpec()) {
    env.emit<UseType>(input, input->type());
    auto bool_obj = input->type().intSpec() ? Py_True : Py_False;
    return env.emit<LoadConst>(Type::fromObject(bool_obj));
  }
  return nullptr;
}

Register* simplifyUnbox(Env& env, const Instr* instr) {
  Register* input_value = instr->GetOperand(0);
  Type output_type = instr->output()->type();
  if (input_value->instr()->IsPrimitiveBox()) {
    // Simplify unbox(box(x)) -> x
    const auto box = static_cast<PrimitiveBox*>(input_value->instr());
    if (box->type() == output_type) {
      // We can't optimize away the potential overflow in unboxing.
      return box->GetOperand(0);
    }
  }
  // Ensure that we are dealing with either a integer or a double.
  Type input_value_type = input_value->type();
  if (!(input_value_type.hasObjectSpec())) {
    return nullptr;
  }
  PyObject* value = input_value_type.objectSpec();
  if (output_type <= (TCSigned | TCUnsigned)) {
    if (!PyLong_Check(value)) {
      return nullptr;
    }
    int overflow = 0;
    long number =
        PyLong_AsLongAndOverflow(input_value_type.objectSpec(), &overflow);
    if (overflow != 0) {
      return nullptr;
    }
    if (output_type <= TCSigned) {
      if (!Type::CIntFitsType(number, output_type)) {
        return nullptr;
      }
      return env.emit<LoadConst>(Type::fromCInt(number, output_type));
    } else {
      if (!Type::CUIntFitsType(number, output_type)) {
        return nullptr;
      }
      return env.emit<LoadConst>(Type::fromCUInt(number, output_type));
    }
  } else if (output_type <= TCDouble) {
    if (!PyFloat_Check(value)) {
      return nullptr;
    }
    double number = PyFloat_AS_DOUBLE(input_value_type.objectSpec());
    return env.emit<LoadConst>(Type::fromCDouble(number));
  }
  return nullptr;
}

#if PY_VERSION_HEX >= 0x030E0000

Register* simplifyLoadAttrSplitDict(
    Env& env,
    const LoadAttr* load_attr,
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<PyUnicodeObject> name) {
  if constexpr (kFreeThreadedBuild) {
    // See T255055907.
    return nullptr;
  }

  if (!PyType_HasFeature(type, Py_TPFLAGS_MANAGED_DICT) ||
      !PyType_HasFeature(type, Py_TPFLAGS_INLINE_VALUES)) {
    return nullptr;
  }
  BorrowedRef<PyHeapTypeObject> heap_type{type};
  if (heap_type->ht_cached_keys == nullptr) {
    return nullptr;
  }
  PyDictKeysObject* keys = heap_type->ht_cached_keys;
  Py_ssize_t attr_idx = getDictKeysIndex(keys, name);
  if (attr_idx == -1) {
    return nullptr;
  }
  // T244151823: For now we deopt on the type changing and in that case, de-opt
  // the whole function. Ideally we'd just skip to the slow-path in this case.
  // PyType_Modified() can notify watchers before the class dict update is
  // visible, so a narrower split-dict patcher can miss descriptor additions.
  Register* receiver = load_attr->GetOperand(0);
  auto patchpoint = env.emitInstr<DeoptPatchpoint>(
      env.func.allocateCodePatcher<TypeDeoptPatcher>(type));
  patchpoint->setGuiltyReg(receiver);
  patchpoint->setDescr("split dict type modified");
  env.emit<UseType>(receiver, receiver->type());

  Register* inline_values_valid = env.emit<LoadField>(
      receiver,
      "inline_values.valid",
      type->tp_basicsize + offsetof(PyDictValues, valid),
      TCUInt8);

  return env.emitCond(
      [&](BasicBlock* bb1, BasicBlock* bb2) {
        env.emit<CondBranch>(inline_values_valid, bb1, bb2);
      },
      [&] { // Inline values are valid.
        Register* maybe_attr = env.emit<LoadField>(
            receiver,
            unicodeAsString(name),
            attr_idx * sizeof(PyObject*) + type->tp_basicsize +
                offsetof(PyDictValues, values),
            TOptObject);
        Register* attr =
            env.emit<CheckField>(maybe_attr, name, *load_attr->frameState());
        static_cast<CheckField*>(attr->instr())->setGuiltyReg(receiver);
        return attr;
      },
      [&] { // Not valid - slow-path, call getattr.
        return env.emit<LoadAttr>(
            receiver,
            load_attr->name_idx(),
            *load_attr->frameState(),
            /* already_optimized= */ true);
      });
}

#else

// Attempt to simplify the given LoadAttr to a split dict load. Assumes various
// sanity checks have already passed:
// - The receiver has a known, exact type.
// - The type has a valid version tag.
// - The type doesn't have a descriptor at the attribute name.
Register* simplifyLoadAttrSplitDict(
    Env& env,
    const LoadAttr* load_attr,
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<PyUnicodeObject> name) {
  if (!PyType_HasFeature(type, Py_TPFLAGS_MANAGED_DICT)) {
    return nullptr;
  }
  BorrowedRef<PyHeapTypeObject> ht(type);
  if (ht->ht_cached_keys == nullptr) {
    return nullptr;
  }
  PyDictKeysObject* keys = ht->ht_cached_keys;
  Py_ssize_t attr_idx = getDictKeysIndex(keys, name);
  if (attr_idx == -1) {
    return nullptr;
  }

  Register* receiver = load_attr->GetOperand(0);
  auto patchpoint = env.emitInstr<DeoptPatchpoint>(
      env.func.allocateCodePatcher<TypeDeoptPatcher>(type));
  patchpoint->setGuiltyReg(receiver);
  patchpoint->setDescr("split dict type modified");
  env.emit<UseType>(receiver, receiver->type());

#if PY_VERSION_HEX < 0x030C0000
  // CPython 3.11 stores the inline values pointer in a separate pre-header slot
  // from the managed dict pointer.  If values is null the instance dict has
  // been materialized, so deopt to the generic LOAD_ATTR path instead of
  // treating the dict pointer as tagged values.
  Register* values = env.emit<LoadField>(
      receiver, "__dict_values__", -4 * sizeof(PyObject*), TCUInt64);
  auto guard = env.emitInstr<Guard>(values);
  guard->setGuiltyReg(receiver);
  guard->setDescr("dict values check");
  Register* values_obj = env.emit<BitCast>(values, TOptObject);
#else
  // PyDictOrValues is stored at -3 per _PyObject_DictOrValuesPointer.
  Register* obj_dict = env.emit<LoadField>(
      receiver, "__dict__", -3 * sizeof(PyObject*), TOptDict);
  // We pass the attribute's name to this CheckField (not "__dict__") because
  // ultimately it means that the attribute we're trying to load is missing,
  // and the AttributeError to be raised should contain the attribute's name.
  Register* checked_dict =
      env.emit<CheckField>(obj_dict, name, *load_attr->frameState());
  static_cast<CheckField*>(checked_dict->instr())->setGuiltyReg(receiver);

  Register* one = env.emit<LoadConst>(Type::fromCUInt(1, TCUInt64));
  Register* dict_ptr = env.emit<BitCast>(checked_dict, TCUInt64);
  Register* is_values =
      env.emit<IntBinaryOp>(BinaryOpKind::kAnd, dict_ptr, one);
  auto guard = env.emitInstr<Guard>(is_values);
  guard->setGuiltyReg(receiver);
  guard->setDescr("dict values check");
  Register* values = env.emit<IntBinaryOp>(BinaryOpKind::kAdd, dict_ptr, one);
  Register* values_obj = env.emit<BitCast>(values, TOptObject);
#endif
  Register* attr = env.emit<LoadField>(
      values_obj, "attr", attr_idx * sizeof(PyObject*), TOptObject);

  Register* checked_attr =
      env.emit<CheckField>(attr, name, *load_attr->frameState());
  static_cast<CheckField*>(checked_attr->instr())->setGuiltyReg(receiver);

  return checked_attr;
}
#endif

// For LoadAttr instructions that resolve to a descriptor, DescrInfo holds
// unpacked state that's used by a number of different simplification cases.
struct DescrInfo {
  FrameState* frame_state;
  Register* receiver;
  Type type;
  BorrowedRef<PyTypeObject> py_type;
  BorrowedRef<PyUnicodeObject> attr_name;
  BorrowedRef<> descr;
};

void emitTypeAttrDeoptPatcher(
    Env& env,
    const DescrInfo& info,
    const char* description) {
  if (_PyClassLoader_IsImmutable(info.py_type)) {
    return;
  }

  // The descriptor could be from a base type, but PyType_Modified() also
  // notifies subtypes of the modified type, so we only have to watch the
  // object's type.
  auto patchpoint = env.emitInstr<DeoptPatchpoint>(
      env.func.allocateCodePatcher<TypeAttrDeoptPatcher>(
          info.py_type, info.attr_name, info.descr));
  patchpoint->setGuiltyReg(info.receiver);
  patchpoint->setDescr(description);
}

Register* simplifyLoadAttrMemberDescr(Env& env, const DescrInfo& info) {
  if (Py_TYPE(info.descr) != &PyMemberDescr_Type) {
    return nullptr;
  }

  // PyMemberDescrs are data descriptors, so we don't need to check if the
  // instance dictionary overrides the descriptor.
  PyMemberDef* def =
      reinterpret_cast<PyMemberDescrObject*>(info.descr.get())->d_member;
  if (def->flags & READ_RESTRICTED) {
    // This should be rare and requires raising an audit event; see
    // Objects/descrobject.c:member_get().
    return nullptr;
  }

  if (def->type == T_OBJECT || def->type == T_OBJECT_EX) {
    const char* name_cstr = PyUnicode_AsUTF8(info.attr_name);
    if (name_cstr == nullptr) {
      PyErr_Clear();
      name_cstr = "<unknown>";
    }
    emitTypeAttrDeoptPatcher(env, info, "member descriptor attribute");
    env.emit<UseType>(info.receiver, info.type);
    Register* field =
        env.emit<LoadField>(info.receiver, name_cstr, def->offset, TOptObject);
    if (def->type == T_OBJECT_EX) {
      auto check_field =
          env.emitInstr<CheckField>(field, info.attr_name, *info.frame_state);
      check_field->setGuiltyReg(info.receiver);
      return check_field->output();
    }

    return env.emitCond(
        [&](BasicBlock* bb1, BasicBlock* bb2) {
          env.emit<CondBranch>(field, bb1, bb2);
        },
        [&] { // Field is set
          return env.emit<RefineType>(TObject, field);
        },
        [&] { // Field is nullptr
          return env.emit<LoadConst>(TNoneType);
        });
  }
  return nullptr;
}

bool simplifyStoreAttrMemberDescr(
    Env& env,
    const DescrInfo& info,
    Register* value) {
#ifdef Py_GIL_DISABLED
  // STORE_ATTR_SLOT uses the object lock in free-threaded builds. StoreField
  // does not, so keep member-descriptor stores on the generic path there.
  (void)env;
  (void)info;
  (void)value;
  return false;
#endif
  if (Py_TYPE(info.descr) != &PyMemberDescr_Type) {
    return false;
  }

  PyMemberDef* def =
      reinterpret_cast<PyMemberDescrObject*>(info.descr.get())->d_member;
  if ((def->flags & READONLY) != 0) {
    return false;
  }
  // PY_WRITE_RESTRICTED is a deprecated no-op in CPython's PyMember_SetOne();
  // unlike READ_RESTRICTED on loads, it does not raise an audit event.
  if (def->type != T_OBJECT && def->type != T_OBJECT_EX) {
    return false;
  }

  const char* name_cstr = PyUnicode_AsUTF8(info.attr_name);
  if (name_cstr == nullptr) {
    PyErr_Clear();
    name_cstr = "<unknown>";
  }

  emitTypeAttrDeoptPatcher(env, info, "member descriptor attribute");
  env.emit<UseType>(info.receiver, info.type);
  Register* previous = env.emit<LoadField>(
      info.receiver,
      name_cstr,
      def->offset,
      TOptObject,
      /* borrowed= */ false);
  env.emitInstr<StoreField>(
      info.receiver, name_cstr, def->offset, value, TOptObject, previous);
  return true;
}

Register* simplifyLoadAttrProperty(Env& env, const DescrInfo& info) {
  if (Py_TYPE(info.descr) != &PyProperty_Type) {
    return nullptr;
  }
  auto property = reinterpret_cast<Ci_propertyobject*>(info.descr.get());
  BorrowedRef<> getter = property->prop_get;
  if (getter == nullptr) {
    return nullptr;
  }

  emitTypeAttrDeoptPatcher(env, info, "property attribute");
  env.emit<UseType>(info.receiver, info.type);
  Register* getter_obj = env.emit<LoadConst>(Type::fromObject(getter));
  auto call = env.emitRawInstr<VectorCall>(
      2, env.func.env.AllocateRegister(), CallFlags::None, *info.frame_state);
  call->SetOperand(0, getter_obj);
  call->SetOperand(1, info.receiver);
  return call->output();
}

Register* simplifyLoadAttrGenericDescriptor(Env& env, const DescrInfo& info) {
  BorrowedRef<PyTypeObject> descr_type = Py_TYPE(info.descr);
  descrgetfunc descr_get = descr_type->tp_descr_get;
  descrsetfunc descr_set = descr_type->tp_descr_set;
  if (descr_get == nullptr || descr_set == nullptr) {
    return nullptr;
  }

  emitTypeAttrDeoptPatcher(env, info, "generic descriptor attribute");
  if (!_PyClassLoader_IsImmutable(descr_type)) {
    // We unfortunately have to use a generic TypeDeoptPatcher here that
    // patches on any changes to the type, since type_setattro() calls
    // PyType_Modified() before updating tp_descr_{get,set}.
    auto patchpoint = env.emitInstr<DeoptPatchpoint>(
        env.func.allocateCodePatcher<TypeDeoptPatcher>(descr_type));
    patchpoint->setGuiltyReg(info.receiver);
    patchpoint->setDescr("tp_descr_get/tp_descr_set");
  }
  env.emit<UseType>(info.receiver, info.type);
  Register* descr_reg = env.emit<LoadConst>(Type::fromObject(info.descr));
  Register* type_reg = env.emit<LoadConst>(Type::fromObject(info.py_type));
  auto call = env.emitRawInstr<CallStatic>(
      3,
      env.func.env.AllocateRegister(),
      reinterpret_cast<void*>(descr_get),
      TOptObject);
  call->SetOperand(0, descr_reg);
  call->SetOperand(1, info.receiver);
  call->SetOperand(2, type_reg);
  return env.emit<CheckExc>(call->output(), *info.frame_state);
}

// Attempt to handle LOAD_ATTR cases where the load is a common case for object
// instances (not types).
Register* simplifyLoadAttrInstanceReceiver(
    Env& env,
    const LoadAttr* load_attr) {
#if PY_VERSION_HEX < 0x030C0000
  // The receiver specializations below bake type facts into machine code
  // and lean on DeoptPatchpoint/TypeDeoptPatcher -- push invalidation a
  // watcherless 3.11 can never deliver (and the running mode structurally
  // refuses patchpoint-bearing artifacts).  3.11 attribute access goes
  // through the pull-validated helper caches instead.
  (void)env;
  (void)load_attr;
  return nullptr;
#else
  Register* receiver = load_attr->GetOperand(0);
  Type type = receiver->type();
  BorrowedRef<PyTypeObject> py_type{type.runtimePyType()};

  if (!type.isExact() || py_type == nullptr ||
      !PyType_HasFeature(py_type, Py_TPFLAGS_READY) ||
      py_type->tp_getattro != PyObject_GenericGetAttr) {
    return nullptr;
  }
  if (getThreadedCompileContext().compileRunning()) {
    // Calling ensureVersionTag() in 3.12+ doesn't work during multi-threaded
    // compile as it wants to access tstate.
    if (!Ci_Type_HasValidVersionTag(py_type)) {
      return nullptr;
    }
  } else if (!ensureVersionTag(py_type)) {
    return nullptr;
  }

  BorrowedRef<PyUnicodeObject> attr_name{load_attr->name()};
  if (!PyUnicode_CheckExact(attr_name)) {
    return nullptr;
  }

  BorrowedRef<> descr{typeLookupSafe(py_type, attr_name)};
  if (descr == nullptr) {
    return simplifyLoadAttrSplitDict(env, load_attr, py_type, attr_name);
  }

  DescrInfo info{
      load_attr->frameState(), receiver, type, py_type, attr_name, descr};
  auto descr_funcs = {
      simplifyLoadAttrMemberDescr,
      simplifyLoadAttrProperty,
      simplifyLoadAttrGenericDescriptor,
  };
  for (auto func : descr_funcs) {
    if (Register* reg = func(env, info)) {
      return reg;
    }
  }
  return nullptr;
#endif
}

Register* simplifyLoadAttrTypeReceiver(Env& env, const LoadAttr* load_attr) {
  Register* receiver = load_attr->GetOperand(0);
  if (!receiver->isA(TType)) {
    return nullptr;
  }

#if PY_VERSION_HEX < 0x030C0000
  // a receiver pinned to one specific class (GuardIs on a
  // class-valued global) lets the load resolve at compile time.  The three
  // facts the runtime cache re-proves per hit are pinned here instead:
  // the owner's MRO answer by the owner's version tag (guard), the
  // metatype by requiring exactly `type` with no attribute of this name
  // (static, `type` is immutable), and the value's non-descriptor nature
  // by requiring no tp_descr_get -- with a version guard on the value's
  // type when that type is a heap type that could later grow __get__.
  if (receiver->type().hasObjectSpec()) {
    BorrowedRef<PyTypeObject> cls{receiver->type().objectSpec()};
    BorrowedRef<PyUnicodeObject> attr_name{load_attr->name()};
    BorrowedRef<> descr;
    if (cls != nullptr && PyType_Check(cls) &&
        Py_TYPE(cls.get()) == &PyType_Type && Ci_Type_HasValidVersionTag(cls) &&
        PyUnicode_CheckExact(attr_name)) {
      // _PyType_Lookup fills the shared method cache; compile workers may
      // run in parallel, so the lookups take the serialization lock.
      ThreadedCompileSerialize guard;
      if (_PyType_Lookup(&PyType_Type, attr_name) == nullptr) {
        descr = _PyType_Lookup(cls, attr_name);
      }
    }
    if (cls != nullptr) {
      if (descr != nullptr && Py_TYPE(descr.get())->tp_descr_get == nullptr) {
        env.emit<UseType>(receiver, TType);
        Register* owner = env.emit<LoadConst>(
            Type::fromObject(env.func.env.addReference(BorrowedRef<>{cls})));
        Register* version = env.emit<LoadField>(
            owner,
            "tp_version_tag",
            offsetof(PyTypeObject, tp_version_tag),
            TCUInt32);
        Register* expected =
            env.emit<LoadConst>(Type::fromCUInt(cls->tp_version_tag, TCUInt32));
        Register* same = env.emit<PrimitiveCompare>(
            PrimitiveCompareOp::kEqual, version, expected);
        env.emit<Guard>(same);
        BorrowedRef<PyTypeObject> value_type{Py_TYPE(descr.get())};
        if (PyType_HasFeature(value_type, Py_TPFLAGS_HEAPTYPE) &&
            !Ci_Type_HasValidVersionTag(value_type)) {
          return nullptr;
        }
        // The payload's live type must still be the captured one: a
        // __class__ assignment (heap types, module subtypes) swaps in a type
        // that may carry __get__ without touching either version tag.
        Register* payload = env.emit<LoadConst>(
            Type::fromObject(env.func.env.addReference(descr)));
        Register* live_type = env.emit<LoadField>(
            payload, "ob_type", offsetof(PyObject, ob_type), TType);
        Register* vt = env.emit<LoadConst>(Type::fromObject(
            env.func.env.addReference(BorrowedRef<>{value_type})));
        Register* same_type = env.emit<PrimitiveCompare>(
            PrimitiveCompareOp::kEqual, live_type, vt);
        env.emit<Guard>(same_type);
        if (PyType_HasFeature(value_type, Py_TPFLAGS_HEAPTYPE)) {
          Register* vt_version = env.emit<LoadField>(
              vt,
              "tp_version_tag",
              offsetof(PyTypeObject, tp_version_tag),
              TCUInt32);
          Register* vt_expected = env.emit<LoadConst>(
              Type::fromCUInt(value_type->tp_version_tag, TCUInt32));
          Register* vt_same = env.emit<PrimitiveCompare>(
              PrimitiveCompareOp::kEqual, vt_version, vt_expected);
          env.emit<Guard>(vt_same);
        }
        return payload;
      }
    }
    // Pinned but not resolvable here (descriptor-valued attribute, metatype
    // attribute, missing name): keep the generic polymorphic cache the
    // unpinned site would have used rather than the type-receiver helper.
    return nullptr;
  }
  // 3.11 has no type watcher, so the inline "cached type pointer matches"
  // fast path below would serve stale answers: the pointer still matches
  // after the class, its metaclass or the value's type is mutated.  The
  // helper IS the seam -- LoadTypeAttrCache::invoke re-proves the owner,
  // metaclass and descriptor facts on every call and only then returns the
  // cached value -- so the site is lowered as a plain helper call with no
  // inline arm.  The saving is the MRO walk, not a memory load.
  {
    const int cache_id = env.func.env.allocateLoadTypeAttrCache();
    env.emit<UseType>(receiver, TType);
    return env.emit<FillTypeAttrCache>(
        receiver, load_attr->name_idx(), cache_id, *load_attr->frameState());
  }
#endif

  const int cache_id = env.func.env.allocateLoadTypeAttrCache();
  env.emit<UseType>(receiver, TType);
  Register* guard = env.emit<LoadTypeAttrCacheEntryType>(cache_id);
  Register* type_matches =
      env.emit<PrimitiveCompare>(PrimitiveCompareOp::kEqual, guard, receiver);
  return env.emitCond(
      [&](BasicBlock* fast_path, BasicBlock* slow_path) {
        env.emit<CondBranch>(type_matches, fast_path, slow_path);
      },
      [&] { // Fast path
        return env.emit<LoadTypeAttrCacheEntryValue>(cache_id);
      },
      [&] { // Slow path
        int name_idx = load_attr->name_idx();
        return env.emit<FillTypeAttrCache>(
            receiver, name_idx, cache_id, *load_attr->frameState());
      });
}

Register* simplifyLoadAttr(Env& env, const LoadAttr* load_attr) {
  if (load_attr->alreadyOptimized()) {
    return nullptr;
  }

  if (Register* reg = simplifyLoadAttrInstanceReceiver(env, load_attr)) {
    return reg;
  }
  if (getConfig().attr_caches) {
    Register* receiver = load_attr->GetOperand(0);
    Type ty = receiver->type();
    BorrowedRef<PyTypeObject> type{ty.runtimePyType()};

    if (type == &PyModule_Type || type == &Ci_StrictModule_Type) {
      BorrowedRef<PyUnicodeObject> attr_name{load_attr->name()};
      // TODO: This is a temporary mitigation for package search path state.
      // LoadModuleAttrCached should eventually be fixed by making global
      // cache / dict watcher invalidation correctly cover importlib-managed
      // module __path__ updates, then this per-attribute exclusion can be
      // removed.
      if (type == &PyModule_Type &&
          PyUnicode_CompareWithASCIIString(attr_name, "__path__") == 0) {
        return nullptr;
      }
      return env.emit<LoadModuleAttrCached>(
          load_attr->GetOperand(0),
          load_attr->name_idx(),
          *load_attr->frameState());
    }

    if (Register* reg = simplifyLoadAttrTypeReceiver(env, load_attr)) {
      return reg;
    }
    return env.emit<LoadAttrCached>(
        load_attr->GetOperand(0),
        load_attr->name_idx(),
        *load_attr->frameState());
  }
  return nullptr;
}

// If we're loading ob_fval from a known float into a double, this can be
// simplified into a LoadConst.
Register* simplifyLoadField(Env& env, const LoadField* instr) {
  Register* loadee = instr->GetOperand(0);
  Type load_output_type = instr->output()->type();
  // Ensure that we are dealing with either a integer or a double.
  Type loadee_type = loadee->type();
  if (!loadee_type.hasObjectSpec()) {
    return nullptr;
  }
  PyObject* value = loadee_type.objectSpec();
  if (PyFloat_Check(value) && load_output_type <= TCDouble &&
      instr->offset() == offsetof(PyFloatObject, ob_fval)) {
    double number = PyFloat_AS_DOUBLE(loadee_type.objectSpec());
    env.emit<UseType>(loadee, loadee_type);
    return env.emit<LoadConst>(Type::fromCDouble(number));
  }
  return nullptr;
}

Register* simplifyIsNegativeAndErrOccurred(
    Env& env,
    const IsNegativeAndErrOccurred* instr) {
  if (!instr->GetOperand(0)->instr()->IsLoadConst()) {
    return nullptr;
  }
  // Other optimizations might reduce the strength of global loads, etc. to load
  // consts. If this is the case, we know that there can't be an active
  // exception. In this case, the IsNegativeAndErrOccurred instruction has a
  // known result. Instead of deleting it, we replace it with load of false -
  // the idea is that if there are other downstream consumers of it, they will
  // still have access to the result. Otherwise, DCE will take care of this.
  Type output_type = instr->output()->type();
  return env.emit<LoadConst>(Type::fromCInt(0, output_type));
}

bool simplifyStoreAttrInstanceReceiver(Env& env, const StoreAttr* store_attr) {
#if PY_VERSION_HEX < 0x030C0000
  // Baked member-descriptor stores have no pull seam either; see
  // simplifyLoadAttrInstanceReceiver.
  (void)env;
  (void)store_attr;
  return false;
#endif
  Register* receiver = store_attr->GetOperand(0);
  Type type = receiver->type();
  BorrowedRef<PyTypeObject> py_type{type.runtimePyType()};

  if (!type.isExact() || py_type == nullptr ||
      !PyType_HasFeature(py_type, Py_TPFLAGS_READY) ||
      py_type->tp_setattro != PyObject_GenericSetAttr) {
    return false;
  }
  if (getThreadedCompileContext().compileRunning()) {
    if (!Ci_Type_HasValidVersionTag(py_type)) {
      return false;
    }
  } else if (!ensureVersionTag(py_type)) {
    return false;
  }

  BorrowedRef<PyUnicodeObject> attr_name{store_attr->name()};
  if (!PyUnicode_CheckExact(attr_name)) {
    return false;
  }

  BorrowedRef<> descr{typeLookupSafe(py_type, attr_name)};
  if (descr == nullptr) {
    return false;
  }

  DescrInfo info{
      store_attr->frameState(), receiver, type, py_type, attr_name, descr};
  return simplifyStoreAttrMemberDescr(env, info, store_attr->GetOperand(1));
}

Register* simplifyStoreAttr(Env& env, const StoreAttr* store_attr) {
  if (simplifyStoreAttrInstanceReceiver(env, store_attr)) {
    return nullptr;
  }
  if (getConfig().attr_caches) {
    return env.emit<StoreAttrCached>(
        store_attr->GetOperand(0),
        store_attr->GetOperand(1),
        store_attr->name_idx(),
        *store_attr->frameState());
  }
  return nullptr;
}

static bool isBuiltin(PyMethodDef* meth, const char* name) {
  // To make sure we have the right function, look up the PyMethodDef in the
  // fixed builtins. Any joker can make a new C method called "len", for
  // example.
  const Builtins& builtins = getContext()->builtins();
  return builtins.find(meth) == name;
}

static bool isBuiltin(Register* callable, const char* name) {
  Type callable_type = callable->type();
  if (!callable_type.hasObjectSpec()) {
    return false;
  }
  PyObject* callable_obj = callable_type.objectSpec();
  if (Py_TYPE(callable_obj) == &PyCFunction_Type) {
    PyCFunctionObject* func =
        reinterpret_cast<PyCFunctionObject*>(callable_obj);
    return isBuiltin(func->m_ml, name);
  }
  if (Py_TYPE(callable_obj) == &PyMethodDescr_Type) {
    PyMethodDescrObject* meth =
        reinterpret_cast<PyMethodDescrObject*>(callable_obj);
    return isBuiltin(meth->d_method, name);
  }
  return false;
}

// This is inspired by _PyEval_EvalCodeWithName in 3.8's Python/ceval.c
// We have a vector of Register* (resolved_args) that gets populated with
// already-provided arguments from call instructions alongside the function's
// default arguments, when such defaults are needed
static Register* resolveArgs(
    Env& env,
    const VectorCall* instr,
    BorrowedRef<PyFunctionObject> target,
    Register* resolved_func = nullptr) {
  BorrowedRef<PyCodeObject> code{target->func_code};
  if (resolved_func == nullptr) {
    resolved_func = instr->func();
  }
  JIT_CHECK(!(code->co_flags & CO_VARARGS), "can't resolve varargs");
  // number of positional args (including args with default values)
  size_t co_argcount = static_cast<size_t>(code->co_argcount);
  if (instr->numArgs() > co_argcount) {
    // TASK(T143644311): support varargs and check if non-varargs here
    return nullptr;
  }

  size_t num_positional = std::min(co_argcount, instr->numArgs());
  std::vector<Register*> resolved_args(co_argcount, nullptr);

  JIT_CHECK(!(code->co_flags & CO_VARKEYWORDS), "can't resolve varkwargs");

  // grab default positional arguments
  BorrowedRef<PyTupleObject> defaults{target->func_defaults};

  // TASK(T143644350): support kwargs and kwdefaults
  size_t num_defaults =
      defaults == nullptr ? 0 : static_cast<size_t>(PyTuple_GET_SIZE(defaults));

  if (num_positional + num_defaults < co_argcount) {
    // function was called with too few arguments
    return nullptr;
  }
  // TASK(T143644377): support kwonly args
  JIT_CHECK(code->co_kwonlyargcount == 0, " can't resolve kwonly args");
  for (size_t i = 0; i < co_argcount; i++) {
    if (i < num_positional) {
      resolved_args[i] = instr->arg(i);
    } else {
      size_t num_non_defaults = co_argcount - num_defaults;
      size_t default_idx = i - num_non_defaults;

      ThreadedCompileSerialize guard;
      auto def = PyTuple_GET_ITEM(defaults, default_idx);
      JIT_CHECK(def != nullptr, "expected non-null default");
      auto type = Type::fromObject(env.func.env.addReference(def));
      resolved_args[i] = env.emit<LoadConst>(type);
    }
    JIT_CHECK(resolved_args.at(i) != nullptr, "expected non-null arg");
  }

  Register* defaults_obj = env.emit<LoadField>(
      resolved_func,
      "func_defaults",
      offsetof(PyFunctionObject, func_defaults),
      TTuple);
  env.emit<GuardIs>(defaults, defaults_obj);
#if PY_VERSION_HEX < 0x030C0000
  // The completion is written against this code object's signature.  A
  // __code__ assignment changes neither the function's identity (pinned by
  // the callee guard) nor its defaults tuple, so the code object is pinned
  // and guarded as well; the guard fails before the call, which the
  // interpreter then makes with the arguments as written.
  env.func.env.addReference(
      BorrowedRef<>{reinterpret_cast<PyObject*>(code.get())});
  Register* code_obj = env.emit<LoadField>(
      resolved_func,
      "func_code",
      offsetof(PyFunctionObject, func_code),
      TObject);
  env.emit<GuardIs>(reinterpret_cast<PyObject*>(code.get()), code_obj);
#endif

  // creates an instruction VectorCall(arg_size, dest_reg, frame_state)
  // and inserts it to the current block. Returns the output of vectorcall
  auto new_instr = env.emitRawInstr<VectorCall>(
      resolved_args.size() + 1,
      env.func.env.AllocateRegister(), // output register
      CallFlags::None,
      *instr->frameState());
  Register* result = new_instr->output();

  // populate the call arguments of the newly created VectorCall
  // the first arg is the function to call
  new_instr->SetOperand(0, resolved_func);
  for (size_t i = 0; i < resolved_args.size(); i++) {
    new_instr->SetOperand(i + 1, resolved_args.at(i));
  }
  result->set_type(outputType(*new_instr));
  return result;
}

#if PY_VERSION_HEX < 0x030C0000
// Direct dispatch of LoadGlobal-called Python functions (including
// default-argument completion) is part of the CPython 3.11 delivery only;
// 3.12+ keeps the pre-existing VectorCall simplifications unchanged.
static bool canResolveArgs(
    const VectorCall* instr,
    BorrowedRef<PyFunctionObject> target) {
  BorrowedRef<PyCodeObject> code{target->func_code};
  size_t co_argcount = static_cast<size_t>(code->co_argcount);
  if (instr->numArgs() > co_argcount) {
    return false;
  }

  size_t num_positional = std::min(co_argcount, instr->numArgs());
  BorrowedRef<PyTupleObject> defaults{target->func_defaults};
  size_t num_defaults =
      defaults == nullptr ? 0 : static_cast<size_t>(PyTuple_GET_SIZE(defaults));
  return num_positional + num_defaults >= co_argcount;
}

static BorrowedRef<PyFunctionObject> resolveLoadGlobalFuncTarget(
    Env& env,
    const LoadGlobal* instr) {
  if (env.func.code == nullptr || env.func.globals == nullptr ||
      env.func.builtins == nullptr) {
    return nullptr;
  }

  BorrowedRef<PyCodeObject> code{env.func.code};
  int name_idx = instr->name_idx();
  if (name_idx < 0 || name_idx >= PyTuple_GET_SIZE(code->co_names)) {
    return nullptr;
  }

  BorrowedRef<> name = PyTuple_GET_ITEM(code->co_names, name_idx);
  JIT_CHECK(name != nullptr, "LOAD_GLOBAL name cannot be null");

  ThreadedCompileSerialize guard;
  PyObject* target = PyDict_GetItemWithError(
      reinterpret_cast<PyObject*>(env.func.globals.get()), name);
  if (target == nullptr && !PyErr_Occurred()) {
    target = PyDict_GetItemWithError(
        reinterpret_cast<PyObject*>(env.func.builtins.get()), name);
  }
  if (PyErr_Occurred()) {
    PyErr_Clear();
    return nullptr;
  }
  if (target == nullptr || !PyFunction_Check(target)) {
    return nullptr;
  }
  return reinterpret_cast<PyFunctionObject*>(target);
}

static Register* resolveLoadGlobalFuncArgs(
    Env& env,
    const VectorCall* instr,
    BorrowedRef<PyFunctionObject> target) {
  Register* func = instr->func();
  if (func->instr() == nullptr || !func->instr()->IsLoadGlobal()) {
    return nullptr;
  }

  BorrowedRef<PyFunctionObject> current_target =
      resolveLoadGlobalFuncTarget(env, static_cast<LoadGlobal*>(func->instr()));
  if (current_target != target) {
    return nullptr;
  }
  if (!canResolveArgs(instr, target)) {
    return nullptr;
  }

  BorrowedRef<> kept_target = env.func.env.addReference(target);
  auto guarded = env.emit<GuardIs>(kept_target, func);
  return resolveArgs(env, instr, target, guarded);
}
#endif // PY_VERSION_HEX < 0x030C0000

Register* simplifyCallMethod(Env& env, const CallMethod* instr) {
#if PY_VERSION_HEX < 0x030C0000
  // If this is the guarded CPython 3.11 method-cache path, call the cached
  // Python function directly with the pushed receiver. Do not apply this to
  // ordinary LoadMethod results: polymorphic method sites may temporarily have
  // a TFunc callable while still needing the generic CallMethod semantics.
  Instr* func_instr = instr->func()->instr();
  Type func_type = instr->func()->type();
  if (func_instr != nullptr && func_instr->IsLoadConst() &&
      func_type.hasObjectSpec() && PyFunction_Check(func_type.objectSpec()) &&
      !(instr->self()->type() <= TNullptr)) {
    auto call = env.emitRawInstr<VectorCall>(
        instr->NumOperands(),
        env.func.env.AllocateRegister(),
        instr->flags(),
        *instr->frameState());
    for (size_t i = 0; i < instr->NumOperands(); ++i) {
      call->SetOperand(i, instr->GetOperand(i));
    }
    call->output()->set_type(instr->output()->type());
    return call->output();
  }
#endif

  if constexpr (PY_VERSION_HEX >= 0x030E0000) {
    if (instr->self()->type() <= TNullptr) {
      auto call = env.emitRawInstr<VectorCall>(
          instr->NumOperands() - 1,
          env.func.env.AllocateRegister(),
          instr->flags(),
          *instr->frameState());
      call->SetOperand(0, instr->GetOperand(0));
      for (size_t i = 2; i < instr->NumOperands(); ++i) {
        call->SetOperand(i - 1, instr->GetOperand(i));
      }
      call->output()->set_type(instr->output()->type());
      return call->output();
    }
  } else {
    if (instr->func()->type() <= TNullptr) {
      auto call = env.emitRawInstr<VectorCall>(
          instr->NumOperands() - 1,
          env.func.env.AllocateRegister(),
          instr->flags(),
          *instr->frameState());
      for (size_t i = 1; i < instr->NumOperands(); ++i) {
        call->SetOperand(i - 1, instr->GetOperand(i));
      }
      return call->output();
    }
  }

  return nullptr;
}

// Translate VectorCall to CallStatic whenever possible, saving stack
// manipulation costs (pushing args to stack).
static Register* trySpecializeCCall(Env& env, const VectorCall* instr) {
  Register* callable = instr->func();
  Type callable_type = callable->type();
  PyObject* callable_obj = callable_type.asObject();
  if (callable_obj == nullptr) {
    return nullptr;
  }

  // Non METH_STATIC and METH_CLASS tp_methods on types are stored as
  // PyMethodDescr inside tp_dict. Check out:
  // Objects/typeobject.c#type_add_method
  if (Py_TYPE(callable_obj) == &PyMethodDescr_Type) {
    auto meth = reinterpret_cast<PyMethodDescrObject*>(callable_obj);
    PyMethodDef* def = meth->d_method;
    if (def->ml_flags & METH_NOARGS && instr->numArgs() == 1) {
      Register* result = env.emitVariadic<CallStatic>(
          1,
          reinterpret_cast<void*>(def->ml_meth),
          instr->output()->type() | TNullptr,
          /* self */ instr->arg(0));
      return env.emit<CheckExc>(result, *instr->frameState());
    }
    if (def->ml_flags & METH_O && instr->numArgs() == 2) {
      Register* result = env.emitVariadic<CallStatic>(
          2,
          reinterpret_cast<void*>(def->ml_meth),
          instr->output()->type() | TNullptr,
          /* self */ instr->arg(0),
          /* arg */ instr->arg(1));
      return env.emit<CheckExc>(result, *instr->frameState());
    }
  }
  return nullptr;
}

Register* simplifyVectorCallStatic(Env& env, const VectorCall* instr) {
  if (!(instr->flags() & CallFlags::Static)) {
    return nullptr;
  }
  Register* func = instr->func();
  if (isBuiltin(func, "list.append") && instr->numArgs() == 2) {
    env.emit<UseType>(func, func->type());
    env.emit<ListAppend>(instr->arg(0), instr->arg(1), *instr->frameState());
    return env.emit<LoadConst>(TNoneType);
  }

  return trySpecializeCCall(env, instr);
}

Register* simplifyBoundListInsert(Env& env, const VectorCall* instr) {
#ifdef Py_GIL_DISABLED
  return nullptr;
#else
  if ((instr->flags() & CallFlags::KwArgs) || instr->numArgs() != 2) {
    return nullptr;
  }

  Instr* callee_def = instr->func()->instr();
  if (callee_def == nullptr || !callee_def->IsLoadAttrCached()) {
    return nullptr;
  }

  auto* load_attr = static_cast<const LoadAttrCached*>(callee_def);
  if (PyUnicode_CompareWithASCIIString(load_attr->name(), "insert") != 0) {
    return nullptr;
  }

  Register* receiver = load_attr->GetOperand(0);
  Register* index = instr->arg(0);
  Register* value = instr->arg(1);
  if (!receiver->isA(TListExact) || !index->isA(TLongExact)) {
    return nullptr;
  }

  env.emit<UseType>(receiver, TListExact);
  Register* unboxed_index = nullptr;
  Type index_type = index->type();
  // Worker threads used by precompile_all() do not have a current Python
  // thread state, so they must not call APIs that read or write PyErr.
  if (index_type.hasObjectSpec() &&
      !getThreadedCompileContext().compileRunning()) {
    Py_ssize_t constant_index = PyLong_AsSsize_t(index_type.objectSpec());
    if (constant_index == -1 && PyErr_Occurred()) {
      // Keep overflowing constants on the runtime conversion path.  That path
      // deopts and lets list.insert's normal slice-index conversion preserve
      // its clipping semantics.
      PyErr_Clear();
    } else {
      // The full specialized type is required here: using only TLongExact
      // would lose the value dependency and make this constant invalid for a
      // different exact int.
      env.emit<UseType>(index, index_type);
      unboxed_index =
          env.emit<LoadConst>(Type::fromCInt(constant_index, TCInt64));
    }
  }

  if (unboxed_index == nullptr) {
    env.emit<UseType>(index, TLongExact);
    unboxed_index = env.func.env.AllocateRegister();
    env.emitRawInstr<CallStatic>(
        1,
        unboxed_index,
        reinterpret_cast<void*>(PyLong_AsSsize_t),
        TCInt64,
        index);
    env.emit<IsNegativeAndErrOccurred>(unboxed_index, *instr->frameState());
  }

  auto output = env.func.env.AllocateRegister();
  env.emitRawInstr<CallStatic>(
      3,
      output,
      reinterpret_cast<void*>(PyList_Insert),
      TCInt32,
      receiver,
      unboxed_index,
      value);
  env.emit<CheckNeg>(output, *instr->frameState());
  return env.emit<LoadConst>(TNoneType);
#endif
}

// Special case here where we are testing `if isinstance`. In that case we do
// not want to go through the boxing and then unboxing that we are about to do.
// Instead, we want to directly provide the result of the unboxed comparison.
std::optional<std::pair<Instr*, std::vector<Instr*>>> isVectorCallIfIsInstance(
    Env& env,
    const VectorCall* instr) {
  std::vector<Instr*> snapshots;

  Register* output = nullptr;

  enum state { kInitial, kCondBranch, kIsTruthy, kFailed };
  auto state = kInitial;

  auto block = instr->block();
  for (auto current = block->rbegin();
       current != block->rend() && state != kFailed;
       ++current) {
    switch (state) {
      case kInitial: {
        if (!current->IsCondBranch()) {
          state = kFailed;
          break;
        }

        const auto& last_uses = env.lastUses();
        auto lu_at_condbranch = last_uses.find(&*current);
        if (lu_at_condbranch == last_uses.end() ||
            lu_at_condbranch->second.size() != 1) {
          // If the CondBranch instruction is not the last use of the
          // IsTruthy output, then we cannot perform this optimization.
          state = kFailed;
          break;
        }

        state = kCondBranch;
        output = current->GetOperand(0);
        break;
      }
      case kCondBranch: {
        if (current->IsIsTruthy() && output == current->output() &&
            current->GetOperand(0) == instr->output()) {
          const auto& last_uses = env.lastUses();
          auto lu_at_istruthy = last_uses.find(&*current);
          if (lu_at_istruthy == last_uses.end() ||
              lu_at_istruthy->second.size() != 1) {
            // If the IsTruthy instruction is not the last use of the VectorCall
            // output, then we cannot perform this optimization.
            state = kFailed;
          } else {
            state = kIsTruthy;
          }
          break;
        }

        if (current->IsSnapshot()) {
          snapshots.push_back(&*current);
          break;
        }

        state = kFailed;
        break;
      }
      case kIsTruthy: {
        if (&*current == instr) {
          JIT_CHECK(output != nullptr, "output should have been set");
          return std::make_optional(std::make_pair(output->instr(), snapshots));
        }

        if (current->IsSnapshot()) {
          // Leave these snapshots in place.
          break;
        }

        state = kFailed;
        break;
      }
      case kFailed:
        JIT_ABORT("Hit kFailed state but it should not be reachable");
    }
  }

  // If we found anything else between the VectorCall, IsTruthy, and CondBranch
  // besides the expected instructions and some snapshots, then we cannot
  // perform this optimization.
  return std::nullopt;
}

#if PY_VERSION_HEX < 0x030C0000
// A keyword call whose callee is a compile-time constant Python function (a
// guarded global, a pinned method) is bound here, at compile time, into the
// positional call the callee's direct entry takes.  The names tuple is a code
// constant, so the site's (positional count, names) resolve against the
// callee's signature once: every callee slot maps to a positional value, a
// keyword value or a default by index.  Two facts the callee constant does
// not pin are re-proved per call: the function's code object (a __code__
// assignment changes the signature) by identity, and its defaults tuple by
// shape (an exact tuple of the arity seen here; the values are read live, so
// a same-arity rebinding needs no deopt).  Shapes the binder cannot settle
// statically -- varargs / varkeywords / keyword-only parameters, static
// prototypes, more than 16 parameters, a name that is not a parameter or
// names a positional-only one, a parameter given twice, a required parameter
// unset -- keep the keyword call (the generic path binds or raises).
Register* trySimplifyStaticKwCall311(Env& env, const VectorCall* instr) {
  Register* callable = instr->func();
  Type ct = callable->type();
  if (!ct.hasObjectSpec()) {
    return nullptr;
  }
  PyObject* fobj = ct.objectSpec();
  if (fobj == nullptr || !PyFunction_Check(fobj)) {
    return nullptr;
  }
  const size_t nops = instr->NumOperands();
  if (nops < 2) {
    return nullptr;
  }
  Register* kwnames_reg = instr->GetOperand(nops - 1);
  Type kt = kwnames_reg->type();
  if (!kt.hasObjectSpec()) {
    return nullptr;
  }
  PyObject* kwnames = kt.objectSpec();
  if (kwnames == nullptr || !PyTuple_CheckExact(kwnames)) {
    return nullptr;
  }
  const Py_ssize_t nkw = PyTuple_GET_SIZE(kwnames);
  const Py_ssize_t nargs_total = static_cast<Py_ssize_t>(nops) - 2;
  const Py_ssize_t npos = nargs_total - nkw;
  if (npos < 0) {
    return nullptr;
  }
  auto* func = reinterpret_cast<PyFunctionObject*>(fobj);
  auto* co = reinterpret_cast<PyCodeObject*>(func->func_code);
  const Py_ssize_t argcount = co->co_argcount;
  if ((co->co_flags & (CO_VARARGS | CO_VARKEYWORDS)) ||
      co->co_kwonlyargcount != 0 ||
      (co->co_flags & CI_CO_STATICALLY_COMPILED) || argcount > 16 ||
      npos > argcount || nargs_total > argcount) {
    return nullptr;
  }
  constexpr int kUnset = -128;
  int src[16];
  for (Py_ssize_t i = 0; i < argcount; ++i) {
    src[i] = i < npos ? static_cast<int>(i) : kUnset;
  }
  for (Py_ssize_t k = 0; k < nkw; ++k) {
    PyObject* keyword = PyTuple_GET_ITEM(kwnames, k);
    Py_ssize_t j = -1;
    for (Py_ssize_t c = co->co_posonlyargcount; c < argcount; ++c) {
      if (getVarname(co, c) == keyword) {
        j = c;
        break;
      }
    }
    if (j < 0 || src[j] != kUnset) {
      return nullptr;
    }
    src[j] = static_cast<int>(npos + k);
  }
  PyObject* defaults = func->func_defaults;
  const Py_ssize_t defcount =
      defaults == nullptr ? 0 : PyTuple_GET_SIZE(defaults);
  bool need_defaults = false;
  for (Py_ssize_t j = 0; j < argcount; ++j) {
    if (src[j] != kUnset) {
      continue;
    }
    Py_ssize_t d = j - (argcount - defcount);
    if (d < 0) {
      return nullptr;
    }
    src[j] = static_cast<int>(-(d + 1));
    need_defaults = true;
  }
  const FrameState* dominating_fs = instr->getDominatingFrameState();
  if (dominating_fs == nullptr) {
    return nullptr;
  }
  // The guards below fail back to the interpreter.  The call's own frame
  // state resumes at CALL, but the keyword names of a 3.11 call are not on
  // the operand stack: KW_NAMES sets them in the evaluator's call shape,
  // which a frame entered at CALL has empty -- the keyword arguments would
  // be bound as positional ones.  The guards therefore resume at the
  // KW_NAMES instruction (its EXTENDED_ARG prefix included): the operand
  // stack is the same there, no argument is evaluated again, and the
  // interpreter replays KW_NAMES, PRECALL and CALL.
  FrameState guard_fs = *dominating_fs;
  {
    BorrowedRef<PyCodeObject> caller_code = guard_fs.code;
    if (caller_code == nullptr) {
      return nullptr;
    }
    const BCOffset call_off = guard_fs.cur_instr_offs;
    BCOffset resume_off{-1};
    BytecodeInstructionBlock block{caller_code};
    BytecodeInstruction prev1{caller_code, BCOffset{0}};
    BytecodeInstruction prev2{caller_code, BCOffset{0}};
    int seen = 0;
    for (const BytecodeInstruction& bc : block) {
      if (bc.baseOffset() == call_off || bc.opcodeOffset() == call_off) {
        if (seen >= 2 && bc.opcode() == CALL && prev1.opcode() == PRECALL &&
            prev2.opcode() == KW_NAMES) {
          resume_off = prev2.baseOffset();
        }
        break;
      }
      prev2 = prev1;
      prev1 = bc;
      seen++;
    }
    if (resume_off < BCOffset{0}) {
      return nullptr;
    }
    guard_fs.cur_instr_offs = resume_off;
  }
  // The code object is compared by identity; pin it so a __code__
  // reassignment cannot recycle the address under the guard.
  env.func.env.addReference(BorrowedRef<>{reinterpret_cast<PyObject*>(co)});
  env.emit<Snapshot>(guard_fs);
  Register* code_reg = env.emit<LoadField>(
      callable, "func_code", offsetof(PyFunctionObject, func_code), TObject);
  env.emit<GuardIs>(reinterpret_cast<PyObject*>(co), code_reg);
  Register* defaults_reg = nullptr;
  if (need_defaults) {
    defaults_reg = env.emit<LoadField>(
        callable,
        "func_defaults",
        offsetof(PyFunctionObject, func_defaults),
        TOptObject);
    // __defaults__ = None clears the slot to NULL; the type guard reads
    // ob_type, so the null case must deopt first.
    env.emit<Snapshot>(guard_fs);
    env.emit<Guard>(defaults_reg);
    defaults_reg = env.emit<RefineType>(TObject, defaults_reg);
    env.emit<Snapshot>(guard_fs);
    defaults_reg = env.emit<GuardType>(TTupleExact, defaults_reg);
    Register* size = env.emit<LoadField>(
        defaults_reg, "ob_size", offsetof(PyVarObject, ob_size), TCInt64);
    Register* expected = env.emit<LoadConst>(Type::fromCInt(defcount, TCInt64));
    Register* same_size =
        env.emit<PrimitiveCompare>(PrimitiveCompareOp::kEqual, size, expected);
    env.emit<Snapshot>(guard_fs);
    env.emit<Guard>(same_size);
  }
  // Every operand must be defined before the call is emitted (the default
  // loads go first; the call takes the cursor after them).
  Register* bound[16];
  for (Py_ssize_t j = 0; j < argcount; ++j) {
    bound[j] = src[j] >= 0 ? instr->GetOperand(1 + src[j])
                           : env.emit<LoadTupleItem>(defaults_reg, -src[j] - 1);
  }
  CallFlags flags = static_cast<CallFlags>(
      static_cast<uint32_t>(instr->flags()) &
      ~static_cast<uint32_t>(CallFlags::KwArgs));
  auto call = env.emitRawInstr<VectorCall>(
      static_cast<size_t>(argcount) + 1,
      env.func.env.AllocateRegister(),
      flags,
      *instr->frameState());
  call->SetOperand(0, callable);
  for (Py_ssize_t j = 0; j < argcount; ++j) {
    call->SetOperand(j + 1, bound[j]);
  }
  call->output()->set_type(instr->output()->type());
  return call->output();
}

// SPIKE(ISINSTINLINE): the per-site cache probe of JITRT_IsInstanceCached311
// in HIR -- receiver type, class, type version tag and the valid-tag flag
// against the site's entry -- so a hit costs loads and compares instead of a
// helper call.  The miss arm keeps the helper, which also fills the entry.
Register* emitIsInstanceSlowArm(
    Env& env,
    Register* obj,
    Register* obj_type,
    Register* cls,
    const FrameState& frame) {
  static const bool inline_probe =
      Ci_SpikeGetenv("SPIKE_ISINSTINLINE") != nullptr &&
      Ci_SpikeGetenv("SPIKE_ISINSTCACHE") != nullptr &&
      Ci_SpikeGetenv("SPIKE_ISINSTFAST") != nullptr;
  PyCodeObject* code = env.func.code.get();
  if (!inline_probe || code == nullptr) {
    return env.emit<IsInstance>(obj, cls, frame);
  }
  JITRT_IsInstanceSiteCache311* cache =
      env.func.predicateCache311<JITRT_IsInstanceSiteCache311>(
          static_cast<int64_t>(env.bc_off.value()), 0);
  Register* entry = env.emit<LoadConst>(Type::fromCPtr(cache));
  Register* cached_type = env.emit<LoadField>(
      entry, "type", offsetof(JITRT_IsInstanceSiteCache311, type), TOptObject);
  Register* type_ok = env.emit<PrimitiveCompare>(
      PrimitiveCompareOp::kEqual, obj_type, cached_type);
  Register* cached_cls = env.emit<LoadField>(
      entry, "cls", offsetof(JITRT_IsInstanceSiteCache311, cls), TOptObject);
  Register* cls_ok =
      env.emit<PrimitiveCompare>(PrimitiveCompareOp::kEqual, cls, cached_cls);
  Register* version = env.emit<LoadField>(
      obj_type,
      "tp_version_tag",
      offsetof(PyTypeObject, tp_version_tag),
      TCUInt32);
  Register* cached_version = env.emit<LoadField>(
      entry,
      "version",
      offsetof(JITRT_IsInstanceSiteCache311, version),
      TCUInt32);
  Register* version_ok = env.emit<PrimitiveCompare>(
      PrimitiveCompareOp::kEqual, version, cached_version);
  Register* flags = env.emit<LoadField>(
      obj_type, "tp_flags", offsetof(PyTypeObject, tp_flags), TCUInt64);
  Register* valid_bit = env.emit<LoadConst>(
      Type::fromCUInt(Py_TPFLAGS_VALID_VERSION_TAG, TCUInt64));
  Register* valid_bits =
      env.emit<IntBinaryOp>(BinaryOpKind::kAnd, flags, valid_bit);
  Register* zero64 = env.emit<LoadConst>(Type::fromCUInt(0, TCUInt64));
  Register* valid = env.emit<PrimitiveCompare>(
      PrimitiveCompareOp::kNotEqual, valid_bits, zero64);
  Register* cls_version = env.emit<LoadField>(
      cls, "tp_version_tag", offsetof(PyTypeObject, tp_version_tag), TCUInt32);
  Register* cached_cls_version = env.emit<LoadField>(
      entry,
      "cls_version",
      offsetof(JITRT_IsInstanceSiteCache311, cls_version),
      TCUInt32);
  Register* cls_version_ok = env.emit<PrimitiveCompare>(
      PrimitiveCompareOp::kEqual, cls_version, cached_cls_version);
  Register* cls_flags = env.emit<LoadField>(
      cls, "tp_flags", offsetof(PyTypeObject, tp_flags), TCUInt64);
  Register* cls_valid_bits =
      env.emit<IntBinaryOp>(BinaryOpKind::kAnd, cls_flags, valid_bit);
  Register* cls_valid = env.emit<PrimitiveCompare>(
      PrimitiveCompareOp::kNotEqual, cls_valid_bits, zero64);
  Register* hit = env.emit<IntBinaryOp>(BinaryOpKind::kAnd, type_ok, cls_ok);
  hit = env.emit<IntBinaryOp>(BinaryOpKind::kAnd, hit, cls_version_ok);
  hit = env.emit<IntBinaryOp>(BinaryOpKind::kAnd, hit, cls_valid);
  hit = env.emit<IntBinaryOp>(BinaryOpKind::kAnd, hit, version_ok);
  hit = env.emit<IntBinaryOp>(BinaryOpKind::kAnd, hit, valid);
  return env.emitCond(
      [&](BasicBlock* t, BasicBlock* f) { env.emit<CondBranch>(hit, t, f); },
      [&] {
        Register* result = env.emit<LoadField>(
            entry,
            "result",
            offsetof(JITRT_IsInstanceSiteCache311, result),
            TCInt32);
        Register* zero32 = env.emit<LoadConst>(Type::fromCInt(0, TCInt32));
        return env.emit<PrimitiveCompare>(
            PrimitiveCompareOp::kNotEqual, result, zero32);
      },
      [&] { return env.emit<IsInstance>(obj, cls, frame); });
}
#else
Register* emitIsInstanceSlowArm(
    Env& env,
    Register* obj,
    Register* /* obj_type */,
    Register* cls,
    const FrameState& frame) {
  return env.emit<IsInstance>(obj, cls, frame);
}
#endif

Register* simplifyVectorCall(Env& env, const VectorCall* instr) {
  if (Register* result = simplifyVectorCallStatic(env, instr)) {
    return result;
  }
  if (Register* result = simplifyBoundListInsert(env, instr)) {
    return result;
  }
  if (instr->flags() & CallFlags::KwArgs) {
#if PY_VERSION_HEX < 0x030C0000
    return trySimplifyStaticKwCall311(env, instr);
#else
    return nullptr;
#endif
  }

  Register* target = instr->GetOperand(0);
  Type target_type = target->type();
  if (target_type == env.type_object && instr->NumOperands() == 2) {
    env.emit<UseType>(target, env.type_object);
    return env.emit<LoadField>(
        instr->GetOperand(1), "ob_type", offsetof(PyObject, ob_type), TType);
  }
  if (isBuiltin(target, "len") && instr->numArgs() == 1) {
    env.emit<UseType>(target, target->type());
    return env.emit<GetLength>(instr->arg(0), *instr->frameState());
  }
#if PY_VERSION_HEX < 0x030C0000
  // SPIKE(ISINSTGEN): the isinstance calls the inline type test below does
  // not take -- a class tuple, or a class operand of unknown type -- replay
  // the answer from a per-site cache instead of calling the builtin.
  static const bool spike_isinstgen =
      Ci_SpikeGetenv("SPIKE_ISINSTGEN") != nullptr;
  if (spike_isinstgen && isBuiltin(target, "isinstance") &&
      instr->numArgs() == 2 && env.func.code.get() != nullptr &&
      !(instr->GetOperand(2)->type() <= TType &&
        !(instr->GetOperand(2)->type() <= TTuple))) {
    JITRT_IsInstanceGenCache311* cache =
        env.func.predicateCache311<JITRT_IsInstanceGenCache311>(
            static_cast<int64_t>(env.bc_off.value()), 1);
    env.emit<UseType>(target, target->type());
    Register* entry = env.emit<LoadConst>(Type::fromCPtr(cache));
    Register* result = env.emitVariadic<CallStatic>(
        3,
        reinterpret_cast<void*>(JITRT_IsInstanceGen311),
        TBool | TNullptr,
        entry,
        instr->GetOperand(1),
        instr->GetOperand(2));
    return env.emit<CheckExc>(result, *instr->frameState());
  }
  // SPIKE(GETATTRDFL): getattr(obj, "name", default) with a constant name.
  static const bool spike_getattrdfl =
      Ci_SpikeGetenv("SPIKE_GETATTRDFL") != nullptr;
  if (spike_getattrdfl && isBuiltin(target, "getattr") &&
      instr->numArgs() == 3 && env.func.code.get() != nullptr) {
    Type name_type = instr->GetOperand(2)->type();
    if (name_type.hasObjectSpec() &&
        PyUnicode_CheckExact(name_type.objectSpec())) {
      JITRT_GetAttrDefaultCache311* cache =
          env.func.predicateCache311<JITRT_GetAttrDefaultCache311>(
              static_cast<int64_t>(env.bc_off.value()), 2);
      env.emit<UseType>(target, target->type());
      Register* entry = env.emit<LoadConst>(Type::fromCPtr(cache));
      Register* result = env.emitVariadic<CallStatic>(
          4,
          reinterpret_cast<void*>(JITRT_GetAttrDefault311),
          TObject | TNullptr,
          entry,
          instr->GetOperand(1),
          instr->GetOperand(2),
          instr->GetOperand(3));
      return env.emit<CheckExc>(result, *instr->frameState());
    }
  }
#endif
  if (isBuiltin(target, "isinstance") && instr->numArgs() == 2 &&
      instr->GetOperand(2)->type() <= TType &&
      !(instr->GetOperand(2)->type() <= TTuple)) {
    auto obj_op = instr->GetOperand(1);
    auto type_op = instr->GetOperand(2);

    auto obj_type = env.emit<LoadField>(
        obj_op, "ob_type", offsetof(PyObject, ob_type), TType);

    auto compare_type = env.emit<PrimitiveCompare>(
        PrimitiveCompareOp::kEqual, obj_type, type_op);

    // If this is a VectorCall to isinstance and it's being used as the
    // predicate of an if statement, it will look like:
    //
    //     o1 = VectorCall
    //     o2 = IsTruthy o1
    //     CondBranch o2
    //
    // Below, this would then expand into boxing the bool on both sides of the
    // conditional, then unboxing it again to do another comparison. Instead, we
    // can circumvent that by directly using the result of the primitive
    // compare.
    auto data = isVectorCallIfIsInstance(env, instr);
    if (data.has_value()) {
      auto& [is_truthy, snapshots] = data.value();
      auto result = is_truthy->output();

      // We no longer need the IsTruthy instruction.
      is_truthy->unlink();
      delete is_truthy;

      // We also no longer need the Snapshot instructions contained between the
      // IsTruthy instruction and the CondBranch instruction.
      for (auto snapshot : snapshots) {
        snapshot->unlink();
        delete snapshot;
      }

      env.emitCondSlowPath(
          result,
          compare_type,
          [&](auto slow_path) {
            return env.emitInstr<CondBranch>(compare_type, nullptr, slow_path);
          },
          [&] {
            return emitIsInstanceSlowArm(
                env, obj_op, obj_type, type_op, *instr->frameState());
          });

      // The output of the VectorCall instruction was previously a TBool, but we
      // are replacing it with a TCBool since we are now doing a primitive
      // compare instead. This works, but requires that we change the
      // instruction's output type to match in order to pass the assertions that
      // come after the call to simplifyInstr.
      instr->output()->set_type(TCBool);

      return result;
    }

    Register* cbool_res = env.emitCond(
        [&](BasicBlock* fast_path, BasicBlock* slow_path) {
          env.emit<CondBranch>(compare_type, fast_path, slow_path);
        },
        [&] { // Fast path
          return compare_type;
        },
        [&] { // Slow path
          return emitIsInstanceSlowArm(
              env, obj_op, obj_type, type_op, *instr->frameState());
        });
    return env.emit<PrimitiveBoxBool>(cbool_res);
  }
  if (target_type.hasValueSpec(TFunc)) {
    BorrowedRef<PyFunctionObject> func{target_type.objectSpec()};
    BorrowedRef<PyCodeObject> code{func->func_code};
    if (code->co_kwonlyargcount > 0 || (code->co_flags & CO_VARARGS) ||
        (code->co_flags & CO_VARKEYWORDS)) {
      // TASK(T143644854): full argument resolution
      return nullptr;
    }

    JIT_CHECK(
        code->co_argcount >= 0,
        "argcount must be greater than or equal to zero");
    if (instr->numArgs() != static_cast<size_t>(code->co_argcount)) {
      return resolveArgs(env, instr, func);
    }
  }
#if PY_VERSION_HEX < 0x030C0000
  else if (target->instr() != nullptr && target->instr()->IsLoadGlobal()) {
    if (auto func = resolveLoadGlobalFuncTarget(
            env, static_cast<LoadGlobal*>(target->instr()))) {
      BorrowedRef<PyCodeObject> code{func->func_code};
      if (code->co_kwonlyargcount > 0 || (code->co_flags & CO_VARARGS) ||
          (code->co_flags & CO_VARKEYWORDS)) {
        return nullptr;
      }
      if (instr->numArgs() != static_cast<size_t>(code->co_argcount)) {
        return resolveLoadGlobalFuncArgs(env, instr, func);
      }
    }
  }
#endif
  return nullptr;
}

Register* simplifyStoreSubscr(Env& env, const StoreSubscr* instr) {
  if (instr->GetOperand(0)->isA(TDictExact)) {
    env.emit<UseType>(instr->GetOperand(0), TDictExact);
    auto output = env.func.env.AllocateRegister();
    env.emitRawInstr<CallStatic>(
        3,
        output,
        reinterpret_cast<void*>(PyDict_Type.tp_as_mapping->mp_ass_subscript),
        TCInt32,
        instr->GetOperand(0),
        instr->GetOperand(1),
        instr->GetOperand(2));

    env.emit<CheckNeg>(output, *instr->frameState());
    return nullptr;
  }

  // C3/C4/C5: direct list[int] store with compact-index unboxing and inlined
  // negative-index normalization/bounds checking.
  if (!kFreeThreadedBuild && instr->GetOperand(0)->isA(TListExact) &&
      instr->GetOperand(1)->isA(TLongExact)) {
    Register* container = instr->GetOperand(0);
    Register* index = instr->GetOperand(1);
    Register* value = instr->GetOperand(2);
    Register* adjusted_idx =
        unboxAndCheckListOrTupleIndex(env, instr, container, index);
    if (adjusted_idx == nullptr) {
      return nullptr;
    }

    Register* ob_item = env.emit<LoadField>(
        container, "ob_item", offsetof(PyListObject, ob_item), TCPtr);
    // Transfer the slot's old owned reference into HIR, overwrite the slot,
    // then keep the old object alive until the write is complete. Refcount
    // insertion will place the old-value decref after UseObj.
    Register* old_value = env.emit<LoadArrayItem>(
        ob_item, adjusted_idx, container, 0, TObject, false);
    env.emit<StoreArrayItem>(ob_item, adjusted_idx, value, container, TObject);
    env.emit<UseObj>(old_value);
    env.optimized = true;
    return nullptr;
  }

  return nullptr;
}

Register* simplifyCIntToCBool(Env& env, const CIntToCBool* instr) {
  Type input_type = instr->GetOperand(0)->type();
  if (input_type.hasIntSpec()) {
    return env.emit<LoadConst>(Type::fromCBool(input_type.intSpec()));
  }
  return nullptr;
}

Register* simplifyInstr(Env& env, const Instr* instr) {
  switch (instr->opcode()) {
    case Opcode::kCheckVar:
    case Opcode::kCheckExc:
    case Opcode::kCheckField:
      return simplifyCheck(static_cast<const CheckBase*>(instr));
    case Opcode::kCheckSequenceBounds:
      return simplifyCheckSequenceBounds(
          env, static_cast<const CheckSequenceBounds*>(instr));
    case Opcode::kGuardType:
      return simplifyGuardType(env, static_cast<const GuardType*>(instr));
    case Opcode::kRefineType:
      return simplifyRefineType(static_cast<const RefineType*>(instr));
    case Opcode::kCast:
      return simplifyCast(static_cast<const Cast*>(instr));

    case Opcode::kCompare:
      return simplifyCompare(env, static_cast<const Compare*>(instr));
#if PY_VERSION_HEX < 0x030C0000
    case Opcode::kCompareBool:
      return simplifyCompareBool(env, static_cast<const CompareBool*>(instr));
#endif
    case Opcode::kLongCompare:
      return simplifyLongCompare(env, static_cast<const LongCompare*>(instr));

    case Opcode::kCondBranch:
      return simplifyCondBranch(env, static_cast<const CondBranch*>(instr));
    case Opcode::kCondBranchCheckType:
      return simplifyCondBranchCheckType(
          env, static_cast<const CondBranchCheckType*>(instr));

    case Opcode::kGetLength:
      return simplifyGetLength(env, static_cast<const GetLength*>(instr));

    case Opcode::kPrimitiveConvert:
      return simplifyPrimitiveConvert(
          env, static_cast<const PrimitiveConvert*>(instr));

    case Opcode::kIsTruthy:
      return simplifyIsTruthy(env, static_cast<const IsTruthy*>(instr));
    case Opcode::kIsCompactLong:
      return simplifyIsCompactLong(
          env, static_cast<const IsCompactLong*>(instr));
    case Opcode::kCompactLongUnbox:
      return simplifyCompactLongUnbox(
          env, static_cast<const CompactLongUnbox*>(instr));

    case Opcode::kLoadAttr:
      // TODO(T255262756) - Enable this again. See P2169675076 and
      // P2184559031 (same pattern but applied to
      // simplifyLoadAttrTypeReceiver).
      if constexpr (kFreeThreadedBuild) {
        return nullptr;
      }
      return simplifyLoadAttr(env, static_cast<const LoadAttr*>(instr));
    case Opcode::kLoadMethod:
      // TODO(T255263721) - Enable this again. See P2169673579 and P2184559031.
      if constexpr (kFreeThreadedBuild) {
        return nullptr;
      }
      return simplifyLoadMethod(env, static_cast<const LoadMethod*>(instr));
    case Opcode::kLoadField:
      return simplifyLoadField(env, static_cast<const LoadField*>(instr));
    case Opcode::kLoadTupleItem:
      return simplifyLoadTupleItem(
          env, static_cast<const LoadTupleItem*>(instr));
    case Opcode::kLoadArrayItem:
      return simplifyLoadArrayItem(
          env, static_cast<const LoadArrayItem*>(instr));
    case Opcode::kLoadVarObjectSize:
      return simplifyLoadVarObjectSize(
          env, static_cast<const LoadVarObjectSize*>(instr));

    case Opcode::kBinaryOp:
      return simplifyBinaryOp(env, static_cast<const BinaryOp*>(instr));
    case Opcode::kInPlaceOp:
      return simplifyInPlaceOp(env, static_cast<const InPlaceOp*>(instr));
    case Opcode::kLongBinaryOp:
      return simplifyLongBinaryOp(env, static_cast<const LongBinaryOp*>(instr));
    case Opcode::kFloatBinaryOp:
      return simplifyFloatBinaryOp(
          env, static_cast<const FloatBinaryOp*>(instr));
    case Opcode::kUnaryOp:
      return simplifyUnaryOp(env, static_cast<const UnaryOp*>(instr));
    case Opcode::kIntBinaryOp:
      return simplifyIntBinaryOp(env, static_cast<const IntBinaryOp*>(instr));

    case Opcode::kPrimitiveCompare:
      return simplifyPrimitiveCompare(
          env, static_cast<const PrimitiveCompare*>(instr));
    case Opcode::kPrimitiveBox:
      return simplifyPrimitiveBox(env, static_cast<const PrimitiveBox*>(instr));
    case Opcode::kPrimitiveBoxBool:
      return simplifyPrimitiveBoxBool(
          env, static_cast<const PrimitiveBoxBool*>(instr));
    case Opcode::kIndexUnbox:
    case Opcode::kPrimitiveUnbox:
      return simplifyUnbox(env, instr);

    case Opcode::kIsNegativeAndErrOccurred:
      return simplifyIsNegativeAndErrOccurred(
          env, static_cast<const IsNegativeAndErrOccurred*>(instr));

    case Opcode::kStoreAttr:
      return simplifyStoreAttr(env, static_cast<const StoreAttr*>(instr));

    case Opcode::kCallMethod:
      return simplifyCallMethod(env, static_cast<const CallMethod*>(instr));

    case Opcode::kVectorCall:
      return simplifyVectorCall(env, static_cast<const VectorCall*>(instr));

    case Opcode::kStoreSubscr:
      return simplifyStoreSubscr(env, static_cast<const StoreSubscr*>(instr));

    case Opcode::kCIntToCBool:
      return simplifyCIntToCBool(env, static_cast<const CIntToCBool*>(instr));

    default:
      return nullptr;
  }
}

} // namespace

void Simplify::Run(Function& irfunc) {
  Env env{irfunc};

  const SimplifierConfig& config = getConfig().simplifier;
  size_t new_block_limit = config.new_block_limit;
  size_t iteration_limit = config.iteration_limit;

  // Iterate the simplifier until the CFG stops changing, or we hit limits on
  // total number of iterations or the number of new blocks added.
  bool changed = true;
  for (size_t i = 0;
       changed && i < iteration_limit && env.new_blocks < new_block_limit;
       ++i) {
    changed = false;
    // The previous iteration's cleanup passes (below) may have invalidated
    // liveness; recompute it lazily on next use this iteration.
    env.invalidateLastUses();
    for (auto& block : irfunc.cfg.blocks) {
      env.block = &block;

      for (auto blk_it = block.begin(); blk_it != block.end();) {
        Instr& instr = *blk_it;
        ++blk_it;

        env.optimized = false;
        env.cursor = block.iterator_to(instr);
        env.bc_off = instr.bytecodeOffset();
#if PY_VERSION_HEX < 0x030C0000
        const DeoptBase* orig_db = instr.asDeoptBase();
        env.exc_edge = orig_db != nullptr && orig_db->excEdge();
#endif
        Register* new_output = simplifyInstr(env, &instr);
#if PY_VERSION_HEX < 0x030C0000
        if (env.exc_edge && new_output != nullptr &&
            new_output->instr() != nullptr &&
            new_output->instr()->output() == new_output &&
            new_output->type() <= TOptObject) {
          // The replacement that produces the value is itself the raising
          // instruction now (DictSubscr, ...): it inherits the edge, and its
          // output type is refreshed at once -- the register's type was
          // computed at emission without the mark, and the RefineType on the
          // fall-through would otherwise be simplified away against that
          // stale non-null type before the next reflow.
          if (DeoptBase* new_db = new_output->instr()->asDeoptBase()) {
            new_db->setExcEdge(true);
            new_output->set_type(outputType(*new_db));
          }
        }
        env.exc_edge = false;
#endif
        JIT_CHECK(
            env.cursor == env.block->iterator_to(instr),
            "Simplify functions are expected to leave env.cursor pointing to "
            "the original instruction, with new instructions inserted before "
            "it.");
        if (new_output == nullptr && !env.optimized) {
          continue;
        }

        changed = true;
        JIT_CHECK(
            (new_output == nullptr) == (instr.output() == nullptr),
            "Simplify function should return a new output if and only if the "
            "existing instruction has an output");
        if (new_output != nullptr) {
          JIT_CHECK(
              new_output->type() <= instr.output()->type(),
              "New output type {} isn't compatible with old output type {}",
              new_output->type(),
              instr.output()->type());
          env.emitRawInstr<Assign>(instr.output(), new_output);
        }

        if (instr.IsCondBranch() || instr.IsCondBranchIterNotDone() ||
            instr.IsCondBranchCheckType()) {
          JIT_CHECK(env.cursor != env.block->begin(), "Unexpected empty block");
          Instr& prev_instr = *std::prev(env.cursor);
          JIT_CHECK(
              instr.opcode() == prev_instr.opcode() || prev_instr.IsBranch(),
              "The only supported simplification for CondBranch* is to a "
              "Branch or a different CondBranch, got unexpected '{}'",
              prev_instr);

          // If we've optimized a CondBranchBase into a Branch, we also need to
          // remove any Phi references to the current block from the block that
          // we no longer visit.
          if (prev_instr.IsBranch()) {
            auto cond = static_cast<CondBranchBase*>(&instr);
            BasicBlock* new_dst = prev_instr.successor(0);
            BasicBlock* old_branch_block = cond->false_bb() == new_dst
                ? cond->true_bb()
                : cond->false_bb();
            old_branch_block->removePhiPredecessor(cond->block());
          }
        }

        instr.unlink();
        delete &instr;

        if (env.block != &block) {
          // If we're now in a different block, `block' should only contain the
          // newly-emitted instructions, with no more old instructions to
          // process. Continue to the next block in the list; any newly-created
          // blocks were added to the end of the list and will be processed
          // later.
          break;
        }
      }

      // Check for going past the new block limit only upon leaving a block.  We
      // might go past the limit, but not by too much.
      if (env.new_blocks > new_block_limit) {
        break;
      }
    }

    if (changed) {
      // Perform some simple cleanup between each pass. Folding a CondBranch
      // into a Branch above can leave unreachable blocks whose instructions
      // still use registers defined by reachable code. Remove those blocks
      // before CopyPropagation deletes Assigns, or the stale uses would keep
      // pointing at freed instructions.
      removeUnreachableBlocks(irfunc);
      CopyPropagation{}.Run(irfunc);
      reflowTypes(irfunc);
      CleanCFG{}.Run(irfunc);
    }
  }
}

} // namespace jit::hir
