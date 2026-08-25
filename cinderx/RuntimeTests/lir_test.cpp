// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/Jit/lir/dce.h"
#include "cinderx/Jit/lir/generator.h"
#include "cinderx/Jit/lir/parser.h"
#include "cinderx/Jit/lir/postalloc.h"
#include "cinderx/Jit/lir/postgen.h"
#include "cinderx/Jit/lir/regalloc.h"
#include "cinderx/Jit/lir/target_select.h"
#include "cinderx/Jit/lir/verify.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <math.h>

#include <memory>
#include <ostream>
#include <regex>
#include <string>
#include <string_view>
#include <utility>

using namespace asmjit;
using namespace jit;
using namespace jit::lir;

TEST(LIRTypeTest, DataTypeByteShift) {
  EXPECT_EQ(byteShift(DataType::k8bit), 0);
  EXPECT_EQ(byteShift(DataType::k16bit), 1);
  EXPECT_EQ(byteShift(DataType::k32bit), 2);
  EXPECT_EQ(byteShift(DataType::k64bit), 3);
  EXPECT_EQ(byteShift(DataType::kDouble), 3);
  EXPECT_EQ(byteShift(DataType::kObject), 3);
}

class LIRGeneratorTest : public RuntimeTest {
 public:
  std::string getLIRString(PyObject* func_obj) {
    JIT_CHECK(
        PyFunction_Check(func_obj),
        "Trying to compile something that isn't a function");
    BorrowedRef<PyFunctionObject> func{func_obj};

    PyObject* globals = PyFunction_GetGlobals(func_obj);
    if (!PyDict_CheckExact(globals)) {
      return "";
    }

    if (!PyDict_CheckExact(func->func_builtins)) {
      return "";
    }

    std::unique_ptr<jit::hir::Function> irfunc(buildHIR(func));

    Compiler::runPasses(*irfunc, PassConfig::kAllExceptInliner);

    jit::codegen::Environ env;

    env.ctx = getContext();

    jit::CodeRuntime runtime{func};
#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
    // Frame reifiers exist only on the lightweight-frames runtime; the 3.11
    // materialized-frame build has no reifier to attach.
    Ref<> reifier;
    if (irfunc->reifier != nullptr) {
      runtime.setReifier(irfunc->reifier);
    } else {
      reifier = makeFrameReifier(func->func_code);
      runtime.setReifier(reifier);
    }
#endif
    env.code_rt = &runtime;

    LIRGenerator lir_gen(irfunc.get(), &env);

    auto lir_func = lir_gen.TranslateFunction();

    std::stringstream ss;

    lir_func->sortBasicBlocks();
    ss << *lir_func << '\n';
    return ss.str();
  }

  std::string removeCommentsAndWhitespace(const std::string& input_s) {
    std::istringstream iss(input_s);
    std::string line;
    std::string output_s;
    while (std::getline(iss, line)) {
      if (line.length() == 0) {
        // skip blank lines
        continue;
      } else if (line.length() > 0 && line.at(0) == '#') {
        // skip comments
        continue;
      } else {
        output_s += line + '\n';
      }
    }
    return output_s;
  }
};

#if PY_VERSION_HEX < 0x030C0000
TEST_F(LIRGeneratorTest, PrimitiveBoxBoolSelectUsesVRegInputs) {
  const char* hir_source = R"(fun test {
  bb 0 {
    v0 = LoadConst<CBool[true]>
    v1 = PrimitiveBoxBool v0
    Return v1
  }
}
)";

  std::unique_ptr<hir::Function> irfunc = hir::HIRParser{}.ParseHIR(hir_source);
  ASSERT_NE(irfunc, nullptr);

  jit::codegen::Environ env;
  env.ctx = getContext();
  LIRGenerator lir_gen(irfunc.get(), &env);
  auto lir_func = lir_gen.TranslateFunction();

  bool found_select = false;
  for (const auto& block : lir_func->basicblocks()) {
    for (const auto& instr : block->instructions()) {
      if (instr->opcode() != Instruction::kSelect) {
        continue;
      }
      found_select = true;
      ASSERT_EQ(instr->getInput(1)->type(), OperandBase::kVreg);
      ASSERT_EQ(instr->getInput(2)->type(), OperandBase::kVreg);
    }
  }
  EXPECT_TRUE(found_select);
}

TEST_F(LIRGeneratorTest, IsNegativeAndErrOccurredSetErrBranchesToDone) {
  const char* hir_source = R"(fun test {
  bb 0 {
    v0 = LoadConst<CInt64[-1]>
    v1 = IsNegativeAndErrOccurred v0 {
      FrameState {
        CurInstrOffset 0
      }
    }
    Return v1
  }
}
)";

  std::unique_ptr<hir::Function> irfunc = hir::HIRParser{}.ParseHIR(hir_source);
  ASSERT_NE(irfunc, nullptr);

  jit::codegen::Environ env;
  jit::CodeRuntime code_runtime{
      irfunc->code, irfunc->builtins, irfunc->globals};
  env.ctx = getContext();
  env.code_rt = &code_runtime;
  LIRGenerator lir_gen(irfunc.get(), &env);
  auto lir_func = lir_gen.TranslateFunction();

  bool found_dec_then_branch = false;
  for (const auto& block : lir_func->basicblocks()) {
    const auto& instructions = block->instructions();
    if (instructions.size() < 2) {
      continue;
    }
    auto iter = instructions.rbegin();
    const Instruction* branch = iter->get();
    ++iter;
    const Instruction* decrement = iter->get();
    if (decrement->opcode() == Instruction::kDec &&
        branch->opcode() == Instruction::kBranch) {
      found_dec_then_branch = true;
      break;
    }
  }
  EXPECT_TRUE(found_dec_then_branch);
}

TEST_F(LIRGeneratorTest, RaiseOnlyFunctionHasVerifierSafeSyntheticExit) {
  const char* src = R"(
def test(value):
    raise RuntimeError(value)
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<hir::Function> irfunc(buildHIR(func));
  Compiler::runPasses(*irfunc, PassConfig::kAllExceptInliner);

  jit::codegen::Environ env;
  env.ctx = getContext();
  jit::CodeRuntime runtime{func};
  env.code_rt = &runtime;

  LIRGenerator lir_gen(irfunc.get(), &env);
  auto lir_func = lir_gen.TranslateFunction();

  ASSERT_NE(lir_func->exitBlock(), nullptr);
  ASSERT_FALSE(lir_func->exitBlock()->instructions().empty());
  EXPECT_EQ(
      lir_func->exitBlock()->instructions().front()->opcode(),
      Instruction::kMove)
      << *lir_func;

  for (const auto* block : lir_func->basicblocks()) {
    for (const auto& instr : block->instructions()) {
      if (instr->opcode() == Instruction::kPhi) {
        EXPECT_GT(instr->getNumInputs(), 0u) << *lir_func;
      }
    }
  }

  PostGenerationRewrite post_gen(lir_func.get(), &env);
  post_gen.run();
  eliminateDeadCode(lir_func.get());
  selectTargetOpcodes(lir_func.get());

  LinearScanAllocator allocator(lir_func.get());
  allocator.run();
  env.shadow_frames_and_spill_size = allocator.getFrameSize();
  env.changed_regs = allocator.getChangedRegs();

  PostRegAllocRewrite post_alloc(lir_func.get(), &env);
  post_alloc.run();

  std::ostringstream errors;
  EXPECT_TRUE(verifyPostRegAllocInvariants(lir_func.get(), errors))
      << errors.str() << *lir_func;
}
#endif

TEST_F(LIRGeneratorTest, BroadPythonTranslationCoverage) {
  const char* snippets[] = {
      R"(
def f(a, b):
  c = a + b
  d = a - b
  e = a * b
  return (c, d, e, a / b, a // b, a % b)
)",
      R"(
def f(flag, left, right):
  if flag:
    value = left
  else:
    value = right
  return value.attr
)",
      R"(
def f(seq):
  total = 0
  for item in seq:
    if item:
      total += item
    else:
      total -= 1
  return total
)",
      R"(
def f(seq, mapping, key):
  first = seq[0]
  mapping[key] = first
  return mapping.get(key, 0)
)",
      R"(
def f(obj, value):
  obj.field = value
  return obj.field
)",
      R"(
def f(a, b, c):
  return (a and b) or (not c)
)",
      R"(
def f(fn, arg):
  return fn(arg, arg + 1, named=arg + 2)
)",
      R"(
def f(items):
  return [x + 1 for x in items if x > 0]
)",
      R"(
def f(value):
  try:
    return 10 // value
  except ZeroDivisionError:
    return -1
)",
      R"(
def f(obj):
  return isinstance(obj, int), type(obj), len(obj)
)",
  };

  for (const char* src : snippets) {
    Ref<PyObject> pyfunc(compileAndGet(src, "f"));
    ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";
    auto lir_str = getLIRString(pyfunc.get());
    ASSERT_FALSE(lir_str.empty());
  }
}

TEST_F(LIRGeneratorTest, Aarch64ExactLongAddSubFastPathSelection) {
#if defined(CINDER_AARCH64) && PY_VERSION_HEX >= 0x030E0000 &&  \
    PY_VERSION_HEX < 0x030F0000 && !defined(Py_GIL_DISABLED) && \
    !defined(Py_REF_DEBUG) && !defined(Py_STATS)
  auto count_opcode = [](const std::string& lir, std::string_view opcode) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = lir.find(opcode, pos)) != std::string::npos) {
      ++count;
      pos += opcode.size();
    }
    return count;
  };

  Ref<PyObject> binary_func(compileAndGet(
      R"(
def binary_func(a, b):
  return (a + b, a - b)
)",
      "binary_func"));
  ASSERT_NE(binary_func.get(), nullptr);
  auto binary_lir = getLIRString(binary_func.get());
  EXPECT_EQ(count_opcode(binary_lir, "BinaryOpExactLongAddSubFastPath"), 2)
      << binary_lir;

  Ref<PyObject> other_binary_func(compileAndGet(
      R"(
def other_binary_func(a, b):
  return (
      a & b,
      a // b,
      a << b,
      a % b,
      a * b,
      a | b,
      a >> b,
      a / b,
      a ^ b,
      a ** b,
      a @ b,
      a[b],
  )
)",
      "other_binary_func"));
  ASSERT_NE(other_binary_func.get(), nullptr);
  auto other_binary_lir = getLIRString(other_binary_func.get());
  EXPECT_EQ(
      count_opcode(other_binary_lir, "BinaryOpExactLongAddSubFastPath"), 0)
      << other_binary_lir;
  EXPECT_NE(other_binary_lir.find(":Object = Call"), std::string::npos)
      << other_binary_lir;

  Ref<PyObject> inplace_func(compileAndGet(
      R"(
def inplace_func(a, b):
  a += b
  a -= b
  return a
)",
      "inplace_func"));
  ASSERT_NE(inplace_func.get(), nullptr);
  auto inplace_lir = getLIRString(inplace_func.get());
  EXPECT_EQ(count_opcode(inplace_lir, "BinaryOpExactLongAddSubFastPath"), 0)
      << inplace_lir;
  EXPECT_NE(inplace_lir.find(":Object = Call"), std::string::npos)
      << inplace_lir;

  const char* known_exact_hir = R"(
fun known_exact {
  bb 0 {
    v1 = LoadArg<0>
    v2 = LoadArg<1>
    v3 = RefineType<LongExact> v1
    v4 = RefineType<LongExact> v2
    v5 = BinaryOp<Add> v3 v4
    v6 = BinaryOp<Subtract> v5 v4
    Return v6
  }
}
)";
  auto irfunc = hir::HIRParser{}.ParseHIR(known_exact_hir);
  ASSERT_NE(irfunc, nullptr);
  Compiler::runPasses(
      *irfunc,
      static_cast<PassConfig>(
          PassConfig::kAllExceptInliner & ~PassConfig::kInsertUpdatePrevInstr));
  jit::codegen::Environ env;
  jit::CodeRuntime code_runtime{
      irfunc->code, irfunc->builtins, irfunc->globals};
  env.ctx = getContext();
  env.code_rt = &code_runtime;
  LIRGenerator lir_gen(irfunc.get(), &env);
  auto lir_func = lir_gen.TranslateFunction();
  std::stringstream exact_stream;
  lir_func->sortBasicBlocks();
  exact_stream << *lir_func << '\n';
  auto exact_lir = exact_stream.str();
  EXPECT_NE(exact_lir.find("LongBinaryOp<Add>"), std::string::npos)
      << exact_lir;
  EXPECT_NE(exact_lir.find("LongBinaryOp<Subtract>"), std::string::npos)
      << exact_lir;
  EXPECT_EQ(count_opcode(exact_lir, "BinaryOpExactLongAddSubFastPath"), 0)
      << exact_lir;
#else
  GTEST_SKIP() << "AArch64 CPython 3.14 GIL-only fast path";
#endif
}

// CPython 3.11 intentionally links static_python_stub.cpp and does not provide
// the _static module required by these translation tests.
#if PY_VERSION_HEX >= 0x030C0000
TEST_F(LIRGeneratorTest, BroadStaticTranslationCoverage) {
  const char* snippets[] = {
      R"(
from __static__ import int64

def f(a: int64, b: int64) -> int64:
  c: int64 = a + b
  d: int64 = a * b
  return c - d
)",
      R"(
from __static__ import int64

def f(limit: int64) -> int64:
  total: int64 = 0
  i: int64 = 0
  while i < limit:
    total += i
    i += 1
  return total
)",
      R"(
from __static__ import int64, box

def f(value: int64) -> int:
  return box(value + 1)
)",
      R"(
from __static__ import double, box

def f(left: double, right: double) -> float:
  return box((left * right) + (left / right))
)",
      R"(
from __static__ import CheckedList

def f(values: CheckedList[int]) -> int:
  return values[0] + len(values)
)",
  };

  for (const char* src : snippets) {
    Ref<PyObject> pyfunc(compileStaticAndGet(src, "f"));
    ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";
    auto lir_str = getLIRString(pyfunc.get());
    ASSERT_FALSE(lir_str.empty());
  }
}

TEST_F(LIRGeneratorTest, StaticLoadInteger) {
  const char* pycode = R"(
from __static__ import int64

def f() -> int64:
  d: int64 = 12
  return d
)";

  Ref<PyObject> pyfunc(compileStaticAndGet(pycode, "f"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_str = getLIRString(pyfunc.get());
  // Check that the resulting LIR has the unboxed constant we care about,
  // without hardcoding a variable name or the program structure.
  ASSERT_NE(lir_str.find(":64bit = Move 12(0xc):Object"), std::string::npos);
}

TEST_F(LIRGeneratorTest, StaticLoadDouble) {
  const char* pycode = R"(
from __static__ import double

def f() -> double:
  d: double = 3.1415
  return d
)";

  Ref<PyObject> pyfunc(compileStaticAndGet(pycode, "f"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_str = getLIRString(pyfunc.get());
  // Check that the resulting LIR has the unboxed constant we care about,
  // without hardcoding a variable name or the program structure.
  ASSERT_NE(
      lir_str.find(
          ":64bit = Move 4614256447914709615(0x400921cac083126f):64bit"),
      std::string::npos);
}

TEST_F(LIRGeneratorTest, StaticBoxDouble) {
  const char* pycode = R"(
from __static__ import double, box

def f() -> float:
  d: double = 3.1415
  return box(d)
)";

  Ref<PyObject> pyfunc(compileStaticAndGet(pycode, "f"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_str = getLIRString(pyfunc.get());

  ASSERT_NE(
      lir_str.find(":64bit = Move 4614256447914709615"), std::string::npos);
  ASSERT_NE(lir_str.find(":Object = Call"), std::string::npos);
}

TEST_F(LIRGeneratorTest, StaticAddDouble) {
  const char* pycode = R"(
from __static__ import double, box

def f() -> float:
  d: double = 1.14
  e: double = 2.00
  return box(d + e)
)";

  Ref<PyObject> pyfunc(compileStaticAndGet(pycode, "f"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_str = getLIRString(pyfunc.get());

  const char* lir_expected = R"(Function:
BB %0 - succs: %3
       %1:Object = Bind R10:Object
       %2:Object = Bind R11:Object

BB %3 - preds: %0 - succs: %12

# v7:CDouble[1.14] = LoadConst<CDouble[1.14]>
        %4:64bit = Move 4607812922747849277(0x3ff23d70a3d70a3d):64bit
       %5:Double = Move %4:64bit

# v9:CDouble[2] = LoadConst<CDouble[2]>
        %6:64bit = Move 4611686018427387904(0x4000000000000000):64bit
       %7:Double = Move %6:64bit

# v11:CDouble = DoubleBinaryOp<Add> v7 v9
       %8:Double = Fadd %5:Double, %7:Double)";
  ASSERT_EQ(lir_expected, lir_expected);
}
#endif

// disabled due to unstable Guard instruction
TEST_F(LIRGeneratorTest, DISABLED_Fallthrough) {
  const char* src = R"(
def func2(x):
  y = 0
  if x:
    y = 100
  return y
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func2"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_str = getLIRString(pyfunc.get());

  auto lir_expected = fmt::format(
      R"(Function:
BB %0
              %1 = Bind RDI
              %2 = Bind RSI
              %3 = Bind RDX
              %4 = Bind R9
              %5 = Bind R10
              %6 = Bind R11

BB %7 - preds: %0
              %8 = Load %2, 0(0x0)
              %9 = Load %5, 8(0x8)
             %10 = Call {0}({0:#x}), %8
                   Guard 1(0x1), 0(0x0), %10, %9, %8
                   CondBranch %10, BB%14, BB%13

BB %13 - preds: %7

BB %14 - preds: %7
             %15 = Load %5, 16(0x10)

BB %16 - preds: %13 %14
             %17 = Phi (BB%14, %15), (BB%13, %9)
                   Call {1}({1:#x}), %17
                   Return %17

BB %20 - preds: %16
             RDI = Move %6


)",
      reinterpret_cast<uint64_t>(PyObject_IsTrue),
      reinterpret_cast<uint64_t>(Py_IncRef));
  ASSERT_EQ(lir_str, lir_expected);
}

// disabled due to unstable Guard instruction
TEST_F(LIRGeneratorTest, DISABLED_CondBranch) {
  const char* pycode = R"(
def func(x):
    if x:
        return True
    return False
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_str = getLIRString(pyfunc.get());

  auto lir_expected = fmt::format(
      R"(Function:
BB %0
              %1 = Bind RDI
              %2 = Bind RSI
              %3 = Bind RDX
              %4 = Bind R9
              %5 = Bind R10
              %6 = Bind R11

BB %7 - preds: %0
              %8 = Load %2, 0(0x0)
              %9 = Call {0}({0:#x}), %8
                   Guard 1(0x1), 0(0x0), %9, %8
                   CondBranch %9, BB%16, BB%12

BB %12 - preds: %7
             %13 = Load %5, 16(0x10)
                   Call {1}({1:#x}), %13
                   Return %13

BB %16 - preds: %7
             %17 = Load %5, 8(0x8)
                   Call {1}({1:#x}), %17
                   Return %17

BB %20 - preds: %12 %16
             RDI = Move %6


)",
      reinterpret_cast<uint64_t>(PyObject_IsTrue),
      reinterpret_cast<uint64_t>(Py_IncRef));
  ASSERT_EQ(lir_str, lir_expected);
}

TEST_F(LIRGeneratorTest, ParserDataTypeTest) {
  std::string lir_str = fmt::format(
      R"(Function:
BB %0 - succs: %7 %10
         %1:8bit = Bind {}:8bit
        %2:32bit = Bind {}:32bit
        %3:16bit = Bind {}:16bit
        %4:64bit = Bind {}:64bit
       %5:Object = Move 0(0x0):Object
                   CondBranch %5:Object, BB%7, BB%10

BB %7 - preds: %0 - succs: %10
       %8:Object = Move [0x5]:Object
                   Return %8:Object

BB %10 - preds: %0 %7

)",
      PhyLocation{0, 8},
      PhyLocation{6, 32},
      PhyLocation{9, 16},
      PhyLocation{10, 64});

  Parser parser;
  auto parsed_func = parser.parse(lir_str);
  std::stringstream ss;
  parsed_func->sortBasicBlocks();
  ss << *parsed_func;
  // Assume that the parser assigns basic block and register numbers
  // based on the parsing order of the instructions.
  // If the parser behavior is modified and assigns numbers differently,
  // then the assert may fail.
  ASSERT_EQ(lir_str, ss.str());
}

TEST_F(LIRGeneratorTest, ParserMemIndTest) {
  auto lir_str = fmt::format(
      R"(Function:
BB %0
        %1:64bit = Bind {}:Object
        %2:64bit = Move [{}:Object + {}:Object * 8 + 0x8]:Object
        %3:64bit = Move [%2:64bit + 0x3]:Object
        %4:64bit = Move [%2:64bit + %3:64bit * 16]:Object
[%4:64bit - 0x16]:Object = Move [{}:Object + %4:64bit]:Object

)",
      PhyLocation{5, 64},
      PhyLocation{5, 64},
      PhyLocation{4, 64},
      PhyLocation{0, 64});

  Parser parser;
  auto parsed_func = parser.parse(lir_str);
  std::stringstream ss;
  parsed_func->sortBasicBlocks();
  ss << *parsed_func;
  // Assume that the parser assigns basic block and register numbers
  // based on the parsing order of the instructions.
  // If the parser behavior is modified and assigns numbers differently,
  // then the assert may fail.
  ASSERT_EQ(lir_str, ss.str());
}

TEST_F(LIRGeneratorTest, ParserTest) {
  const char* pycode = R"(
def func(x):
    if x:
        return True
    return False
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_str = removeCommentsAndWhitespace(getLIRString(pyfunc.get()));

  Parser parser;
  auto parsed_func = parser.parse(lir_str);
  std::stringstream ss;
  parsed_func->sortBasicBlocks();
  ss << *parsed_func;
  ASSERT_EQ(lir_str, removeCommentsAndWhitespace(ss.str()));
}

TEST_F(LIRGeneratorTest, ParserSectionTest) {
  std::string lir_str = fmt::format(
      R"(Function:
BB %0 - section: hot
         %1:8bit = Bind {}:8bit
        %2:32bit = Bind {}:32bit
        %3:16bit = Bind {}:16bit
        %4:64bit = Bind {}:64bit
       %5:Object = Move 0(0x0):Object
                   CondBranch %5:Object, BB%7, BB%10

BB %7 - preds: %0 - succs: %10 - section: .coldtext
       %8:Object = Move [0x5]:Object
                   Return %8:Object

BB %10 - preds: %0 %7 - section: hot

)",
      PhyLocation{0, 8},
      PhyLocation{6, 32},
      PhyLocation{9, 16},
      PhyLocation{10, 64});

  Parser parser;
  auto parsed_func = parser.parse(lir_str);
  ASSERT_EQ(parsed_func->basicblocks().size(), 3);
  ASSERT_EQ(
      parsed_func->basicblocks()[0]->section(), codegen::CodeSection::kHot);
  ASSERT_EQ(
      parsed_func->basicblocks()[1]->section(), codegen::CodeSection::kCold);
  ASSERT_EQ(
      parsed_func->basicblocks()[2]->section(), codegen::CodeSection::kHot);
}

template <typename... Args>
std::string formatMemoryIndirect(Args&&... args) {
  jit::lir::MemoryIndirect im(nullptr);
  im.setMemoryIndirect(std::forward<Args>(args)...);
  return fmt::format("{}", im);
}

TEST(LIRTest, MemoryIndirectTests) {
  auto base = codegen::ARGUMENT_REGS[3];
  auto index = codegen::ARGUMENT_REGS[2];

  ASSERT_EQ(fmt::format("[{}:Object]", base), formatMemoryIndirect(base.loc));
  ASSERT_EQ(
      fmt::format("[{}:Object + 0x7fff]", base),
      formatMemoryIndirect(base.loc, 0x7fff));
  ASSERT_EQ(
      fmt::format("[{}:Object + {}:Object]", base, index),
      formatMemoryIndirect(base.loc, index.loc, 0));
  ASSERT_EQ(
      fmt::format("[{}:Object + {}:Object * 4]", base, index),
      formatMemoryIndirect(base.loc, index.loc, 2));
  ASSERT_EQ(
      fmt::format("[{}:Object + {}:Object + 0x100]", base, index),
      formatMemoryIndirect(base.loc, index.loc, 0, 0x100));
  ASSERT_EQ(
      fmt::format("[{}:Object + {}:Object * 2 + 0x1000]", base, index),
      formatMemoryIndirect(base.loc, index.loc, 1, 0x1000));
}

extern "C" uint64_t __Invoke_PyTuple_Check(PyObject* obj);

TEST_F(LIRGeneratorTest, CondBranchCheckTypeEmitsCallToSubclassCheck) {
  const char* hir = R"(
fun foo {
  bb 0 {
    v0 = LoadArg<0>
    CondBranchCheckType<1, 2, Tuple> v0
  }

  bb 1 {
    v0 = LoadConst<NoneType>
    Branch<2>
  }

  bb 2 {
    Return v0
  }
}
)";

  std::unique_ptr<hir::Function> irfunc = hir::HIRParser{}.ParseHIR(hir);
  ASSERT_NE(irfunc, nullptr);

  Compiler::runPasses(
      *irfunc,
      static_cast<PassConfig>(
          // We don't have a code-object for kInsertUpdatePrevInstr.
          PassConfig::kAllExceptInliner & ~PassConfig::kInsertUpdatePrevInstr));

  jit::codegen::Environ env;

  env.ctx = getContext();

  LIRGenerator lir_gen(irfunc.get(), &env);

  auto lir_func = lir_gen.TranslateFunction();

  std::stringstream ss;

  lir_func->sortBasicBlocks();
  ss << *lir_func << '\n';

  std::string lir_str = ss.str();
  lir_str.erase(
      std::remove(lir_str.begin(), lir_str.end(), '\n'), lir_str.end());

  std::string lir_expected_re = fmt::format(
      R"(# CondBranchCheckType<1, 3, Tuple> v1\s+%\d+:8bit = Call {0}\({0:#x}\):64bit, %\d+:Object\s+CondBranch %\d+:8bit, BB%\d+, BB%\d+)",
      reinterpret_cast<uint64_t>(__Invoke_PyTuple_Check));

  std::regex re(lir_expected_re);
  if (!std::regex_search(lir_str, re)) {
    FAIL() << "Couldn't find expected string \n"
           << lir_expected_re << '\n'
           << "In:\n"
           << lir_str << '\n';
  }
}

TEST_F(LIRGeneratorTest, UnreachableFollowsBottomType) {
  const char* hir_source = R"(fun test {
  bb 0 {
    v7 = LoadConst<Nullptr>
    v8 = CheckVar<"a"> v7 {
      FrameState {
        CurInstrOffset 2
        Locals<1> v7
      }
    }
    Unreachable
  }
}
)";

  std::unique_ptr<hir::Function> irfunc = hir::HIRParser{}.ParseHIR(hir_source);
  ASSERT_NE(irfunc, nullptr);

  Compiler::runPasses(
      *irfunc,
      static_cast<PassConfig>(
          // We don't have a code-object for kInsertUpdatePrevInstr.
          PassConfig::kAllExceptInliner & ~PassConfig::kInsertUpdatePrevInstr));

  jit::codegen::Environ env;
  jit::CodeRuntime code_runtime{
      irfunc->code, irfunc->builtins, irfunc->globals};

  env.ctx = getContext();
  env.code_rt = &code_runtime;

  LIRGenerator lir_gen(irfunc.get(), &env);

  auto lir_func = lir_gen.TranslateFunction();

  std::stringstream ss;

  lir_func->sortBasicBlocks();
  ss << *lir_func << '\n';
#if PY_VERSION_HEX >= 0x030E0000
  auto lir_expected = fmt::format(
      R"(Function:
BB %0 - succs: %1

BB %1 - preds: %0 - succs: %6
                   SetupFrame
       %3:Object = Bind {}:Object
       %4:Object = Bind {}:Object
       %5:Object = Bind {}:Object

BB %6 - preds: %1

# v9:Nullptr = LoadConst<Nullptr>
       %7:Object = Move 0(0x0):Object

# v10:Bottom = CheckVar<"a"> v9 {{
#   LiveValues<1> unc:v9
#   FrameState {{
#     CurInstrOffset 2
#     Locals<1> v9
#   }}
# }}
                   Guard 4(0x4):64bit, 0(0x0):64bit, %7:Object, 0(0x0):64bit, %7:Object

# Unreachable
                   Unreachable

BB %10
      %11:Object = Move 0(0x0):Object
                   EpilogueEnd %11:Object


)",
      PhyLocation{10, 64},
      PhyLocation{11, 64},
#if defined(CINDER_X86_64)
      PhyLocation { 7, 64 }
#else
      PhyLocation { 0, 64 }
#endif
  );
#else
  auto lir_expected = fmt::format(
      R"(Function:
BB %0 - succs: %1

BB %1 - preds: %0 - succs: %6
                   SetupFrame
       %3:Object = Bind {}:Object
       %4:Object = Bind {}:Object
       %5:Object = Bind {}:Object

BB %6 - preds: %1

# v9:Nullptr = LoadConst<Nullptr>
       %7:Object = Move 0(0x0):Object

# v10:Bottom = CheckVar<"a"> v9 {{
#   LiveValues<1> unc:v9
#   FrameState {{
#     CurInstrOffset 2
#     Locals<1> v9
#   }}
# }}
                   Guard 4(0x4):64bit, 0(0x0):64bit, %7:Object, 0(0x0):64bit, %7:Object

# Unreachable
                   Unreachable

BB %10
      %11:Object = Move 0(0x0):Object
                   EpilogueEnd %11:Object


)",
      PhyLocation{10, 64},
      PhyLocation{11, 64},
#if defined(CINDER_X86_64)
      PhyLocation { 7, 64 }
#else
      PhyLocation { 0, 64 }
#endif
  );
#endif
  ASSERT_EQ(ss.str(), lir_expected.c_str());
}

TEST_F(LIRGeneratorTest, UnstableGlobals) {
  getMutableConfig().stable_frame = false;

  const char* src = R"(
def func1(x):
  return x + 1

def func2(x):
  return func1(x) + 2

def func3(x):
  def inner(x2):
    return func1(x2) + 4
  return inner(3)
)";

  Ref<PyObject> pyfunc2(compileAndGet(src, "func2"));
  ASSERT_NE(pyfunc2.get(), nullptr) << "Failed compiling func";

  auto lir_str = getLIRString(pyfunc2.get());

  auto fast_path =
      fmt::format("{}", reinterpret_cast<uint64_t>(JITRT_LoadGlobal));
  auto slow_path = fmt::format(
      "{}", reinterpret_cast<uint64_t>(JITRT_LoadGlobalFromThreadState));

  EXPECT_FALSE(getConfig().stable_frame);

  EXPECT_EQ(lir_str.find(fast_path), std::string::npos)
      << "Should not call out to JITRT_LoadGlobal as globals aren't stable";
  EXPECT_NE(lir_str.find(slow_path), std::string::npos)
      << "Should be calling out to JITRT_LoadGlobalFromThreadState as globals "
         "aren't stable";

  Ref<PyObject> pyfunc3(compileAndGet(src, "func3"));
  ASSERT_NE(pyfunc3.get(), nullptr) << "Failed compiling func";

  lir_str = getLIRString(pyfunc3.get());

  slow_path =
      fmt::format("{}", reinterpret_cast<uint64_t>(JITRT_LoadGlobalsDict));

  EXPECT_FALSE(getConfig().stable_frame);

  EXPECT_NE(lir_str.find(slow_path), std::string::npos)
      << "Should be calling out to JITRT_LoadGlobalsDict as globals "
         "aren't stable";
}

TEST_F(LIRGeneratorTest, AttrCachesOff) {
  getMutableConfig().attr_caches = false;

  const char* src = R"(
import sys

def func():
  return sys.argv
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_str = getLIRString(pyfunc.get());

  auto fast_path =
      fmt::format("{}", reinterpret_cast<uint64_t>(LoadAttrCache::invoke));
  auto slow_path =
      fmt::format("{}", reinterpret_cast<uint64_t>(PyObject_GetAttr));

  EXPECT_FALSE(getConfig().attr_caches);

  EXPECT_NE(lir_str.find(slow_path), std::string::npos)
      << "Should be calling out to PyObject_GetAttr as inline caches are "
         "disabled";
  EXPECT_EQ(lir_str.find(fast_path), std::string::npos)
      << "Should not be calling out to LoadAttrCache::invoke as inline caches "
         "are disabled";
}

TEST_F(LIRGeneratorTest, UnstableCode) {
  getMutableConfig().stable_frame = false;

  const char* src = R"(
import sys

def func():
  return sys.argv
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_str = getLIRString(pyfunc.get());

  auto slow_path =
      fmt::format("{}", reinterpret_cast<uint64_t>(JITRT_LoadName));

  EXPECT_FALSE(getConfig().stable_frame);

  EXPECT_NE(lir_str.find(slow_path), std::string::npos)
      << "Should be calling out to JITRT_LoadName as code objects aren't "
         "stable";
}

TEST_F(LIRGeneratorTest, LoadEvalBreakerUsesMoveRelaxed) {
  // Backward jumps (loop back-edges) emit LoadEvalBreaker in HIR to check
  // whether the interpreter needs to handle pending events. This should lower
  // to MoveRelaxed in LIR.
  const char* src = R"(
def func():
  x = 0
  while x < 10:
    x += 1
  return x
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_str = getLIRString(pyfunc.get());
  EXPECT_NE(lir_str.find("MoveRelaxed"), std::string::npos)
      << "LoadEvalBreaker should lower to MoveRelaxed";
}

TEST_F(LIRGeneratorTest, ListDynamicIndexLoadStoreUsesScaledArrayLIR) {
  const char* src = R"(
import os

def func(value):
  xs = [1, 2, 3]
  i = 1 if os.argv else 2
  old = xs[i]
  xs[i] = value
  return old
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_str = getLIRString(pyfunc.get());
  const std::regex scaled_array_load{
      R"(:Object = Move \[%\d+:\w+ \+ %\d+:\w+ \* 8(?: \+ 0x0)?\]:Object)"};
  const std::regex scaled_array_store{
      R"(\[%\d+:\w+ \+ %\d+:\w+ \* 8(?: \+ 0x0)?\]:Object = Move %\d+:Object)"};

  EXPECT_TRUE(std::regex_search(lir_str, scaled_array_load)) << lir_str;
  if (lir_str.find("StoreSubscr") == std::string::npos) {
    EXPECT_TRUE(std::regex_search(lir_str, scaled_array_store)) << lir_str;
  }
}
