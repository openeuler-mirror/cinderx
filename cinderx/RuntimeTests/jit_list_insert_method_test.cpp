// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/lir/generator.h"
#include "cinderx/RuntimeTests/fixtures.h"

namespace {

size_t countHIRCallsTo(const jit::hir::Function& func, void* target) {
  size_t count = 0;
  for (const auto& block : func.cfg.blocks) {
    for (const auto& instr : block) {
      if (instr.IsCallStatic() &&
          static_cast<const jit::hir::CallStatic&>(instr).addr() == target) {
        count++;
      }
    }
  }
  return count;
}

size_t countHIROpcode(const jit::hir::Function& func, jit::hir::Opcode opcode) {
  size_t count = 0;
  for (const auto& block : func.cfg.blocks) {
    for (const auto& instr : block) {
      count += instr.opcode() == opcode;
    }
  }
  return count;
}

const jit::hir::CallStatic* findHIRCallTo(
    const jit::hir::Function& func,
    void* target) {
  for (const auto& block : func.cfg.blocks) {
    for (const auto& instr : block) {
      if (instr.IsCallStatic()) {
        const auto& call = static_cast<const jit::hir::CallStatic&>(instr);
        if (call.addr() == target) {
          return &call;
        }
      }
    }
  }
  return nullptr;
}

size_t countLIRCallsTo(const jit::lir::Function& func, void* target) {
  size_t count = 0;
  auto address = reinterpret_cast<uint64_t>(target);
  for (const auto* block : func.basicblocks()) {
    for (const auto& instr : block->instructions()) {
      if (instr->isCall() && instr->getNumInputs() > 0 &&
          instr->getInput(0)->isImm() &&
          instr->getInput(0)->getConstant() == address) {
        count++;
      }
    }
  }
  return count;
}

} // namespace

class ListInsertMethodTest : public RuntimeTest {
 protected:
  std::unique_ptr<jit::hir::Function> optimizedHIR(
      BorrowedRef<PyFunctionObject> func) {
    auto irfunc = buildHIR(func);
    jit::Compiler::runPasses(*irfunc, jit::PassConfig::kAllExceptInliner);
    return irfunc;
  }

  size_t countLoweredCallsTo(BorrowedRef<PyFunctionObject> func, void* target) {
    auto irfunc = optimizedHIR(func);

    jit::codegen::Environ env;
    env.ctx = jit::getContext();

    jit::CodeRuntime runtime{func};
#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
    // Frame reifiers exist only on the lightweight-frames runtime; the 3.11
    // materialized-frame build has no reifier to attach.
    Ref<> reifier;
    if (irfunc->reifier != nullptr) {
      runtime.setReifier(irfunc->reifier);
    } else {
      reifier = jit::makeFrameReifier(func->func_code);
      runtime.setReifier(reifier);
    }
#endif
    env.code_rt = &runtime;

    jit::lir::LIRGenerator lir_gen(irfunc.get(), &env);
    auto lir_func = lir_gen.TranslateFunction();
    return countLIRCallsTo(*lir_func, target);
  }
};

TEST_F(ListInsertMethodTest, ConstantIndexSkipsConversionInHIRAndLIR) {
  // The specialized insert path leans on the cached-attribute machinery;
  // the 3.11 default keeps attribute caches off until MR-09, so enable
  // them for this test's scope.
  bool old_attr_caches = jit::getConfig().attr_caches;
  jit::getMutableConfig().attr_caches = true;
  SCOPE_EXIT(jit::getMutableConfig().attr_caches = old_attr_caches);
  runCode(R"(
import cinderx.jit as jit

jit.enable_specialized_opcodes()

def insert_constant_zero(item):
    values = []
    insert = values.insert
    insert(0, item)
    return values

def insert_dynamic(values, index, item):
    insert = values.insert
    insert(index, item)

def insert_overflow(item):
    values = []
    insert = values.insert
    insert(10**100, item)
    return values

jit.jit_suppress(insert_constant_zero)
jit.jit_suppress(insert_dynamic)
jit.jit_suppress(insert_overflow)
try:
    for i in range(100):
        assert insert_constant_zero(i) == [i]
        values = []
        insert_dynamic(values, 0, i)
        assert values == [i]
        try:
            overflow_values = insert_overflow(i)
        except OverflowError:
            pass
        else:
            assert overflow_values == [i]
finally:
    jit.jit_unsuppress(insert_constant_zero)
    jit.jit_unsuppress(insert_dynamic)
    jit.jit_unsuppress(insert_overflow)
)");

  Ref<PyFunctionObject> constant_func(getGlobal("insert_constant_zero"));
  Ref<PyFunctionObject> dynamic_func(getGlobal("insert_dynamic"));
  Ref<PyFunctionObject> overflow_func(getGlobal("insert_overflow"));
  ASSERT_NE(constant_func.get(), nullptr);
  ASSERT_NE(dynamic_func.get(), nullptr);
  ASSERT_NE(overflow_func.get(), nullptr);

  auto constant_hir = optimizedHIR(constant_func);
  ASSERT_NE(constant_hir, nullptr);
  EXPECT_EQ(
      countHIRCallsTo(*constant_hir, reinterpret_cast<void*>(PyLong_AsSsize_t)),
      0);
  EXPECT_EQ(
      countHIRCallsTo(*constant_hir, reinterpret_cast<void*>(PyList_Insert)),
      1);

  const auto* constant_insert =
      findHIRCallTo(*constant_hir, reinterpret_cast<void*>(PyList_Insert));
  ASSERT_NE(constant_insert, nullptr);
  ASSERT_EQ(constant_insert->NumArgs(), 3);
  auto* constant_index_instr = constant_insert->arg(1)->instr();
  ASSERT_NE(constant_index_instr, nullptr);
  ASSERT_TRUE(constant_index_instr->IsLoadConst());
  const auto& constant_index =
      static_cast<const jit::hir::LoadConst&>(*constant_index_instr);
  EXPECT_TRUE(constant_index.type() <= jit::hir::TCInt64);
  ASSERT_TRUE(constant_index.type().hasIntSpec());
  EXPECT_EQ(constant_index.type().intSpec(), 0);

  auto dynamic_hir = optimizedHIR(dynamic_func);
  ASSERT_NE(dynamic_hir, nullptr);
  size_t dynamic_hir_conversion =
      countHIRCallsTo(*dynamic_hir, reinterpret_cast<void*>(PyLong_AsSsize_t));
  size_t dynamic_hir_insert =
      countHIRCallsTo(*dynamic_hir, reinterpret_cast<void*>(PyList_Insert));
  EXPECT_EQ(dynamic_hir_conversion, dynamic_hir_insert);
  EXPECT_LE(dynamic_hir_insert, 1);
  if (dynamic_hir_insert == 0) {
    EXPECT_GE(countHIROpcode(*dynamic_hir, jit::hir::Opcode::kVectorCall), 1);
  }

  auto overflow_hir = optimizedHIR(overflow_func);
  ASSERT_NE(overflow_hir, nullptr);
  EXPECT_EQ(
      countHIRCallsTo(*overflow_hir, reinterpret_cast<void*>(PyLong_AsSsize_t)),
      1);
  EXPECT_EQ(
      countHIRCallsTo(*overflow_hir, reinterpret_cast<void*>(PyList_Insert)),
      1);

  EXPECT_EQ(
      countLoweredCallsTo(
          constant_func, reinterpret_cast<void*>(PyLong_AsSsize_t)),
      0);
  EXPECT_EQ(
      countLoweredCallsTo(
          constant_func, reinterpret_cast<void*>(PyList_Insert)),
      1);
  size_t dynamic_lir_conversion = countLoweredCallsTo(
      dynamic_func, reinterpret_cast<void*>(PyLong_AsSsize_t));
  size_t dynamic_lir_insert =
      countLoweredCallsTo(dynamic_func, reinterpret_cast<void*>(PyList_Insert));
  EXPECT_EQ(dynamic_lir_conversion, dynamic_lir_insert);
  EXPECT_LE(dynamic_lir_insert, 1);
  EXPECT_EQ(
      countLoweredCallsTo(
          overflow_func, reinterpret_cast<void*>(PyLong_AsSsize_t)),
      1);
  EXPECT_EQ(
      countLoweredCallsTo(
          overflow_func, reinterpret_cast<void*>(PyList_Insert)),
      1);
}

TEST_F(ListInsertMethodTest, BoundListInsertEmitsDirectCall) {
  runCode(R"(
import cinderx.jit as jit

jit.enable_specialized_opcodes()

def insert_local(item):
    values = [1, 2, 3]
    insert = values.insert
    insert(1, item)
    return values

jit.force_uncompile(insert_local)
jit.jit_suppress(insert_local)
try:
    for i in range(20):
        insert_local(i)
finally:
    jit.jit_unsuppress(insert_local)

assert jit.force_compile(insert_local)
counts = jit.get_function_hir_opcode_counts(insert_local)
assert counts.get("CallStatic", 0) == 1, counts
assert counts.get("VectorCall", 0) == 0, counts
assert insert_local("x") == [1, "x", 2, 3]
)");
}

TEST_F(ListInsertMethodTest, BoundListInsertOverflowMatchesPython) {
  runCode(R"(
import cinderx.jit as jit

jit.enable_specialized_opcodes()

def exception_info(func, *args):
    try:
        func(*args)
    except Exception as exc:
        return type(exc), str(exc)
    return None, None

def generic_insert_huge(item):
    values = []
    values.insert(10**100, item)
    return values

def insert_huge(item):
    values = []
    insert = values.insert
    insert(10**100, item)
    return values

jit.jit_suppress(generic_insert_huge)
try:
    expected = exception_info(generic_insert_huge, 1)
finally:
    jit.jit_unsuppress(generic_insert_huge)

jit.force_uncompile(insert_huge)
jit.jit_suppress(insert_huge)
try:
    for _ in range(20):
        exception_info(insert_huge, 1)
finally:
    jit.jit_unsuppress(insert_huge)

assert jit.force_compile(insert_huge)
counts = jit.get_function_hir_opcode_counts(insert_huge)
assert counts.get("CallStatic", 0) == 2, counts
assert counts.get("VectorCall", 0) == 0, counts
assert exception_info(insert_huge, 1) == expected
)");
}

TEST_F(ListInsertMethodTest, BoundListInsertPreservesReferenceCounts) {
  runCode(R"(
import sys

import cinderx.jit as jit

jit.enable_specialized_opcodes()

def insert_local(values, item):
    insert = values.insert
    insert(0, item)

values = []

class Payload:
    pass

payload = Payload()

def run_once():
    insert_local(values, payload)
    assert values.pop(0) is payload

jit.force_uncompile(insert_local)
jit.jit_suppress(insert_local)
try:
    for _ in range(20):
        run_once()
finally:
    jit.jit_unsuppress(insert_local)

assert jit.force_compile(insert_local)
counts = jit.get_function_hir_opcode_counts(insert_local)
if counts.get("CallStatic", 0):
    assert counts.get("CallStatic", 0) == 1, counts
    assert counts.get("VectorCall", 0) == 0, counts
else:
    # Function arguments may remain untyped in HIR. Keeping VectorCall is the
    # required safe fallback when an exact-list receiver is not proven.
    assert counts.get("VectorCall", 0) >= 1, counts

receiver_refs = sys.getrefcount(values)
value_refs = sys.getrefcount(payload)
for _ in range(200):
    run_once()
assert sys.getrefcount(values) == receiver_refs
assert sys.getrefcount(payload) == value_refs
)");
}

TEST_F(ListInsertMethodTest, BoundListInsertExactGuardsFallBack) {
  runCode(R"(
import cinderx.jit as jit

jit.enable_specialized_opcodes()

def compile_func(func, runner):
    jit.force_uncompile(func)
    jit.jit_suppress(func)
    try:
        for _ in range(20):
            runner()
    finally:
        jit.jit_unsuppress(func)
    assert jit.force_compile(func)

def insert_bool(item):
    values = [1, 2]
    insert = values.insert
    insert(True, item)
    return values

compile_func(insert_bool, lambda: insert_bool(0))
counts = jit.get_function_hir_opcode_counts(insert_bool)
assert counts.get("VectorCall", 0) >= 1, counts
assert insert_bool("x") == [1, "x", 2]

class IntSubclass(int):
    pass

def insert_int_subclass(index, item):
    values = [1, 2]
    insert = values.insert
    insert(index, item)
    return values

index = IntSubclass(0)
compile_func(
    insert_int_subclass,
    lambda: insert_int_subclass(index, 0),
)
counts = jit.get_function_hir_opcode_counts(insert_int_subclass)
assert counts.get("VectorCall", 0) >= 1, counts
assert insert_int_subclass(index, "x") == ["x", 1, 2]

class ListSubclass(list):
    def __init__(self):
        super().__init__([1, 2])
        self.insert_calls = []

    def insert(self, index, item):
        self.insert_calls.append((index, item))
        return "overridden"

def insert_subclass(values, index, item):
    insert = values.insert
    return insert(index, item)

values = ListSubclass()
compile_func(
    insert_subclass,
    lambda: insert_subclass(values, 0, "warmup"),
)
counts = jit.get_function_hir_opcode_counts(insert_subclass)
assert counts.get("VectorCall", 0) >= 1, counts
assert insert_subclass(values, 1, "x") == "overridden"
assert values.insert_calls[-1] == (1, "x")
assert values == [1, 2]
)");
}
