// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/codegen/gen_asm.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/trigger_stats.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/module_state.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace asmjit;
using namespace jit;
using namespace jit::codegen;
using namespace jit::hir;

class ASMGeneratorTest : public RuntimeTest {
 public:
  Ref<CompiledFunction> GenerateCode(PyObject* func) {
    auto func_obj = reinterpret_cast<PyFunctionObject*>(func);

    CompilationKey key{
        func_obj->func_code, func_obj->func_builtins, func_obj->func_globals};
    auto jit_ctx = reinterpret_cast<jit::CompilerContext<Compiler>*>(
        cinderx::getModuleState()->jit_context.get());
    std::optional<CompiledFunctionData> compiled_func =
        jit_ctx->compiler().Compile(func);
    if (!compiled_func.has_value()) {
      return nullptr;
    }
    return jit_ctx->makeCompiledFunction(
        func_obj, key, std::move(*compiled_func));
  }
};

#if PY_VERSION_HEX < 0x030C0000 && defined(CINDER_AARCH64)
TEST_F(ASMGeneratorTest, BindingEntryTailStubsPreserveCallSlots311) {
  SKIP_311_EXECUTABLE_COMPILE();
  auto check_binding_tail = [&](const CompiledFunction& artifact,
                                std::initializer_list<uint64_t> helpers,
                                bool checks_argcount) {
    auto bytes = artifact.codeBuffer();
    auto read = [&](size_t offset) {
      uint32_t instruction;
      std::memcpy(&instruction, bytes.data() + offset, sizeof(instruction));
      return instruction;
    };
    size_t code_end = bytes.size();
    for (size_t offset = 0; offset + 4 <= code_end; offset += 4) {
      const uint32_t instruction = read(offset);
      if ((instruction & 0xff000000u) == 0x58000000u) {
        int32_t immediate = (instruction >> 5) & 0x7ffff;
        if (immediate & 0x40000) {
          immediate -= 0x80000;
        }
        const int64_t literal =
            static_cast<int64_t>(offset) + static_cast<int64_t>(immediate) * 4;
        if (literal > static_cast<int64_t>(offset) &&
            static_cast<uint64_t>(literal) <= bytes.size() - 8) {
          code_end = std::min(code_end, static_cast<size_t>(literal));
        }
      }
    }
    auto pointer_move = [&](size_t& cursor, unsigned reg, uint64_t& value) {
      if (cursor + 4 > code_end) {
        return false;
      }
      uint32_t instruction = read(cursor);
      const uint32_t opcode = instruction & 0x7f800000u;
      if ((instruction & 31u) != reg ||
          (opcode != 0x52800000u && opcode != 0x12800000u)) {
        return false; // MOVZ/MOVN starts the pointer materialization.
      }
      bool wide = (instruction & 0x80000000u) != 0;
      const unsigned shift = ((instruction >> 21) & 3u) * 16;
      if (!wide && shift >= 32) {
        return false;
      }
      value = static_cast<uint64_t>((instruction >> 5) & 0xffffu) << shift;
      if (opcode == 0x12800000u) {
        value = ~value;
      }
      value &= wide ? ~uint64_t{0} : uint64_t{0xffffffff};
      cursor += 4;
      while (cursor + 4 <= code_end) {
        instruction = read(cursor);
        if ((instruction & 0x7f800000u) != 0x72800000u ||
            (instruction & 31u) != reg) {
          break;
        }
        wide = (instruction & 0x80000000u) != 0;
        const unsigned part_shift = ((instruction >> 21) & 3u) * 16;
        if (!wide && part_shift >= 32) {
          return false;
        }
        value = (value & ~(uint64_t{0xffff} << part_shift)) |
            (static_cast<uint64_t>((instruction >> 5) & 0xffffu) << part_shift);
        // AsmJit encodeMovSequence32 can emit MOVZ Xreg + MOVK Wreg for
        // low-address pointers. Every W write zero-extends the full Xreg;
        // a later MOVK Xreg may then populate its upper halfwords again.
        value &= wide ? ~uint64_t{0} : uint64_t{0xffffffff};
        cursor += 4;
      }
      return true;
    };
    std::vector<std::pair<uint64_t, size_t>> calls;
    for (size_t offset = 0; offset + 4 <= code_end; offset += 4) {
      size_t cursor = offset;
      uint64_t runtime = 0;
      uint64_t helper = 0;
      if (!pointer_move(cursor, 4, runtime) ||
          !pointer_move(cursor, 16, helper) || cursor + 4 > code_end ||
          read(cursor) != 0xd61f0200u || // BR X16: no extra frame/LR write.
          std::find(helpers.begin(), helpers.end(), helper) == helpers.end()) {
        continue;
      }
      EXPECT_EQ(runtime, reinterpret_cast<uint64_t>(artifact.runtime()));
      size_t references = 0;
      size_t caller = bytes.size();
      for (size_t pc = 0; pc + 12 <= offset; pc += 4) {
        const uint32_t instruction = read(pc);
        if ((instruction & 0xfc000000u) != 0x94000000u) {
          continue;
        }
        int64_t immediate = instruction & 0x03ffffffu;
        if (immediate & 0x02000000) {
          immediate -= 0x04000000;
        }
        if (static_cast<int64_t>(pc) + immediate * 4 !=
            static_cast<int64_t>(offset)) {
          continue;
        }
        ++references;
        caller = pc;
        EXPECT_EQ(read(pc + 4), 0xd503201fu)
            << "local BL must keep its NOP slot";
        EXPECT_EQ(read(pc + 8) & 0xfc000000u, 0x14000000u)
            << "the eight-byte call must be followed by the prologue-exit jump";
      }
      EXPECT_EQ(references, 1u);
      calls.emplace_back(helper, caller);
    }
    ASSERT_EQ(calls.size(), helpers.size());
    for (uint64_t helper : helpers) {
      EXPECT_EQ(
          std::count_if(
              calls.begin(),
              calls.end(),
              [&](const auto& call) { return call.first == helper; }),
          1);
    }
    if (checks_argcount) {
      ASSERT_EQ(calls.size(), 2u);
      std::sort(calls.begin(), calls.end(), [](const auto& a, const auto& b) {
        return a.second < b.second;
      });
      const size_t wrong_count = calls[1].second;
      // Two BL+NOP+exit-jump slots separated by MOV argc/CMP/B.EQ. No x4
      // materialization remains before the body, preserving the old 24B gap.
      EXPECT_EQ(wrong_count - calls[0].second, 24u);
      ASSERT_GE(wrong_count, 4u);
      const uint32_t branch = read(wrong_count - 4);
      ASSERT_EQ(branch & 0xff00001fu, 0x54000000u); // B.EQ
      int32_t immediate = (branch >> 5) & 0x7ffff;
      if (immediate & 0x40000) {
        immediate -= 0x80000;
      }
      EXPECT_EQ(
          static_cast<int64_t>(wrong_count - 4) +
              static_cast<int64_t>(immediate) * 4,
          static_cast<int64_t>(wrong_count + 12));
    }
  };
  const char* binding_source = R"(
def binding_simple(first, second=9):
    return first + second
def binding_general(first, *, second=9):
    return first + second
def binding_variadic(first, *rest, **kwargs):
    return first + len(rest) + len(kwargs)
)";
  Ref<PyObject> simple(compileAndGet(binding_source, "binding_simple"));
  ASSERT_NE(simple, nullptr);
  auto simple_code = GenerateCode(simple);
  ASSERT_NE(simple_code, nullptr);
  check_binding_tail(
      *simple_code.get(),
      {reinterpret_cast<uint64_t>(JITRT_CallWithKeywordArgsSimpleAtEntry311),
       reinterpret_cast<uint64_t>(JITRT_CallWithIncorrectArgcountAtEntry311)},
      true);
  auto four = Ref<>::steal(PyLong_FromLong(4));
  auto five = Ref<>::steal(PyLong_FromLong(5));
  ASSERT_NE(four, nullptr);
  ASSERT_NE(five, nullptr);
  PyObject* default_args[] = {four};
  auto entry_before = jit::triggerStatsSnapshot().machine_code_entries;
  auto default_result =
      Ref<>::steal(simple_code->invoke(simple, default_args, 1));
  ASSERT_NE(default_result, nullptr);
  EXPECT_EQ(PyLong_AsLong(default_result), 13);
  EXPECT_GT(jit::triggerStatsSnapshot().machine_code_entries, entry_before);
  auto second_name = Ref<>::steal(PyUnicode_FromString("second"));
  ASSERT_NE(second_name, nullptr);
  auto names = Ref<>::steal(PyTuple_Pack(1, second_name.get()));
  ASSERT_NE(names, nullptr);
  PyObject* keyword_args[] = {four, five};
  entry_before = jit::triggerStatsSnapshot().machine_code_entries;
  auto keyword_result = Ref<>::steal(
      simple_code->vectorcallEntry()(simple, keyword_args, 1, names));
  ASSERT_NE(keyword_result, nullptr);
  EXPECT_EQ(PyLong_AsLong(keyword_result), 9);
  EXPECT_GT(jit::triggerStatsSnapshot().machine_code_entries, entry_before);
  for (const char* name : {"binding_general", "binding_variadic"}) {
    Ref<PyObject> owner(getGlobal(name));
    auto artifact = GenerateCode(owner);
    ASSERT_NE(artifact, nullptr);
    check_binding_tail(
        *artifact.get(),
        {reinterpret_cast<uint64_t>(JITRT_CallWithKeywordArgsAtEntry311)},
        false);
    entry_before = jit::triggerStatsSnapshot().machine_code_entries;
    auto answer = Ref<>::steal(artifact->invoke(owner, default_args, 1));
    ASSERT_NE(answer, nullptr);
    EXPECT_EQ(
        PyLong_AsLong(answer),
        std::strcmp(name, "binding_general") == 0 ? 13 : 4);
    EXPECT_GT(jit::triggerStatsSnapshot().machine_code_entries, entry_before);
  }
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(ASMGeneratorTest, Aarch64PointerLiteralPoolUsesNaturalAlignment) {
  SKIP_311_EXECUTABLE_COMPILE();
  const auto saved = getConfig();
  SCOPE_EXIT(getMutableConfig() = saved);
  // These functions use the production contiguous text layout. Split sections
  // have their own allocation/section-offset contract and are not needed to
  // reproduce an odd instruction count before this shared literal pool.
  getMutableConfig().multiple_code_sections = false;
  size_t total_literal_sites = 0;
  auto check_literals = [&](const CompiledFunction& compiled) {
    auto code = compiled.codeBuffer();
    ASSERT_GE(code.size(), sizeof(uint64_t));
    auto read_word = [&](size_t offset) {
      uint32_t word;
      std::memcpy(&word, code.data() + offset, sizeof(word));
      return word;
    };
    size_t instruction_end = code.size();
    size_t literal_sites = 0;
    size_t unaligned_sites = 0;
    size_t first_unaligned = code.size();
    for (size_t offset = 0; offset + sizeof(uint32_t) <= instruction_end;
         offset += sizeof(uint32_t)) {
      const uint32_t word = read_word(offset);
      // LDR Xt, label: opc=01, V=0, fixed 011000, signed imm19 at 23:5.
      // Its PC is this instruction's address, and the displacement is imm19*4.
      if ((word & 0xff000000u) != 0x58000000u) {
        continue;
      }
      int32_t immediate = (word >> 5) & 0x7ffff;
      if (immediate & 0x40000) {
        immediate -= 0x80000;
      }
      const int64_t target =
          static_cast<int64_t>(offset) + static_cast<int64_t>(immediate) * 4;
      if (target < 0 ||
          static_cast<uint64_t>(target) > code.size() - sizeof(uint64_t)) {
        continue; // A literal outside this artifact is not its local pool.
      }
      ASSERT_GT(target, static_cast<int64_t>(offset));
      // All env.const_pool_labels entries are referenced by LDR X and emitted
      // after executable code. The smallest forward target is therefore the
      // data boundary. Stop there rather than decode pointer data as opcodes.
      instruction_end = std::min(instruction_end, static_cast<size_t>(target));
      ++literal_sites;
      const uintptr_t address = reinterpret_cast<uintptr_t>(code.data()) +
          static_cast<uintptr_t>(target);
      if (address % alignof(uint64_t) != 0) {
        ++unaligned_sites;
        first_unaligned =
            std::min(first_unaligned, static_cast<size_t>(target));
      }
    }
    EXPECT_GT(literal_sites, 0u);
    EXPECT_EQ(unaligned_sites, 0u)
        << "code size=" << code.size() << " pool offset=" << instruction_end
        << " first unaligned literal=" << first_unaligned;
    total_literal_sites += literal_sites;
  };

  runCode("def literal_supplier(): return 17\n");
  const char* sources[] = {
      "def literal_owner(callee, value): return callee()\n",
      "def literal_owner(callee, value): return callee(value)\n",
      "def literal_owner(callee, value): return callee(value, 3)\n",
      "def literal_owner(value, fallback): return value.get('aligned', "
      "fallback)\n",
  };
  auto seventeen = Ref<>::steal(PyLong_FromLong(17));
  auto negative = Ref<>::steal(PyLong_FromLong(-17));
  auto mapping = Ref<>::steal(PyDict_New());
  ASSERT_NE(seventeen, nullptr);
  ASSERT_NE(negative, nullptr);
  ASSERT_NE(mapping, nullptr);
  ASSERT_EQ(PyDict_SetItemString(mapping, "aligned", seventeen), 0);
  for (size_t i = 0; i < 4; ++i) {
    SCOPED_TRACE(i);
    Ref<PyObject> owner(compileAndGet(sources[i], "literal_owner"));
    ASSERT_NE(owner, nullptr);
    auto compiled = GenerateCode(owner);
    ASSERT_NE(compiled, nullptr);
    check_literals(*compiled.get());
    // Dynamic ordinary callees emit the non-function dispatch stub even when
    // a particular invocation supplies a Python function or a static type.
    Ref<> callee = i == 0 ? getGlobal("literal_supplier")
        : i == 1
        ? Ref<>::create(PyDict_GetItemString(PyEval_GetBuiltins(), "abs"))
        : Ref<>::create(PyDict_GetItemString(PyEval_GetBuiltins(), "max"));
    ASSERT_NE(callee, nullptr);
    PyObject* args[] = {
        i == 3 ? mapping.get() : callee.get(),
        i == 1 ? negative.get() : seventeen.get()};
    const auto entries_before =
        jit::triggerStatsSnapshot().machine_code_entries;
    auto result = Ref<>::steal(compiled->invoke(owner, args, 2));
    ASSERT_NE(result, nullptr);
    EXPECT_GT(jit::triggerStatsSnapshot().machine_code_entries, entries_before);
    EXPECT_EQ(PyLong_AsLong(result), 17);
    EXPECT_EQ(PyErr_Occurred(), nullptr);
  }

  const char* sparse_source = R"(
from array import array
y = array('d', [0.0, 0.0])
val = array('d', [2.0, 3.0, 4.0])
row = array('i', [0, 2, 3])
col = array('i', [0, 1, 0])
x = array('d', [5.0, 7.0])
def sparse_literal_owner(M, y, val, row, col, x, num_iterations):
    for _ in range(num_iterations):
        for r in range(M):
            total = 0.0
            for i in range(row[r], row[r + 1]):
                total += x[col[i]] * val[i]
            y[r] = total
    return y
)";
  Ref<PyObject> sparse(compileAndGet(sparse_source, "sparse_literal_owner"));
  ASSERT_NE(sparse, nullptr);
  auto compiled_sparse = GenerateCode(sparse);
  ASSERT_NE(compiled_sparse, nullptr);
  check_literals(*compiled_sparse.get());
  auto two = Ref<>::steal(PyLong_FromLong(2));
  auto y = getGlobal("y");
  auto values = getGlobal("val");
  auto rows = getGlobal("row");
  auto columns = getGlobal("col");
  auto x = getGlobal("x");
  PyObject* sparse_args[] = {two, y, values, rows, columns, x, two};
  const auto entries_before = jit::triggerStatsSnapshot().machine_code_entries;
  auto result = Ref<>::steal(compiled_sparse->invoke(sparse, sparse_args, 7));
  ASSERT_NE(result, nullptr);
  EXPECT_GT(jit::triggerStatsSnapshot().machine_code_entries, entries_before);
  EXPECT_EQ(result.get(), y.get());
  auto first = Ref<>::steal(PySequence_GetItem(result, 0));
  auto second = Ref<>::steal(PySequence_GetItem(result, 1));
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(PyFloat_AsDouble(first), 31.0);
  EXPECT_EQ(PyFloat_AsDouble(second), 20.0);
  EXPECT_GE(total_literal_sites, 12u);
  RecordProperty(
      "literal_sites_checked", static_cast<int>(total_literal_sites));
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}
#endif // PY_VERSION_HEX < 0x030C0000 && defined(CINDER_AARCH64)

#if PY_VERSION_HEX < 0x030C0000
class ShadowCompileTest : public RuntimeTest {};

TEST_F(ShadowCompileTest, ShadowCompileDoesNotAllocateOrInstallCode) {
  const char* pycode = R"(
def func(value):
  return value + 1
)";

  Ref<PyFunctionObject> pyfunc(compileAndGet(pycode, "func"));
  ASSERT_NE(pyfunc, nullptr);

  auto* module_state = cinderx::getModuleState();
  ASSERT_NE(module_state, nullptr);
  ASSERT_NE(module_state->code_allocator, nullptr);
  auto* jit_ctx = reinterpret_cast<jit::CompilerContext<Compiler>*>(
      module_state->jit_context.get());
  ASSERT_NE(jit_ctx, nullptr);

  size_t used_before = module_state->code_allocator->usedBytes();
  Py_ssize_t code_refcount_before = Py_REFCNT(pyfunc->func_code);
  Py_ssize_t globals_refcount_before = Py_REFCNT(pyfunc->func_globals);
  Py_ssize_t builtins_refcount_before = Py_REFCNT(pyfunc->func_builtins);
  jit::TriggerStats trigger_before = jit::triggerStatsSnapshot();
  auto result = jit_ctx->compiler().CompileShadow(pyfunc);
  jit::TriggerStats trigger_after = jit::triggerStatsSnapshot();

  ASSERT_TRUE(result.has_value());
  EXPECT_GT(result->code_size, 0u);
  EXPECT_EQ(module_state->code_allocator->usedBytes(), used_before);
  EXPECT_EQ(
      trigger_after.executable_alloc_calls,
      trigger_before.executable_alloc_calls);
  EXPECT_EQ(
      trigger_after.executable_alloc_bytes,
      trigger_before.executable_alloc_bytes);
  EXPECT_EQ(
      trigger_after.compiled_function_creations,
      trigger_before.compiled_function_creations);
  EXPECT_EQ(
      trigger_after.machine_code_entries, trigger_before.machine_code_entries);
  EXPECT_EQ(jit_ctx->lookupFunc(pyfunc), nullptr);
  EXPECT_EQ(Py_REFCNT(pyfunc->func_code), code_refcount_before);
  EXPECT_EQ(Py_REFCNT(pyfunc->func_globals), globals_refcount_before);
  EXPECT_EQ(Py_REFCNT(pyfunc->func_builtins), builtins_refcount_before);
}

TEST_F(ShadowCompileTest, ShadowCompileUsesEphemeralInlineCaches) {
  const char* pycode = R"(
def func(value):
  return value.member
)";

  Ref<PyFunctionObject> pyfunc(compileAndGet(pycode, "func"));
  ASSERT_NE(pyfunc, nullptr);

  auto* module_state = cinderx::getModuleState();
  ASSERT_NE(module_state, nullptr);
  auto* jit_ctx = reinterpret_cast<jit::CompilerContext<Compiler>*>(
      module_state->jit_context.get());
  ASSERT_NE(jit_ctx, nullptr);

  size_t used_before = module_state->code_allocator->usedBytes();
  Py_ssize_t code_refcount_before = Py_REFCNT(pyfunc->func_code);
  Py_ssize_t globals_refcount_before = Py_REFCNT(pyfunc->func_globals);
  Py_ssize_t builtins_refcount_before = Py_REFCNT(pyfunc->func_builtins);

  for (int i = 0; i < 32; i++) {
    auto result = jit_ctx->compiler().CompileShadow(pyfunc);
    ASSERT_TRUE(result.has_value()) << i;
    EXPECT_GT(result->code_size, 0u) << i;
  }

  EXPECT_EQ(module_state->code_allocator->usedBytes(), used_before);
  EXPECT_EQ(jit_ctx->lookupFunc(pyfunc), nullptr);
  EXPECT_EQ(Py_REFCNT(pyfunc->func_code), code_refcount_before);
  EXPECT_EQ(Py_REFCNT(pyfunc->func_globals), globals_refcount_before);
  EXPECT_EQ(Py_REFCNT(pyfunc->func_builtins), builtins_refcount_before);
}

TEST_F(ShadowCompileTest, TryLoopReturningHandlerPassesShadowVerifier) {
  const char* pycode = R"(
class Done(Exception):
  pass

def func(limit):
  try:
    i = 0
    while True:
      i += 1
      if i >= limit:
        raise Done(i)
  except Done as exc:
    return exc.args[0]
)";

  Ref<PyFunctionObject> pyfunc(compileAndGet(pycode, "func"));
  ASSERT_NE(pyfunc, nullptr);
  EXPECT_EQ(unsupportedShapeReason311(pyfunc->func_code), nullptr);

  auto* jit_ctx = reinterpret_cast<jit::CompilerContext<Compiler>*>(
      cinderx::getModuleState()->jit_context.get());
  ASSERT_NE(jit_ctx, nullptr);

  auto result = jit_ctx->compiler().CompileShadow(pyfunc);
  ASSERT_TRUE(result.has_value());
  EXPECT_GT(result->code_size, 0u);
  EXPECT_EQ(jit_ctx->lookupFunc(pyfunc), nullptr);
}

TEST_F(ShadowCompileTest, UnboundLocalCasesPassShadowVerifier) {
  const char* pycode = R"(
def read_before_assign():
  if False:
    x = 1
  return x

def annotation_only_then_read():
  x: int
  return x

def walrus_dead_branch():
  if False:
    y = (w := 10)
  return w
)";

  auto* jit_ctx = reinterpret_cast<jit::CompilerContext<Compiler>*>(
      cinderx::getModuleState()->jit_context.get());
  ASSERT_NE(jit_ctx, nullptr);

  for (const char* name :
       {"read_before_assign",
        "annotation_only_then_read",
        "walrus_dead_branch"}) {
    Ref<PyFunctionObject> pyfunc(compileAndGet(pycode, name));
    ASSERT_NE(pyfunc, nullptr) << name;
    auto result = jit_ctx->compiler().CompileShadow(pyfunc);
    ASSERT_TRUE(result.has_value()) << name;
    EXPECT_GT(result->code_size, 0u) << name;
    EXPECT_EQ(jit_ctx->lookupFunc(pyfunc), nullptr) << name;
  }
}

TEST_F(ShadowCompileTest, StringSubscriptPassesShadowVerifier) {
  const char* pycode = R"(
def func(name):
  return len(name) > 4 and name[:2] == name[-2:] == "__" and name[2] != "_"
)";

  Ref<PyFunctionObject> pyfunc(compileAndGet(pycode, "func"));
  ASSERT_NE(pyfunc, nullptr);
  EXPECT_EQ(unsupportedShapeReason311(pyfunc->func_code), nullptr);

  auto* jit_ctx = reinterpret_cast<jit::CompilerContext<Compiler>*>(
      cinderx::getModuleState()->jit_context.get());
  ASSERT_NE(jit_ctx, nullptr);

  auto result = jit_ctx->compiler().CompileShadow(pyfunc);
  ASSERT_TRUE(result.has_value());
  EXPECT_GT(result->code_size, 0u);
  EXPECT_EQ(jit_ctx->lookupFunc(pyfunc), nullptr);
}

TEST_F(ShadowCompileTest, WithStatementPassesShadowVerifier) {
  const char* pycode = R"(
class Ctx:
  def __enter__(self):
    return self
  def __exit__(self, *exc):
    return False

def func():
  with Ctx():
    return 1
)";

  Ref<PyFunctionObject> pyfunc(compileAndGet(pycode, "func"));
  ASSERT_NE(pyfunc, nullptr);
  EXPECT_EQ(unsupportedShapeReason311(pyfunc->func_code), nullptr);

  auto* jit_ctx = reinterpret_cast<jit::CompilerContext<Compiler>*>(
      cinderx::getModuleState()->jit_context.get());
  ASSERT_NE(jit_ctx, nullptr);

  auto result = jit_ctx->compiler().CompileShadow(pyfunc);
  ASSERT_TRUE(result.has_value());
  EXPECT_GT(result->code_size, 0u);
  EXPECT_EQ(jit_ctx->lookupFunc(pyfunc), nullptr);
}

TEST_F(ShadowCompileTest, ShadowAbortBecomesCompileFailure) {
  // Nested try/except/with surfaces exception-table opcodes.  Shadow
  // compilation must report a C++ exception rather than aborting the process.
  const char* pycode = R"(
def func(items):
  total = 0
  try:
    for item in items:
      total += item
  except TypeError:
    return None
  return total
)";

  Ref<PyFunctionObject> pyfunc(compileAndGet(pycode, "func"));
  ASSERT_NE(pyfunc, nullptr);

  auto* jit_ctx = reinterpret_cast<jit::CompilerContext<Compiler>*>(
      cinderx::getModuleState()->jit_context.get());
  ASSERT_NE(jit_ctx, nullptr);

  try {
    auto result = jit_ctx->compiler().CompileShadow(pyfunc);
    EXPECT_EQ(jit_ctx->lookupFunc(pyfunc), nullptr);
    if (result.has_value()) {
      EXPECT_GT(result->code_size, 0u);
    }
  } catch (const std::exception&) {
    EXPECT_EQ(jit_ctx->lookupFunc(pyfunc), nullptr);
  }
}

TEST_F(ShadowCompileTest, ShadowCompileValidatesSplitCodeLayout) {
  Config saved_config = getConfig();
  getMutableConfig().multiple_code_sections = true;
  SCOPE_EXIT(getMutableConfig() = saved_config);

  const char* pycode = R"(
def func(value):
  if value:
    return value + 1
  raise ValueError("value")
)";

  Ref<PyFunctionObject> pyfunc(compileAndGet(pycode, "func"));
  ASSERT_NE(pyfunc, nullptr);

  auto* module_state = cinderx::getModuleState();
  ASSERT_NE(module_state, nullptr);
  auto* jit_ctx = reinterpret_cast<jit::CompilerContext<Compiler>*>(
      module_state->jit_context.get());
  ASSERT_NE(jit_ctx, nullptr);

  auto result = jit_ctx->compiler().CompileShadow(pyfunc);

  ASSERT_TRUE(result.has_value());
  EXPECT_GT(result->code_size, 0u);
}

TEST_F(ShadowCompileTest, ShadowCompileCountsSpecializedOpcodes) {
  const char* pycode = R"(
def func(left, right):
  return left + right

for _ in range(100):
  func(1, 2)
)";

  Ref<PyFunctionObject> pyfunc(compileAndGet(pycode, "func"));
  ASSERT_NE(pyfunc, nullptr);

  auto* module_state = cinderx::getModuleState();
  ASSERT_NE(module_state, nullptr);
  auto* jit_ctx = reinterpret_cast<jit::CompilerContext<Compiler>*>(
      module_state->jit_context.get());
  ASSERT_NE(jit_ctx, nullptr);

  auto result = jit_ctx->compiler().CompileShadow(pyfunc);

  ASSERT_TRUE(result.has_value());
  EXPECT_GT(result->specialized_opcodes, 0u);
}
#endif

TEST_F(ASMGeneratorTest, SanityCheck) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def func():
  a = 314159
  return a
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  PyObject* args[] = {};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 0));
  ASSERT_NE(res, nullptr);
  ASSERT_EQ(PyLong_AsLong(res), 314159);
}

TEST_F(ASMGeneratorTest, Fallthrough) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* src = R"(
def func2(x):
  y = 0
  if x:
    y = 100
  return y
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func2"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg0 = Ref<>::steal(PyLong_FromLong(16));
  ASSERT_NE(arg0, nullptr);
  PyObject* args[] = {arg0};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));
  ASSERT_NE(res, nullptr);
  ASSERT_EQ(PyObject_IsTrue(res), 1);
}

TEST_F(ASMGeneratorTest, CondBranchTest) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def func2(x):
    if x:
        return True
    return False
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "func2"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg0 = Ref<>::steal(PyLong_FromLong(16));
  ASSERT_NE(arg0, nullptr);
  PyObject* args[] = {arg0};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));
  ASSERT_NE(res, nullptr);
  ASSERT_EQ(PyObject_IsTrue(res), 1);

  auto arg1 = Ref<>::steal(PyLong_FromLong(0));
  args[0] = arg1;
  auto res2 = Ref<>::steal(compiled->invoke(pyfunc, args, 1));
  ASSERT_NE(res2, nullptr);
  ASSERT_EQ(PyObject_IsTrue(res2), 0);
}

TEST_F(ASMGeneratorTest, UnboundLocalError) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(x):
    if x:
        y = 1
    z = 100
    return y
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg = Ref<>::steal(PyLong_FromLong(0));
  ASSERT_NE(arg, nullptr);

  PyObject* args[] = {arg.get()};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));
  ASSERT_EQ(res, nullptr);

  PyObject *etyp, *eval, *etb;
  PyErr_Fetch(&etyp, &eval, &etb);
  ASSERT_NE(etyp, nullptr);

  auto typ = Ref<>::steal(etyp);
  auto val = Ref<>::steal(eval);
  auto tb = Ref<>::steal(etb);
  EXPECT_TRUE(PyErr_GivenExceptionMatches(typ, PyExc_UnboundLocalError));
  ASSERT_NE(val.get(), nullptr);

  std::string msg;
  auto sval =
      Ref<>::steal(PyObject_CallMethod(val, "__str__", nullptr /* format */));
  ASSERT_TRUE(PyUnicode_Check(sval));
  msg = PyUnicode_AsUTF8(sval);

  const std::string_view kExpected =
      "cannot access local variable 'y' where it is not associated with a "
      "value";
  ASSERT_EQ(msg, kExpected);

  auto tb_frame = Ref<>::steal(PyObject_GetAttrString(tb, "tb_frame"));
  ASSERT_NE(tb_frame.get(), nullptr);

  auto locals = Ref<>::steal(PyObject_GetAttrString(tb_frame, "f_locals"));
  ASSERT_NE(locals.get(), nullptr);
  EXPECT_EQ(PyObject_Length(locals), 2);
  PyObject* x = PyMapping_GetItemString(locals, "x");
  ASSERT_TRUE(PyLong_CheckExact(x));
  EXPECT_EQ(PyLong_AsLong(x), 0);
  PyObject* y = PyMapping_GetItemString(locals, "z");
  ASSERT_TRUE(PyLong_CheckExact(y));
  EXPECT_EQ(PyLong_AsLong(y), 100);
}

TEST_F(ASMGeneratorTest, InsertXDecrefForMaybeAssignedRegisters) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(x):
    if x:
        y = 1
    z = y
    return z
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg = Ref<>::steal(PyLong_FromLong(133));
  ASSERT_NE(arg, nullptr);

  PyObject* args[] = {arg};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(PyLong_AsLong(res), 1);
}

TEST_F(ASMGeneratorTest, LoadAttr) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(x):
    return x.denominator
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg0 = Ref<>::steal(PyLong_FromLong(16));
  ASSERT_NE(arg0, nullptr);
  PyObject* args[] = {arg0};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));
  ASSERT_NE(res, nullptr);
  ASSERT_EQ(PyLong_AsLong(res), 1);
}

TEST_F(ASMGeneratorTest, LoadAttrRaisesError) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(x):
    y = 100
    return x.denominator
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg = Ref<>::steal(PyDict_New());
  ASSERT_NE(arg, nullptr);

  PyObject* args[] = {arg};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));
  ASSERT_EQ(res, nullptr);

  PyObject *etyp, *eval, *etb;
  PyErr_Fetch(&etyp, &eval, &etb);
  ASSERT_NE(etyp, nullptr);

  auto typ = Ref<>::steal(etyp);
  auto val = Ref<>::steal(eval);
  auto tb = Ref<>::steal(etb);
  EXPECT_TRUE(PyErr_GivenExceptionMatches(typ, PyExc_AttributeError));
  ASSERT_NE(val, nullptr);
  EXPECT_TRUE(PyObject_IsInstance(val, PyExc_AttributeError));
  auto ae = reinterpret_cast<PyAttributeErrorObject*>(val.get());
  ASSERT_TRUE(PyUnicode_Check(ae->name));
  std::string msg = PyUnicode_AsUTF8(ae->name);
  ASSERT_EQ(msg, "denominator");

  auto tb_frame = Ref<>::steal(PyObject_GetAttrString(tb, "tb_frame"));
  ASSERT_NE(tb_frame.get(), nullptr);

  auto locals = Ref<>::steal(PyObject_GetAttrString(tb_frame, "f_locals"));
  ASSERT_NE(locals.get(), nullptr);
  EXPECT_EQ(PyObject_Length(locals), 2);
  PyObject* x = PyMapping_GetItemString(locals, "x");
  ASSERT_EQ(x, arg);
  PyObject* y = PyMapping_GetItemString(locals, "y");
  ASSERT_TRUE(PyLong_CheckExact(y));
  EXPECT_EQ(PyLong_AsLong(y), 100);
}

TEST_F(ASMGeneratorTest, StoreAttr) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* klasscode = R"(
class TestClass:
  pass
)";
  Ref<PyObject> klass(compileAndGet(klasscode, "TestClass"));
  ASSERT_NE(klass, nullptr);

  const char* pycode = R"(
def test(x):
  x.foo = 100
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  PyObject* args[] = {klass};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));
  ASSERT_NE(res, nullptr);

  auto val = Ref<>::steal(PyObject_GetAttrString(klass, "foo"));
  ASSERT_NE(val.get(), nullptr);
  ASSERT_TRUE(PyLong_CheckExact(val));
  EXPECT_EQ(PyLong_AsLong(val), 100);
}

TEST_F(ASMGeneratorTest, Compare) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
    return a is b;
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg0 = Ref<>::steal(PyLong_FromLong(16));
  auto arg1 = Ref<>::steal(PyLong_FromLong(32));
  PyObject* args[] = {arg0, arg1};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));
  ASSERT_NE(res, nullptr);
  ASSERT_EQ(PyObject_IsTrue(res), 0);

  auto arg2 = Ref<>::steal(PyLong_FromLong(0));
  args[0] = arg2;
  args[1] = arg2;
  auto res2 = Ref<>::steal(compiled->invoke(pyfunc, args, 2));
  ASSERT_NE(res2, nullptr);
  ASSERT_EQ(PyObject_IsTrue(res2), 1);
}

TEST_F(ASMGeneratorTest, LoadGlobalTest) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test():
    return len
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  PyObject* args[] = {};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 0));
  ASSERT_NE(res, nullptr);

  PyObject* globals = PyFunction_GetGlobals(pyfunc);
  ASSERT_NE(globals, nullptr);

  PyObject* builtins = PyDict_GetItemString(globals, "__builtins__");
  ASSERT_NE(builtins, nullptr);

  if (PyModule_Check(builtins)) {
    builtins = PyModule_GetDict(builtins);
    ASSERT_NE(builtins, nullptr);
  }

  PyObject* len = PyDict_GetItemString(builtins, "len");
  ASSERT_EQ(res, len);
}

TEST_F(ASMGeneratorTest, CallCFunction) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(x):
  return len(x)
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg = Ref<>::steal(PyDict_New());
  ASSERT_NE(arg.get(), nullptr);

  PyObject* args[] = {arg};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));

  ASSERT_NE(res.get(), nullptr);
  ASSERT_TRUE(PyLong_Check(res.get()));
  EXPECT_EQ(PyLong_AsLong(res), 0);
}

TEST_F(ASMGeneratorTest, CallBoundMethod) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(l):
  l.append(123)
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg = Ref<>::steal(PyList_New(0));
  ASSERT_NE(arg.get(), nullptr);

  PyObject* args[] = {arg};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));

  ASSERT_NE(res.get(), nullptr);
  ASSERT_EQ(PyList_Size(arg), 1);

  PyObject* elem = PyList_GetItem(arg, 0);
  ASSERT_TRUE(PyLong_Check(elem));
  EXPECT_EQ(PyLong_AsLong(elem), 123);
}

TEST_F(ASMGeneratorTest, DefaultArgTest) {
#if PY_VERSION_HEX < 0x030C0000
  GTEST_SKIP() << "CPython 3.11 default-argument JIT call crashes in "
                  "JITRT_CallWithIncorrectArgcount";
#endif
  const char* pycode = R"(
def test(a, b, c=100):
    return a + b + c
)";

  Ref<PyFunctionObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  pyfunc->vectorcall = compiled->vectorcallEntry();
  ASSERT_NE(compiled, nullptr);

  auto one = Ref<>::steal(PyLong_FromLong(1));
  auto two = Ref<>::steal(PyLong_FromLong(2));
  auto three = Ref<>::steal(PyLong_FromLong(3));
  PyObject* args[] = {one, two, three};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));

  ASSERT_NE(res.get(), nullptr);
  ASSERT_EQ(PyLong_AsLong(res.get()), 103);

  auto res2 = Ref<>::steal(compiled->invoke(pyfunc, args, 3));
  ASSERT_NE(res2.get(), nullptr);
  ASSERT_EQ(PyLong_AsLong(res2.get()), 6);
}

TEST_F(ASMGeneratorTest, KWArgCall) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
    return a + b;
)";

  Ref<PyFunctionObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg0 = Ref<>::steal(PyLong_FromLong(16));
  ASSERT_NE(arg0, nullptr);

  auto arg1 = Ref<>::steal(PyLong_FromLong(32));
  ASSERT_NE(arg1, nullptr);

  auto kwnames = Ref<>::steal(Py_BuildValue("(s)", "b"));
  ASSERT_NE(kwnames, nullptr);

  vectorcallfunc cfunc = compiled->vectorcallEntry();
  pyfunc->vectorcall = cfunc;
  auto pfunc = reinterpret_cast<PyObject*>(pyfunc.get());
  PyObject* args[] = {arg0, arg1};
  auto result = Ref<>::steal(cfunc(pfunc, args, 1, kwnames));

  ASSERT_NE(result, nullptr);
  ASSERT_EQ(PyLong_AsLong(result), 48);
}

TEST_F(ASMGeneratorTest, CallPythonFunction) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def meaning_of_life():
  return 42

def test(f):
  return f()
)";

  Ref<PyObject> oracle(compileAndGet(pycode, "meaning_of_life"));
  ASSERT_NE(oracle.get(), nullptr) << "Failed compiling func";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  PyObject* args[] = {oracle};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));

  ASSERT_NE(res.get(), nullptr);
  ASSERT_TRUE(PyLong_Check(res.get()));
  EXPECT_EQ(PyLong_AsLong(res), 42);
}

TEST_F(ASMGeneratorTest, CallType) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* klasscode = R"(
class TestClass:
  pass
)";
  Ref<PyObject> klass(compileAndGet(klasscode, "TestClass"));
  ASSERT_NE(klass.get(), nullptr);

  const char* pycode = R"(
def test(f):
  return f()
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  PyObject* args[] = {klass};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));

  ASSERT_NE(res.get(), nullptr);
  ASSERT_EQ(PyObject_IsInstance(res, klass), 1);
}

TEST_F(ASMGeneratorTest, InvokeBinaryAdd) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
  return a + b
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg1 = Ref<>::steal(PyLong_FromLong(100));
  ASSERT_NE(arg1.get(), nullptr);

  auto arg2 = Ref<>::steal(PyLong_FromLong(200));
  ASSERT_NE(arg2.get(), nullptr);

  PyObject* args[] = {arg1, arg2};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));

  ASSERT_NE(res.get(), nullptr);
  ASSERT_TRUE(PyLong_CheckExact(res.get()));
  EXPECT_EQ(PyLong_AsLong(res.get()), 300);
}

TEST_F(ASMGeneratorTest, InvokeBinaryAnd) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
  return a & b
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg1 = Ref<>::steal(PyLong_FromLong(1));
  ASSERT_NE(arg1.get(), nullptr);

  auto arg2 = Ref<>::steal(PyLong_FromLong(3));
  ASSERT_NE(arg2.get(), nullptr);

  PyObject* args[] = {arg1, arg2};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));

  ASSERT_NE(res.get(), nullptr);
  ASSERT_TRUE(PyLong_CheckExact(res.get()));
  EXPECT_EQ(PyLong_AsLong(res.get()), 1);
}

TEST_F(ASMGeneratorTest, InvokeBinaryFloorDivide) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
  return a // b
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg1 = Ref<>::steal(PyFloat_FromDouble(4.0));
  ASSERT_NE(arg1.get(), nullptr);

  auto arg2 = Ref<>::steal(PyFloat_FromDouble(2.5));
  ASSERT_NE(arg2.get(), nullptr);

  PyObject* args[] = {arg1, arg2};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));

  ASSERT_NE(res.get(), nullptr);
  ASSERT_TRUE(PyFloat_CheckExact(res.get()));
  EXPECT_EQ(PyFloat_AsDouble(res.get()), 1.0);
}

TEST_F(ASMGeneratorTest, InvokeBinaryLShift) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
  return a << b
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg1 = Ref<>::steal(PyLong_FromLong(2));
  ASSERT_NE(arg1.get(), nullptr);

  auto arg2 = Ref<>::steal(PyLong_FromLong(1));
  ASSERT_NE(arg2.get(), nullptr);

  PyObject* args[] = {arg1, arg2};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));

  ASSERT_NE(res.get(), nullptr);
  ASSERT_TRUE(PyLong_CheckExact(res.get()));
  EXPECT_EQ(PyLong_AsLong(res.get()), 4);
}

TEST_F(ASMGeneratorTest, InvokeBinaryModulo) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
  return a % b
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg1 = Ref<>::steal(PyLong_FromLong(200));
  ASSERT_NE(arg1.get(), nullptr);

  auto arg2 = Ref<>::steal(PyLong_FromLong(150));
  ASSERT_NE(arg2.get(), nullptr);

  PyObject* args[] = {arg1, arg2};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));

  ASSERT_NE(res.get(), nullptr);
  ASSERT_TRUE(PyLong_CheckExact(res.get()));
  EXPECT_EQ(PyLong_AsLong(res.get()), 50);
}

TEST_F(ASMGeneratorTest, InvokeBinaryMultiply) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
  return a * b
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg1 = Ref<>::steal(PyLong_FromLong(2));
  ASSERT_NE(arg1.get(), nullptr);

  auto arg2 = Ref<>::steal(PyLong_FromLong(4));
  ASSERT_NE(arg2.get(), nullptr);

  PyObject* args[] = {arg1, arg2};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));

  ASSERT_NE(res.get(), nullptr);
  ASSERT_TRUE(PyLong_CheckExact(res.get()));
  EXPECT_EQ(PyLong_AsLong(res.get()), 8);
}

TEST_F(ASMGeneratorTest, InvokeBinaryOr) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
  return a | b
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg1 = Ref<>::steal(PyLong_FromLong(1));
  ASSERT_NE(arg1.get(), nullptr);

  auto arg2 = Ref<>::steal(PyLong_FromLong(2));
  ASSERT_NE(arg2.get(), nullptr);

  PyObject* args[] = {arg1, arg2};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));

  ASSERT_NE(res.get(), nullptr);
  ASSERT_TRUE(PyLong_CheckExact(res.get()));
  EXPECT_EQ(PyLong_AsLong(res.get()), 3);
}

TEST_F(ASMGeneratorTest, InvokeBinarySubscr) {
#if PY_VERSION_HEX < 0x030C0000
  GTEST_SKIP() << "CPython 3.11 binary subscription execution assertion fails";
#endif
  const char* pycode = R"(
def test(x):
  l = [1, 2]
  return l[x]
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg1 = Ref<>::steal(PyLong_FromLong(1));
  ASSERT_NE(arg1.get(), nullptr);

  PyObject* args[] = {arg1};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));

  ASSERT_NE(res.get(), nullptr);
  ASSERT_TRUE(PyLong_CheckExact(res.get()));
  EXPECT_EQ(PyLong_AsLong(res.get()), 2);
}

TEST_F(ASMGeneratorTest, InvokeBinarySubtract) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
  return a - b
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg1 = Ref<>::steal(PyLong_FromLong(3));
  ASSERT_NE(arg1.get(), nullptr);

  auto arg2 = Ref<>::steal(PyLong_FromLong(2));
  ASSERT_NE(arg2.get(), nullptr);

  PyObject* args[] = {arg1, arg2};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));

  ASSERT_NE(res.get(), nullptr);
  ASSERT_TRUE(PyLong_CheckExact(res.get()));
  EXPECT_EQ(PyLong_AsLong(res.get()), 1);
}

TEST_F(ASMGeneratorTest, InvokeBinaryTrueDivide) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
  return a / b
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg1 = Ref<>::steal(PyLong_FromLong(3));
  ASSERT_NE(arg1.get(), nullptr);

  auto arg2 = Ref<>::steal(PyLong_FromLong(2));
  ASSERT_NE(arg2.get(), nullptr);

  PyObject* args[] = {arg1, arg2};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));

  ASSERT_NE(res.get(), nullptr);
  ASSERT_TRUE(PyFloat_CheckExact(res.get()));
  EXPECT_EQ(PyFloat_AsDouble(res.get()), 1.5);
}

TEST_F(ASMGeneratorTest, InvokeBinaryXor) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
  return a ^ b
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg1 = Ref<>::steal(PyLong_FromLong(3));
  ASSERT_NE(arg1.get(), nullptr);

  auto arg2 = Ref<>::steal(PyLong_FromLong(1));
  ASSERT_NE(arg2.get(), nullptr);

  PyObject* args[] = {arg1, arg2};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));

  ASSERT_NE(res.get(), nullptr);
  ASSERT_TRUE(PyLong_CheckExact(res.get()));
  EXPECT_EQ(PyLong_AsLong(res.get()), 2);
}

TEST_F(ASMGeneratorTest, ReplaceReassignedFirstArgInExceptionFrame) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(x, y):
    if x:
      x = 2
    y.invalid
    # Need a use of x here, otherwise it's reclaimed at the end of the
    # if block
    y = x
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg1 = Ref<>::steal(PyLong_FromLong(100));
  ASSERT_NE(arg1, nullptr);

  auto arg2 = Ref<>::steal(PyDict_New());
  ASSERT_NE(arg2, nullptr);

  PyObject* args[] = {arg1.get(), arg2.get()};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));
  ASSERT_EQ(res, nullptr);

  PyObject *etyp, *eval, *etb;
  PyErr_Fetch(&etyp, &eval, &etb);
  ASSERT_NE(etyp, nullptr);

  auto typ = Ref<>::steal(etyp);
  auto val = Ref<>::steal(eval);
  auto tb = Ref<>::steal(etb);
  EXPECT_TRUE(PyErr_GivenExceptionMatches(typ, PyExc_AttributeError));
  EXPECT_TRUE(PyExceptionInstance_Check(val.get()));
  auto exc_args = Ref<>::steal(PyObject_GetAttrString(val.get(), "args"));
  EXPECT_TRUE(PyTuple_Check(exc_args.get()));
  EXPECT_EQ(PyObject_Length(exc_args.get()), 1);
  auto msg = Ref<>::create(PyTuple_GetItem(exc_args.get(), 0));
  ASSERT_TRUE(PyUnicode_Check(msg));
  std::string msg_str = PyUnicode_AsUTF8(msg);
  ASSERT_EQ(msg_str, "'dict' object has no attribute 'invalid'");

  auto tb_frame = Ref<>::steal(PyObject_GetAttrString(tb, "tb_frame"));
  ASSERT_NE(tb_frame.get(), nullptr);

  auto locals = Ref<>::steal(PyObject_GetAttrString(tb_frame, "f_locals"));
  ASSERT_NE(locals.get(), nullptr);
  EXPECT_EQ(PyObject_Length(locals), 2);
  PyObject* x = PyMapping_GetItemString(locals, "x");
  ASSERT_TRUE(PyLong_CheckExact(x));
  EXPECT_EQ(PyLong_AsLong(x), 2);
  PyObject* y = PyMapping_GetItemString(locals, "y");
  ASSERT_EQ(y, arg2.get());
}

TEST_F(ASMGeneratorTest, TupleListTest) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test_tuple(a):
    return (a, a, a)
def test_list(a):
    return [a, a, a]
)";

  auto three = Ref<>::steal(PyLong_FromLong(3));
  {
    Ref<PyObject> pyfunc(compileAndGet(pycode, "test_tuple"));
    ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

    auto compiled = GenerateCode(pyfunc);
    ASSERT_NE(compiled, nullptr);

    PyObject* args[] = {three.get()};
    auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));

    ASSERT_NE(res.get(), nullptr);
    ASSERT_EQ(PyTuple_GetItem(res.get(), 0), args[0]);
    ASSERT_EQ(PyTuple_GetItem(res.get(), 1), args[0]);
    ASSERT_EQ(PyTuple_GetItem(res.get(), 2), args[0]);
  }

  {
    Ref<PyObject> pyfunc(compileAndGet(pycode, "test_list"));
    ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

    auto compiled = GenerateCode(pyfunc);
    ASSERT_NE(compiled, nullptr);

    PyObject* args[] = {three.get()};
    auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));

    ASSERT_NE(res.get(), nullptr);
    ASSERT_EQ(PyList_GetItem(res.get(), 0), args[0]);
    ASSERT_EQ(PyList_GetItem(res.get(), 1), args[0]);
    ASSERT_EQ(PyList_GetItem(res.get(), 2), args[0]);
  }
}

static void
UnaryTest(ASMGeneratorTest* test, const char* pycode, int inp, int expected) {
  Ref<PyObject> pyfunc(test->compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = test->GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);
  {
    auto arg = Ref<>::steal(PyLong_FromLong(inp));
    PyObject* args[] = {
        arg,
    };
    auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));

    ASSERT_NE(res.get(), nullptr);
    ASSERT_EQ(PyLong_AsLong(res.get()), expected);
  }
  {
    auto arg = Ref<>::steal(PyUnicode_FromString("foo"));
    PyObject* args[] = {
        arg,
    };
    auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));

    ASSERT_EQ(res, nullptr);

    PyObject *etyp, *eval, *etb;
    PyErr_Fetch(&etyp, &eval, &etb);
    ASSERT_NE(etyp, nullptr);

    auto typ = Ref<>::steal(etyp);
    auto val = Ref<>::steal(eval);
    auto tb = Ref<>::steal(etb);
    EXPECT_TRUE(PyErr_GivenExceptionMatches(typ, PyExc_TypeError));
  }
}

TEST_F(ASMGeneratorTest, InvokeUnaryNot) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a):
    return not a
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  {
    auto one = Ref<>::steal(PyLong_FromLong(1));
    PyObject* args[] = {one.get()};
    auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));

    ASSERT_EQ(res.get(), Py_False);
  }

  {
    auto zero = Ref<>::steal(PyLong_FromLong(0));
    PyObject* args[] = {zero.get()};
    auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));

    ASSERT_EQ(res.get(), Py_True);
  }
}

TEST_F(ASMGeneratorTest, InvokeUnaryNegative) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a):
    return -a
)";

  UnaryTest(this, pycode, 2, -2);
}

TEST_F(ASMGeneratorTest, InvokeUnaryPositive) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a):
    return +a
)";

  UnaryTest(this, pycode, 2, 2);
}

TEST_F(ASMGeneratorTest, InvokeUnaryInvert) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a):
    return ~a
)";

  UnaryTest(this, pycode, 1, -2);
}

TEST_F(ASMGeneratorTest, StoreSubscr) {
#if PY_VERSION_HEX < 0x030C0000
  GTEST_SKIP() << "CPython 3.11 subscription store execution assertion fails";
#endif
  const char* pycode = R"(
def test(c, s, v):
  c[s] = v
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto dict = Ref<>::steal(PyDict_New());
  auto one = Ref<>::steal(PyLong_FromLong(1));
  auto two = Ref<>::steal(PyLong_FromLong(2));

  // Success case
  {
    PyObject* args[] = {dict, one, two};
    auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 3));

    ASSERT_EQ(res.get(), Py_None);
    ASSERT_EQ(PyDict_Size(dict), 1);
    ASSERT_EQ(PyDict_GetItem(dict, one), two);
  }
  // Error case
  {
    PyObject* args[] = {one, dict, two};
    auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 3));
    ASSERT_EQ(res.get(), nullptr);

    PyObject *etyp, *eval, *etb;
    PyErr_Fetch(&etyp, &eval, &etb);
    ASSERT_NE(etyp, nullptr);

    auto typ = Ref<>::steal(etyp);
    auto val = Ref<>::steal(eval);
    auto tb = Ref<>::steal(etb);
    EXPECT_TRUE(PyErr_GivenExceptionMatches(typ, PyExc_TypeError));
    PyErr_Clear();
  }
}

static void InPlaceOpTest(
    ASMGeneratorTest* test,
    const char* pycode,
    int a,
    int b,
    int expected) {
  Ref<PyObject> pyfunc(test->compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = test->GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);
  {
    auto aval = Ref<>::steal(PyLong_FromLong(a));
    auto bval = Ref<>::steal(PyLong_FromLong(b));
    PyObject* args[] = {aval, bval};
    auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));

    ASSERT_NE(res.get(), nullptr);
    ASSERT_EQ(PyLong_AsLong(res.get()), expected);
  }
  {
    PyObject* args[] = {Py_None, Py_None};
    auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));

    ASSERT_EQ(res, nullptr);

    PyObject *etyp, *eval, *etb;
    PyErr_Fetch(&etyp, &eval, &etb);
    ASSERT_NE(etyp, nullptr);

    auto typ = Ref<>::steal(etyp);
    auto val = Ref<>::steal(eval);
    auto tb = Ref<>::steal(etb);
    EXPECT_TRUE(PyErr_GivenExceptionMatches(typ, PyExc_TypeError));
  }
}

TEST_F(ASMGeneratorTest, InvokeInPlaceAdd) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
    a += b
    return a
)";

  InPlaceOpTest(this, pycode, 1, 2, 3);
}

TEST_F(ASMGeneratorTest, InvokeInPlaceAnd) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
    a &= b
    return a
)";

  InPlaceOpTest(this, pycode, 1, 2, 0);
}

TEST_F(ASMGeneratorTest, InvokeInPlaceFloorDivide) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
    a //= b
    return a
)";

  InPlaceOpTest(this, pycode, 11, 2, 5);
}

TEST_F(ASMGeneratorTest, InvokeInPlaceLShift) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
    a <<= b
    return a
)";

  InPlaceOpTest(this, pycode, 11, 2, 44);
}

TEST_F(ASMGeneratorTest, InvokeInPlaceRemainder) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
    a %= b
    return a
)";

  InPlaceOpTest(this, pycode, 11, 2, 1);
}

TEST_F(ASMGeneratorTest, InvokeInPlaceMatrixMultiply) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
    a @= b
    return a
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);

  PyObject* args[] = {Py_None, Py_None};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));

  ASSERT_EQ(res, nullptr);

  PyObject *etyp, *eval, *etb;
  PyErr_Fetch(&etyp, &eval, &etb);
  ASSERT_NE(etyp, nullptr);

  auto typ = Ref<>::steal(etyp);
  auto val = Ref<>::steal(eval);
  auto tb = Ref<>::steal(etb);
  EXPECT_TRUE(PyErr_GivenExceptionMatches(typ, PyExc_TypeError));
}

TEST_F(ASMGeneratorTest, InvokeInPlaceMultiply) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
    a *= b
    return a
)";

  InPlaceOpTest(this, pycode, 11, 2, 22);
}

TEST_F(ASMGeneratorTest, InvokeInPlaceOr) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
    a |= b
    return a
)";

  InPlaceOpTest(this, pycode, 11, 4, 15);
}

TEST_F(ASMGeneratorTest, InvokeInPlacePower) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
    a **= b
    return a
)";

  InPlaceOpTest(this, pycode, 11, 2, 121);
}

TEST_F(ASMGeneratorTest, InvokeInPlaceRShift) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
    a >>= b
    return a
)";

  InPlaceOpTest(this, pycode, 11, 2, 2);
}

TEST_F(ASMGeneratorTest, InvokeInPlaceSubtract) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
    a -= b
    return a
)";

  InPlaceOpTest(this, pycode, 11, 2, 9);
}

TEST_F(ASMGeneratorTest, InvokeInPlaceTrueDivide) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
    a /= b
    return a
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);
  {
    auto aval = Ref<>::steal(PyLong_FromLong(1));
    auto bval = Ref<>::steal(PyLong_FromLong(2));
    PyObject* args[] = {aval, bval};
    auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));

    ASSERT_NE(res.get(), nullptr);
    ASSERT_EQ(PyFloat_AsDouble(res.get()), 0.5);
  }
  {
    PyObject* args[] = {Py_None, Py_None};
    auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));

    ASSERT_EQ(res, nullptr);

    PyObject *etyp, *eval, *etb;
    PyErr_Fetch(&etyp, &eval, &etb);
    ASSERT_NE(etyp, nullptr);

    auto typ = Ref<>::steal(etyp);
    auto val = Ref<>::steal(eval);
    auto tb = Ref<>::steal(etb);
    EXPECT_TRUE(PyErr_GivenExceptionMatches(typ, PyExc_TypeError));
  }
}

TEST_F(ASMGeneratorTest, InvokeInPlaceXor) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test(a, b):
    a ^= b
    return a
)";

  InPlaceOpTest(this, pycode, 11, 2, 9);
}

TEST_F(ASMGeneratorTest, InvokeInPlaceNotDefined) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* pycode = R"(
def test():
    a += 1
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);

  PyObject* args[] = {};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 0));

  ASSERT_EQ(res, nullptr);

  PyObject *etyp, *eval, *etb;
  PyErr_Fetch(&etyp, &eval, &etb);
  ASSERT_NE(etyp, nullptr);

  auto typ = Ref<>::steal(etyp);
  auto val = Ref<>::steal(eval);
  auto tb = Ref<>::steal(etb);
  EXPECT_TRUE(PyErr_GivenExceptionMatches(typ, PyExc_UnboundLocalError));
}

TEST_F(ASMGeneratorTest, TestDeepRegUsage) {
  SKIP_311_EXECUTABLE_COMPILE();
  const char* helpercode = R"(
def f(*args):
    return sum(args)
)";
  Ref<PyObject> f(compileAndGet(helpercode, "f"));

  const char* pycode = R"(
def test(a, func):
    if a:
        b = a; c = a; d = a; e = a; f = a; g = a; h = a; i = a; j = a; k = a
        l = a; m = a; n = a; o = a; p = a; q = a; r = a; s = a; t = a
    return func(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t)
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);

  auto aval = Ref<>::steal(PyLong_FromLong(1));

  PyObject* args[] = {aval, f};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));
  ASSERT_EQ(PyLong_AsLong(res), 20);
}

// This can't be tested in the pure Python test suite as it messes with
// __import__.
TEST_F(ASMGeneratorTest, TestImportNameWithImportOverride) {
#if PY_VERSION_HEX < 0x030C0000
  GTEST_SKIP() << "CPython 3.11 overridden import execution assertion fails";
#endif
  const char* pycode = R"(
def test_override_builtin_import(locals):
    captured_data = []

    def fake_import(name, globals, locals, fromlist, level):
        nonlocal captured_data
        captured_data.append([name, len(globals), locals, fromlist, level])

    old_import = __builtins__.__import__
    __builtins__.__import__ = fake_import
    # The CPython interpreter has strange (probably broken) behavior - it will
    # only pass a dictionary of locals to __builtins__.__import__() if the
    # locals have been materialized already, for example by a call to locals().
    # In our implementation of IMPORT_NAME we just always pass None for locals.
    if locals:
        locals()
    import x
    import x.y
    __builtins__.__import__ = old_import
    return repr(captured_data) == "[['x', 7, None, None, 0], ['x.y', 7, None, None, 0]]"
)";

  runCode(pycode);

  auto pyfunc = getGlobal("test_override_builtin_import");
  ASSERT_NE(pyfunc, nullptr)
      << "Failed getting global test_override_builtin_import";
  auto compiled_func = GenerateCode(pyfunc);
  ASSERT_NE(compiled_func, nullptr)
      << "Failed compiling test_override_builtin_import";

  // Without locals() call
  auto locals = Ref<>::steal(PyUnicode_FromString("locals"));
  PyObject* args[] = {Py_False};
  auto res = Ref<>::steal(compiled_func->invoke(pyfunc, args, 1));
  EXPECT_EQ(res.get(), Py_True)
      << "Failed to run test_override_builtin_import(False)";

  // With locals() call
  PyObject* args1[] = {PyDict_GetItem(PyEval_GetBuiltins(), locals)};
  auto res1 = Ref<>::steal(compiled_func->invoke(pyfunc, args1, 1));
  EXPECT_EQ(res1.get(), Py_True)
      << "Failed to run test_override_builtin_import(True)";
}

TEST_F(ASMGeneratorTest, GetLength) {
#if PY_VERSION_HEX < 0x030C0000
  GTEST_SKIP() << "CPython 3.11 GetLength execution assertion fails";
#endif
  //  0 LOAD_FAST  0
  //  2 GET_LENGTH
  //  4 RETURN_VALUE
  const char bc[] = {LOAD_FAST, 0, GET_LEN, 0, RETURN_VALUE, 0};
  auto bytecode = Ref<>::steal(PyBytes_FromStringAndSize(bc, sizeof(bc)));
  ASSERT_NE(bytecode.get(), nullptr);
  auto filename = Ref<>::steal(PyUnicode_FromString("filename"));
  auto funcname = Ref<>::steal(PyUnicode_FromString("funcname"));
  auto consts = Ref<>::steal(PyTuple_New(1));
  Py_INCREF(Py_None);
  PyTuple_SET_ITEM(consts.get(), 0, Py_None);
  auto param = Ref<>::steal(PyUnicode_FromString("param"));
  auto varnames = Ref<>::steal(PyTuple_Pack(1, param.get()));
  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto empty_string = Ref<>::steal(PyBytes_FromString(""));
  auto code = Ref<PyCodeObject>::steal(PyUnstable_Code_New(
      /*argcount=*/1,
      /*kwonlyargcount=*/0,
      /*nlocals=*/1,
      /*stacksize=*/0,
      /*flags=*/0,
      bytecode,
      consts,
      /*names=*/empty_tuple,
      varnames,
      /*freevars=*/empty_tuple,
      /*cellvars=*/empty_tuple,
      filename,
      funcname,
      /*_unused_qualname=*/funcname,
      /*firstlineno=*/0,
      /*linetable=*/empty_string,
      /*_unused_exceptiontable=*/empty_string));
  ASSERT_NE(code.get(), nullptr);

  auto func = Ref<PyFunctionObject>::steal(PyFunction_New(code, MakeGlobals()));
  ASSERT_NE(func.get(), nullptr);

  auto compiled = GenerateCode(func);
  ASSERT_NE(compiled, nullptr);

  auto arg = Ref<>::steal(PyList_New(3));
  PyList_SET_ITEM(arg.get(), 0, PyLong_FromLong(4));
  PyList_SET_ITEM(arg.get(), 1, PyLong_FromLong(5));
  PyList_SET_ITEM(arg.get(), 2, PyLong_FromLong(6));
  auto args = std::to_array({arg.get()});
  auto result = Ref<>::steal(compiled->invoke(func, args.data(), args.size()));
  EXPECT_TRUE(isIntEquals(result, 3));
}

class NewASMGeneratorTest : public RuntimeTest {
 public:
  Ref<CompiledFunction> GenerateCode(PyObject* func) {
    std::optional<CompiledFunctionData> compiled_func =
        Compiler().Compile(func);
    if (!compiled_func.has_value()) {
      return nullptr;
    }
    return CompiledFunction::create(std::move(*compiled_func), false);
  }
};

TEST_F(NewASMGeneratorTest, Linear) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* src = R"(
def func(x):
  return 16 + x
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg0 = Ref<>::steal(PyLong_FromLong(12));
  ASSERT_NE(arg0, nullptr);
  PyObject* args[] = {arg0};
  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 1));
  ASSERT_NE(res, nullptr);
  ASSERT_EQ(PyLong_AsLong(res), 28);
}

TEST_F(NewASMGeneratorTest, DiamondControlBlock) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* src = R"(
def func(a, b):
  c = 0
  if a:
    c = b + 100
  else:
    c = b + 4

  return a + c
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto compiled = GenerateCode(pyfunc);
  ASSERT_NE(compiled, nullptr);

  auto arg0 = Ref<>::steal(PyLong_FromLong(1));
  auto arg1 = Ref<>::steal(PyLong_FromLong(5));
  ASSERT_NE(arg0, nullptr);
  PyObject* args[] = {arg0, arg1};

  auto res = Ref<>::steal(compiled->invoke(pyfunc, args, 2));
  ASSERT_NE(res, nullptr);
  ASSERT_EQ(PyLong_AsLong(res), 106);
}

TEST_F(NewASMGeneratorTest, BlockSorter) {
  jit::lir::Function func;
  std::vector<jit::lir::BasicBlock*> blocks;
  for (int i = 0; i < 6; i++) {
    blocks.push_back(func.allocateBasicBlock());
  }

  // build cfg:

  /*
          --------------
         |     ----     |
         |    |    |    |
         v    v    |    |
    0--->2--->3--->1    4--->5
              |         ^
              |         |
               ---------
  */

  blocks[0]->addSuccessor(blocks[2]);
  blocks[2]->addSuccessor(blocks[3]);
  blocks[3]->addSuccessor(blocks[1]);
  blocks[3]->addSuccessor(blocks[4]);
  blocks[1]->addSuccessor(blocks[3]);
  blocks[4]->addSuccessor(blocks[2]);
  blocks[4]->addSuccessor(blocks[5]);

  func.sortBasicBlocks();

  size_t expected[] = {0, 2, 3, 1, 4, 5};
  for (size_t i = 0; i < 6; i++) {
    ASSERT_EQ(func.basicblocks()[i], blocks[expected[i]]) << "i = " << i;
  }
}
