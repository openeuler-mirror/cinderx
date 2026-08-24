// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/python.h"

#include <gtest/gtest.h>

#include "cinderx/Common/code.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Interpreter/cinder_opcode.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/inliner.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/Jit/hir/phi_elimination.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/hir/refcount_insertion.h"
#include "cinderx/Jit/hir/simplify.h"
#include "cinderx/Jit/hir/ssa.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#if PY_VERSION_HEX >= 0x030C0000
#include "internal/pycore_intrinsics.h"
#endif
}

using namespace jit;
using namespace jit::hir;

HIRPrinter fullPrinter() {
  return HIRPrinter{}.setFullSnapshots(true);
}

size_t countSubstring(const std::string& haystack, const std::string& needle) {
  size_t count = 0;
  size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

size_t countOpcode(const Function& func, Opcode opcode) {
  size_t count = 0;
  for (const auto& block : func.cfg.blocks) {
    for (const auto& instr : block) {
      if (instr.opcode() == opcode) {
        count++;
      }
    }
  }
  return count;
}

void expectColdGlobalGuardShape(
    const Function& func,
    const std::string& hir,
    size_t expected_guard_type,
    size_t expected_guard_is) {
#if PY_VERSION_HEX < 0x030C0000
  // Combined unicode globals take the module-value fast path (CallStatic
  // + Guard).  Cold split dictionaries still emit LoadGlobal.  Neither
  // shape uses GuardType/GuardIs; those are the 3.14 cached-global
  // sentinels this helper originally described.
  (void)expected_guard_type;
  (void)expected_guard_is;
  size_t load_global = countOpcode(func, Opcode::kLoadGlobal);
  size_t guards = countOpcode(func, Opcode::kGuard);
  EXPECT_TRUE(load_global == 1 || guards >= 1) << hir;
  EXPECT_EQ(countSubstring(hir, "GuardType<"), 0) << hir;
  EXPECT_EQ(countSubstring(hir, "GuardIs<"), 0) << hir;
#else
  (void)func;
  EXPECT_EQ(countSubstring(hir, "GuardType<LongExact>"), expected_guard_type)
      << hir;
  EXPECT_EQ(countSubstring(hir, "GuardIs<"), expected_guard_is) << hir;
#endif
}

std::vector<const GuardType*> guardTypesWithFrameState(const Function& func) {
  std::vector<const GuardType*> guards;
  for (const auto& block : func.cfg.blocks) {
    for (const auto& instr : block) {
      if (instr.IsGuardType()) {
        const auto& guard = static_cast<const GuardType&>(instr);
        if (guard.frameState() != nullptr) {
          guards.push_back(&guard);
        }
      }
    }
  }
  return guards;
}

bool isEntrySetupInstrForTest(
    const BytecodeInstruction& instr,
    bool initial_yield_value_on_stack) {
  switch (instr.opcode()) {
    case COPY_FREE_VARS:
    case GEN_START:
    case MAKE_CELL:
    case NOP:
    case NOT_TAKEN:
    case RESUME:
    case RETURN_GENERATOR:
      return true;
    case POP_TOP:
      return initial_yield_value_on_stack;
    default:
      return false;
  }
}

BCOffset firstNonEntrySetupOffset(BorrowedRef<PyCodeObject> code) {
  bool initial_yield_value_on_stack = false;
  for (const auto& instr : BytecodeInstructionBlock{code}) {
    if (!isEntrySetupInstrForTest(instr, initial_yield_value_on_stack)) {
      return instr.baseOffset();
    }

    int opcode = instr.opcode();
    if (opcode == RETURN_GENERATOR) {
      initial_yield_value_on_stack = true;
    } else if (opcode == POP_TOP) {
      initial_yield_value_on_stack = false;
    }
  }
  return BCOffset{countIndices(code)};
}

bool entrySetupPrefixContains(BorrowedRef<PyCodeObject> code, int opcode) {
  bool initial_yield_value_on_stack = false;
  for (const auto& instr : BytecodeInstructionBlock{code}) {
    if (!isEntrySetupInstrForTest(instr, initial_yield_value_on_stack)) {
      return false;
    }
    if (instr.opcode() == opcode) {
      return true;
    }

    int instr_opcode = instr.opcode();
    if (instr_opcode == RETURN_GENERATOR) {
      initial_yield_value_on_stack = true;
    } else if (instr_opcode == POP_TOP) {
      initial_yield_value_on_stack = false;
    }
  }
  return false;
}

void moveEntryResumeBeforeInitialYieldPop(BorrowedRef<PyCodeObject> code) {
  int resume_idx = -1;
  int pop_top_idx = -1;
  bool saw_return_generator = false;
  for (const auto& instr : BytecodeInstructionBlock{code}) {
    int opcode = instr.opcode();
    if (opcode == RETURN_GENERATOR) {
      saw_return_generator = true;
      continue;
    }
    if (!saw_return_generator) {
      continue;
    }
    if (opcode == POP_TOP) {
      pop_top_idx = instr.baseIndex().value();
    } else if (opcode == RESUME) {
      resume_idx = instr.baseIndex().value();
    }
    if (resume_idx != -1 && pop_top_idx != -1) {
      break;
    }
  }

  ASSERT_NE(pop_top_idx, -1);
  ASSERT_NE(resume_idx, -1);
  ASSERT_LT(pop_top_idx, resume_idx);
  std::swap(codeUnit(code)[pop_top_idx], codeUnit(code)[resume_idx]);
}

void assertEntrySelfGuardAfterSetup(
    BorrowedRef<PyFunctionObject> method,
    const Function& irfunc,
    std::initializer_list<int> required_prefix_opcodes) {
  auto guards = guardTypesWithFrameState(irfunc);
  ASSERT_EQ(guards.size(), 1) << fullPrinter().ToString(irfunc);

  BorrowedRef<PyCodeObject> code{method->func_code};
  BCOffset expected_offset = firstNonEntrySetupOffset(code);
  const FrameState* frame_state = guards.front()->frameState();
  ASSERT_NE(frame_state, nullptr);
  EXPECT_EQ(frame_state->instrOffset(), expected_offset)
      << fullPrinter().ToString(irfunc);
  for (int opcode : required_prefix_opcodes) {
    EXPECT_TRUE(entrySetupPrefixContains(code, opcode))
        << "expected " << opcodeName(opcode)
        << " in the entry setup prefix";
  }
}

size_t countStaticCallsTo(const Function& func, void* addr) {
  size_t count = 0;
  for (const auto& block : func.cfg.blocks) {
    for (const auto& instr : block) {
      if (instr.IsCallStatic() &&
          static_cast<const CallStatic&>(instr).addr() == addr) {
        count++;
      }
    }
  }
  return count;
}

const CallStatic* findStaticCallTo(const Function& func, void* addr) {
  for (const auto& block : func.cfg.blocks) {
    for (const auto& instr : block) {
      if (instr.IsCallStatic()) {
        const auto& call = static_cast<const CallStatic&>(instr);
        if (call.addr() == addr) {
          return &call;
        }
      }
    }
  }
  return nullptr;
}

bool hasSpecializedOpcode(BorrowedRef<PyFunctionObject> func, int opcode) {
  BorrowedRef<PyCodeObject> code{func->func_code};
  for (const auto& instr : BytecodeInstructionBlock{code}) {
    if (instr.specializedOpcode() == opcode) {
      return true;
    }
  }
  return false;
}

TEST(BasicBlockTest, CanAppendInstrs) {
  Environment env;
  BasicBlock block;
  auto v0 = env.AllocateRegister();
  block.append<LoadConst>(v0, TNoneType);
  block.append<Return>(v0);
  ASSERT_TRUE(block.GetTerminator()->IsReturn());
}

TEST(BasicBlockTest, CanIterateInstrs) {
  Environment env;
  BasicBlock block;
  auto v0 = env.AllocateRegister();
  block.append<LoadConst>(v0, TNoneType);
  block.append<Return>(v0);

  auto it = block.begin();
  ASSERT_TRUE(it->IsLoadConst());
  it++;
  ASSERT_TRUE(it->IsReturn());
  it++;
  ASSERT_TRUE(it == block.end());
}

TEST(BasicBlockTest, SplitAfterSplitsBlockAfterInstruction) {
  Environment env;
  CFG cfg;
  BasicBlock* head = cfg.AllocateBlock();
  auto v0 = env.AllocateRegister();
  head->append<LoadConst>(v0, TNoneType);
  Instr* load_const = head->GetTerminator();
  head->append<Return>(v0);
  BasicBlock* tail = cfg.splitAfter(*load_const);
  ASSERT_NE(nullptr, head->GetTerminator());
  EXPECT_TRUE(head->GetTerminator()->IsLoadConst());
  ASSERT_NE(nullptr, tail->GetTerminator());
  EXPECT_TRUE(tail->GetTerminator()->IsReturn());
}

TEST(CFGIterTest, IteratingEmptyCFGReturnsEmptyTraversal) {
  CFG cfg;
  std::vector<BasicBlock*> traversal = cfg.GetRPOTraversal();
  ASSERT_EQ(traversal.size(), 0);
}

TEST(CFGIterTest, IteratingSingleBlockCFGReturnsOneBlock) {
  Environment env;
  CFG cfg;
  BasicBlock* block = cfg.AllocateBlock();
  cfg.entry_block = block;

  // Add a single instuction to the block
  block->append<Return>(env.AllocateRegister());

  std::vector<BasicBlock*> traversal = cfg.GetRPOTraversal();
  ASSERT_EQ(traversal.size(), 1) << "Incorrect number of blocks returned";
  ASSERT_EQ(traversal[0], block) << "Incorrect block returned";
}

TEST(CFGIterTest, VisitsBlocksOnlyOnce) {
  CFG cfg;
  BasicBlock* block = cfg.AllocateBlock();
  cfg.entry_block = block;

  // The block loops on itself
  block->append<Branch>(block);

  std::vector<BasicBlock*> traversal = cfg.GetRPOTraversal();
  ASSERT_EQ(traversal.size(), 1) << "Incorrect number of blocks returned";
  ASSERT_EQ(traversal[0], block) << "Incorrect block returned";
}

TEST(CFGIterTest, VisitsAllBranches) {
  Environment env;
  CFG cfg;
  BasicBlock* cond = cfg.AllocateBlock();
  cfg.entry_block = cond;

  BasicBlock* true_block = cfg.AllocateBlock();
  true_block->append<Return>(env.AllocateRegister());

  BasicBlock* false_block = cfg.AllocateBlock();
  false_block->append<Return>(env.AllocateRegister());

  cond->append<CondBranch>(env.AllocateRegister(), true_block, false_block);

  std::vector<BasicBlock*> traversal = cfg.GetRPOTraversal();
  ASSERT_EQ(traversal.size(), 3) << "Incorrect number of blocks returned";
  ASSERT_EQ(traversal[0], cond) << "Should have visited cond block first";
  ASSERT_EQ(traversal[1], true_block)
      << "Should have visited true block second";
  ASSERT_EQ(traversal[2], false_block)
      << "Should have visited false block last";
}

TEST(CFGIterTest, VisitsLoops) {
  Environment env;
  CFG cfg;

  // Create the else block
  BasicBlock* outer_else = cfg.AllocateBlock();
  outer_else->append<Return>(env.AllocateRegister());

  // Create the inner loop
  BasicBlock* loop_cond = cfg.AllocateBlock();
  BasicBlock* loop_body = cfg.AllocateBlock();
  loop_body->append<Branch>(loop_cond);
  loop_cond->append<CondBranch>(env.AllocateRegister(), loop_body, outer_else);

  // Create the outer conditional
  BasicBlock* outer_cond = cfg.AllocateBlock();
  outer_cond->append<CondBranch>(env.AllocateRegister(), loop_cond, outer_else);
  cfg.entry_block = outer_cond;

  std::vector<BasicBlock*> traversal = cfg.GetRPOTraversal();
  ASSERT_EQ(traversal.size(), 4) << "Incorrect number of blocks returned";
  ASSERT_EQ(traversal[0], outer_cond) << "Should have visited outer cond first";
  ASSERT_EQ(traversal[1], loop_cond) << "Should have visited loop cond second";
  ASSERT_EQ(traversal[2], loop_body) << "Should have visited loop body third";
  ASSERT_EQ(traversal[3], outer_else) << "Should have visited else block last";
}

TEST(SplitCriticalEdgesTest, SplitsCriticalEdges) {
  auto hir_source = R"(
fun test {
  bb 0 {
    v0 = LoadConst<NoneType>
    CondBranch<1, 2> v0
  }
  bb 1 {
    v1 = LoadConst<NoneType>
    Branch<2>
  }
  bb 2 {
    v2 = Phi<0, 1> v0 v1
    CondBranch<3, 5> v2
  }
  bb 3 {
    Branch<5>
  }
  bb 5 {
    Return v2
  }
}
)";
  auto func = HIRParser{}.ParseHIR(hir_source);
  ASSERT_NE(func, nullptr);
  ASSERT_TRUE(checkFunc(*func, std::cout));

  func->cfg.splitCriticalEdges();
  const char* expected_hir = R"(fun test {
  bb 0 {
    v0 = LoadConst<NoneType>
    CondBranch<1, 5> v0
  }

  bb 1 (preds 0) {
    v1 = LoadConst<NoneType>
    Branch<2>
  }

  bb 5 (preds 0) {
    Branch<2>
  }

  bb 2 (preds 1, 5) {
    v2 = Phi<1, 5> v1 v0
    CondBranch<3, 6> v2
  }

  bb 3 (preds 2) {
    Branch<5>
  }

  bb 6 (preds 2) {
    Branch<5>
  }

  bb 5 (preds 3, 6) {
    Return v2
  }
}
)";
  EXPECT_EQ(HIRPrinter{}.ToString(*func), expected_hir);
}

TEST(RemoveTrampolineBlocksTest, DoesntModifySingleBlockLoops) {
  CFG cfg;
  Environment env;

  cfg.entry_block = cfg.AllocateBlock();
  cfg.entry_block->append<Branch>(cfg.entry_block);

  removeTrampolineBlocks(&cfg);

  auto s = HIRPrinter().ToString(cfg);
  const char* expected = R"(bb 0 (preds 0) {
  Branch<0>
}
)";
  ASSERT_EQ(s, expected);
}

TEST(RemoveTrampolineBlocksTest, ReducesSimpleLoops) {
  CFG cfg;
  Environment env;

  auto t1 = cfg.AllocateBlock();
  cfg.entry_block = cfg.AllocateBlock();
  cfg.entry_block->append<Branch>(t1);
  t1->append<Branch>(cfg.entry_block);

  removeTrampolineBlocks(&cfg);

  auto s = HIRPrinter().ToString(cfg);
  const char* expected = R"(bb 1 (preds 1) {
  Branch<1>
}
)";
  ASSERT_EQ(s, expected);
}

TEST(RemoveTrampolineBlocksTest, RemovesSimpleChain) {
  CFG cfg;
  Environment env;

  // This constructs a CFG that looks like
  //
  // entry -> t2 -> t1 -> exit
  //
  // after removing tramponline blocks we should be left
  // with only the exit block
  auto exit_block = cfg.AllocateBlock();
  exit_block->append<Return>(env.AllocateRegister());

  auto t1 = cfg.AllocateBlock();
  t1->append<Branch>(exit_block);

  auto t2 = cfg.AllocateBlock();
  t2->append<Branch>(t1);

  cfg.entry_block = cfg.AllocateBlock();
  cfg.entry_block->append<Branch>(t2);

  removeTrampolineBlocks(&cfg);

  auto s = HIRPrinter().ToString(cfg);
  auto expected = R"(bb 0 {
  Return v0
}
)";
  ASSERT_EQ(s, expected);
}

TEST(RemoveTrampolineBlocksTest, ReducesLoops) {
  CFG cfg;
  Environment env;

  // This constructs a CFG that look like
  //
  //              entry
  //                |
  //   +--- true ---+--- false ---+
  //   |                          |
  //  exit                        1->2->3->4-+
  //                                 ^       |
  //                                 |       |
  //                                 +-------+
  //
  // the loop of trampoline blocks on the right should be
  // reduced to a single block that loops back on itself:
  //
  //              entry
  //                |
  //   +--- true ---+--- false ---+
  //   |                          |
  //  exit                        4--+
  //                              ^  |
  //                              |  |
  //                              +--+
  Register* v0 = env.AllocateRegister();
  auto exit_block = cfg.AllocateBlock();
  exit_block->append<Return>(v0);

  auto t1 = cfg.AllocateBlock();
  auto t2 = cfg.AllocateBlock();
  auto t3 = cfg.AllocateBlock();
  auto t4 = cfg.AllocateBlock();
  t1->append<Branch>(t2);
  t2->append<Branch>(t3);
  t3->append<Branch>(t4);
  t4->append<Branch>(t2);

  cfg.entry_block = cfg.AllocateBlock();
  cfg.entry_block->append<CondBranch>(v0, exit_block, t1);

  removeTrampolineBlocks(&cfg);

  auto after = HIRPrinter().ToString(cfg);
  const char* expected = R"(bb 5 {
  CondBranch<0, 4> v0
}

bb 0 (preds 5) {
  Return v0
}

bb 4 (preds 4, 5) {
  Branch<4>
}
)";
  ASSERT_EQ(after, expected);
}

TEST(RemoveTrampolineBlocksTest, UpdatesAllPredecessors) {
  CFG cfg;
  Environment env;

  // This constructs a CFG that look like
  //
  //              entry
  //                |
  //   +--- true ---+--- false ---+
  //   |                          |
  //   4                          3
  //   |                          |
  //   +----------->2<------------+
  //                |
  //                v
  //                1
  //                |
  //                v
  //               exit
  //
  // After removing trampoline blocks this should look like
  //
  //              entry
  //                |
  //                v
  //               exit
  Register* v0 = env.AllocateRegister();
  auto exit_block = cfg.AllocateBlock();
  exit_block->append<Return>(v0);

  auto t1 = cfg.AllocateBlock();
  t1->append<Branch>(exit_block);

  auto t2 = cfg.AllocateBlock();
  t2->append<Branch>(t1);

  auto t3 = cfg.AllocateBlock();
  t3->append<Branch>(t2);

  auto t4 = cfg.AllocateBlock();
  t4->append<Branch>(t2);

  cfg.entry_block = cfg.AllocateBlock();
  cfg.entry_block->append<CondBranch>(v0, t4, t3);

  removeTrampolineBlocks(&cfg);

  auto after = HIRPrinter().ToString(cfg);
  const char* expected = R"(bb 5 {
  Branch<0>
}

bb 0 (preds 5) {
  Return v0
}
)";
  ASSERT_EQ(after, expected);
}

TEST(RemoveUnreachableBlocks, RemovesTransitivelyUnreachableBlocks) {
  const char* hir = R"(
fun foo {
  bb 0 {
    Branch<1>
  }

  bb 2 {
    Branch<2>
  }

  bb 3 {
    Branch<2>
  }

  bb 1 {
    v0 = LoadConst<NoneType>
    Return v0
  }

  bb 12 {
    Branch<11>
  }

  bb 11 {
    v1 = LoadConst<NoneType>
    Return v1
  }

  bb 4 {
    Branch<2>
  }

  bb 10 {
    Branch<1>
  }
}
)";

  std::unique_ptr<Function> func = HIRParser{}.ParseHIR(hir);
  ASSERT_NE(func, nullptr);

  removeUnreachableBlocks(*func);

  const char* expected = R"(fun foo {
  bb 0 {
    Branch<1>
  }

  bb 1 (preds 0) {
    v0 = LoadConst<NoneType>
    Return v0
  }
}
)";
  EXPECT_EQ(HIRPrinter{}.ToString(*func), expected);
}

TEST(RemoveUnreachableBlocks, FixesPhisOfReachableBlocks) {
  const char* hir = R"(
fun foo {
  bb 0 {
    v0 = LoadConst<NoneType>
    CondBranch<1, 3> v0
  }

  bb 1 {
    v1 = LoadConst<NoneType>
    Branch<3>
  }

  bb 2 {
    v2 = LoadConst<NoneType>
    Branch<3>
  }

  bb 3 {
    v3 = Phi<0, 1, 2> v0 v1 v2
    Return v3
  }
}
)";

  std::unique_ptr<Function> func = HIRParser{}.ParseHIR(hir);
  ASSERT_NE(func, nullptr);

  removeUnreachableBlocks(*func);

  const char* expected = R"(fun foo {
  bb 0 {
    v0 = LoadConst<NoneType>
    CondBranch<1, 3> v0
  }

  bb 1 (preds 0) {
    v1 = LoadConst<NoneType>
    Branch<3>
  }

  bb 3 (preds 0, 1) {
    v3 = Phi<0, 1> v0 v1
    Return v3
  }
}
)";
  EXPECT_EQ(HIRPrinter{}.ToString(*func), expected);
}

template <class T>
Ref<> toByteString(T&& data) {
  auto sp = std::span{data};
  return Ref<>::steal(PyBytes_FromStringAndSize(
      reinterpret_cast<const char*>(sp.data()), sp.size_bytes()));
}

class HIRBuildTest : public RuntimeTest {
 public:
  template <class T>
  std::unique_ptr<Function> build_test(
      T&& bc,
      const std::vector<PyObject*>& locals /* borrowed */) {
    Ref<> bytecode = toByteString(std::span{std::forward<T>(bc)});
    assert(bytecode.get());
    const int nlocals = locals.size();

    auto filename = Ref<>::steal(PyUnicode_FromString("filename"));
    auto funcname = Ref<>::steal(PyUnicode_FromString("funcname"));
    auto consts = Ref<>::steal(PyTuple_New(nlocals));
    auto varnames = Ref<>::steal(PyTuple_New(nlocals));
    for (int i = 0; i < nlocals; i++) {
      PyObject* local = locals.at(i);
      Py_INCREF(local);
      PyTuple_SET_ITEM(consts.get(), i, local);
      PyTuple_SET_ITEM(
          varnames.get(),
          i,
          PyUnicode_FromString(fmt::format("param{}", i).c_str()));
    }

    auto empty_tuple = Ref<>::steal(PyTuple_New(0));
    auto empty_bytes = Ref<>::steal(PyBytes_FromString(""));
    auto code = Ref<PyCodeObject>::steal(PyUnstable_Code_New(
        /*argcount=*/1,
        /*kwonlyargcount*/ 0,
        /*nlocals=*/nlocals,
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
        /*linetable=*/empty_bytes,
        /*_unused_exceptiontable=*/empty_bytes));
    assert(code != nullptr);

    auto func =
        Ref<PyFunctionObject>::steal(PyFunction_New(code, MakeGlobals()));
    assert(func != nullptr);

    return buildHIR(func);
  }

  std::unique_ptr<Function> build_specialized_source(
      const char* src,
      std::initializer_list<int> specialized_opcodes,
      int backedge_opcode = 0) {
    Ref<PyFunctionObject> func(compileAndGet(src, "test"));
    PyCodeObject* code = reinterpret_cast<PyCodeObject*>(func->func_code);

    for (int specialized_opcode : specialized_opcodes) {
      replaceFirstOpcode(
          code, unspecialize(specialized_opcode), specialized_opcode);
    }
    if (backedge_opcode != 0) {
      replaceFirstOpcode(code, JUMP_BACKWARD, backedge_opcode);
    }

    return buildHIR(func);
  }

  std::unique_ptr<Function> build_specialized_source(
      const char* src,
      int specialized_opcode,
      int backedge_opcode = 0) {
    return build_specialized_source(src, {specialized_opcode}, backedge_opcode);
  }

 private:
  void replaceFirstOpcode(PyCodeObject* code, int from_opcode, int to_opcode) {
    for (size_t i = 0, n = countIndices(code); i < n; ++i) {
      _Py_CODEUNIT* instr = codeUnit(code) + i;
      if (_Py_OPCODE(*instr) == from_opcode) {
        *instr = _Py_MAKE_CODEUNIT(to_opcode, _Py_OPARG(*instr));
        return;
      }
    }

    FAIL() << "Did not find opcode " << opcodeName(from_opcode);
  }
};

#if PY_VERSION_HEX < 0x030C0000
// Later-MR capability skips. Split off so HIRBuildTest can join the 3.11
// green family, which forbids GTEST_SKIP. 3.14 keeps the original fixture
// name so the section 3.5 registered-test identity does not change.
class HIRBuildDeferredTest : public HIRBuildTest {};
#define HIR_BUILD_DEFERRED_TEST HIRBuildDeferredTest
#else
#define HIR_BUILD_DEFERRED_TEST HIRBuildTest
#endif

#if PY_VERSION_HEX < 0x030C0000
TEST_F(HIRBuildTest, LoadFastChecksForUnboundLocal311) {
  uint8_t bc[] = {LOAD_FAST, 0, RETURN_VALUE, 0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None});

  EXPECT_EQ(countOpcode(*irfunc, Opcode::kCheckVar), 1);
}

class HIRBuildUnsupportedOpcode311Test
    : public HIRBuildTest,
      public testing::WithParamInterface<int> {};

TEST_P(HIRBuildUnsupportedOpcode311Test, RefusesManifestOpcode) {
  uint8_t bc[] = {static_cast<uint8_t>(GetParam()), 0, RETURN_VALUE, 0};

  try {
    build_test(bc, {Py_None});
    FAIL() << "Expected unsupported opcode to be refused";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(
        std::string{error.what()}.find("unsupported opcode"), std::string::npos)
        << error.what();
  }
}

INSTANTIATE_TEST_SUITE_P(
    PatternMatchingAndExceptStar,
    HIRBuildUnsupportedOpcode311Test,
    testing::Values(
        MATCH_CLASS,
        MATCH_KEYS,
        MATCH_MAPPING,
        MATCH_SEQUENCE,
        CHECK_EG_MATCH));
#endif

#if PY_VERSION_HEX >= 0x030E0000
TEST_F(HIRBuildTest, ToBoolBoolSpecializedUsesBoolGuard) {
  const char* src = R"(
def test(x):
    if x:
        return 1
    return 0
)";
  std::unique_ptr<Function> irfunc(
      build_specialized_source(src, TO_BOOL_BOOL));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<Bool>"), 1) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kPrimitiveBoxBool), 0) << hir;
}

TEST_F(HIRBuildTest, ToBoolIntSpecializedUsesLongGuard) {
  const char* src = R"(
def test(x):
    if x:
        return 1
    return 0
)";
  std::unique_ptr<Function> irfunc(build_specialized_source(src, TO_BOOL_INT));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<LongExact>"), 1) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kIsTruthy), 1) << hir;
}

TEST_F(HIRBuildTest, ToBoolListSpecializedUsesListGuard) {
  const char* src = R"(
def test(x):
    if x:
        return 1
    return 0
)";
  std::unique_ptr<Function> irfunc(build_specialized_source(src, TO_BOOL_LIST));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<ListExact>"), 1) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kIsTruthy), 1) << hir;
}

TEST_F(HIRBuildTest, ToBoolStrSpecializedUsesUnicodeGuard) {
  const char* src = R"(
def test(x):
    if x:
        return 1
    return 0
)";
  std::unique_ptr<Function> irfunc(build_specialized_source(src, TO_BOOL_STR));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<UnicodeExact>"), 1) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kIsTruthy), 1) << hir;
}

TEST_F(HIRBuildTest, ToBoolUnretainedSpecializationsStayGeneric) {
  const char* src = R"(
def test(x):
    if x:
        return 1
    return 0
)";
  std::unique_ptr<Function> none_irfunc(
      build_specialized_source(src, TO_BOOL_NONE));
  ASSERT_NE(none_irfunc, nullptr);
  EXPECT_EQ(countOpcode(*none_irfunc, Opcode::kGuardType), 0)
      << fullPrinter().ToString(*none_irfunc);

  std::unique_ptr<Function> always_true_irfunc(
      build_specialized_source(src, TO_BOOL_ALWAYS_TRUE));
  ASSERT_NE(always_true_irfunc, nullptr);
  EXPECT_EQ(countOpcode(*always_true_irfunc, Opcode::kGuardType), 0)
      << fullPrinter().ToString(*always_true_irfunc);
}
#endif

TEST_F(HIRBuildTest, ExactIntGlobalLoadUsesGuardType) {
  const char* src = R"(
G = 257

def test():
    return G
)";
  std::unique_ptr<Function> irfunc;
  ASSERT_NO_FATAL_FAILURE(CompileToHIR(src, "test", irfunc));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  expectColdGlobalGuardShape(*irfunc, hir, 1, 0);
}

TEST_F(HIRBuildTest, ImmortalExactIntGlobalLoadUsesGuardType) {
  const char* src = R"(
G = 24

def test():
    return G
)";
  std::unique_ptr<Function> irfunc;
  ASSERT_NO_FATAL_FAILURE(CompileToHIR(src, "test", irfunc));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  expectColdGlobalGuardShape(*irfunc, hir, 1, 0);
}

TEST_F(HIRBuildTest, NonExactIntGlobalLoadKeepsGuardIs) {
  const char* src = R"(
G = True

def test():
    return G
)";
  std::unique_ptr<Function> irfunc;
  ASSERT_NO_FATAL_FAILURE(CompileToHIR(src, "test", irfunc));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  expectColdGlobalGuardShape(*irfunc, hir, 0, 1);
}

TEST_F(HIRBuildTest, IntSubclassGlobalLoadKeepsGuardIs) {
  const char* src = R"(
class MyInt(int):
    pass

G = MyInt(257)

def test():
    return G
)";
  std::unique_ptr<Function> irfunc;
  ASSERT_NO_FATAL_FAILURE(CompileToHIR(src, "test", irfunc));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  expectColdGlobalGuardShape(*irfunc, hir, 0, 1);
}

TEST_F(HIRBuildTest, InferredSelfGuardForPlainMethod) {
  const char* src = R"(
class Vec:
    def dot(self, other):
        return self.x * other.x
)";
  Ref<> klass(compileAndGet(src, "Vec"));
  ASSERT_NE(klass, nullptr);
  Ref<PyFunctionObject> method(
      Ref<PyFunctionObject>::steal(PyObject_GetAttrString(klass, "dot")));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(PyFunction_Check(method));

  std::unique_ptr<Function> irfunc(buildHIR(method));
  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<ObjectUser[Vec:Exact]>"), 1)
      << hir;
}

TEST_F(HIRBuildTest, InferredSelfGuardFrameStateAfterResume) {
  const char* src = R"(
class Vec:
    def magnitude(self):
        return self.x
)";
  Ref<> klass(compileAndGet(src, "Vec"));
  ASSERT_NE(klass, nullptr);
  Ref<PyFunctionObject> method(
      Ref<PyFunctionObject>::steal(
          PyObject_GetAttrString(klass, "magnitude")));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(PyFunction_Check(method));

  std::unique_ptr<Function> irfunc(buildHIR(method));
  ASSERT_NO_FATAL_FAILURE(
      assertEntrySelfGuardAfterSetup(method, *irfunc, {RESUME}));
}

TEST_F(HIRBuildTest, InferredSelfGuardFrameStateAfterClosureSetup) {
  const char* src = R"(
class ClosureBox:
    def value(self):
        scale = 7
        def inner():
            return scale + len(__class__.__name__)
        return self.x + inner()
)";
  Ref<> klass(compileAndGet(src, "ClosureBox"));
  ASSERT_NE(klass, nullptr);
  Ref<PyFunctionObject> method(
      Ref<PyFunctionObject>::steal(PyObject_GetAttrString(klass, "value")));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(PyFunction_Check(method));

  std::unique_ptr<Function> irfunc(buildHIR(method));
  ASSERT_NO_FATAL_FAILURE(assertEntrySelfGuardAfterSetup(
      method, *irfunc, {COPY_FREE_VARS, MAKE_CELL, RESUME}));
}

TEST_F(
    HIR_BUILD_DEFERRED_TEST,
    InferredSelfGuardFrameStateAfterGeneratorSetup) {
#if PY_VERSION_HEX < 0x030C0000
  GTEST_SKIP() << "CPython 3.11 generator HIR remains disabled until MR-10";
#endif
  const char* src = R"(
class YieldBox:
    def values(self):
        yield self.x
        return self.x + 1
)";
  Ref<> klass(compileAndGet(src, "YieldBox"));
  ASSERT_NE(klass, nullptr);
  Ref<PyFunctionObject> method(
      Ref<PyFunctionObject>::steal(PyObject_GetAttrString(klass, "values")));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(PyFunction_Check(method));

  std::unique_ptr<Function> irfunc(buildHIR(method));
  ASSERT_NO_FATAL_FAILURE(assertEntrySelfGuardAfterSetup(
      method, *irfunc, {RETURN_GENERATOR, POP_TOP, RESUME}));
}

TEST_F(
    HIR_BUILD_DEFERRED_TEST,
    InferredSelfGuardFrameStateAfterInitialYieldPop) {
#if PY_VERSION_HEX < 0x030C0000
  GTEST_SKIP() << "CPython 3.11 generator HIR remains disabled until MR-10";
#endif
  const char* src = R"(
class YieldBox:
    def values(self):
        yield self.x
        return self.x + 1
)";
  Ref<> klass(compileAndGet(src, "YieldBox"));
  ASSERT_NE(klass, nullptr);
  Ref<PyFunctionObject> method(
      Ref<PyFunctionObject>::steal(PyObject_GetAttrString(klass, "values")));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(PyFunction_Check(method));
  ASSERT_NO_FATAL_FAILURE(
      moveEntryResumeBeforeInitialYieldPop(method->func_code));

  std::unique_ptr<Function> irfunc(buildHIR(method));
  ASSERT_NO_FATAL_FAILURE(assertEntrySelfGuardAfterSetup(
      method, *irfunc, {RETURN_GENERATOR, RESUME, POP_TOP}));
}

TEST_F(
    HIR_BUILD_DEFERRED_TEST,
    InferredSelfGuardMissAfterClosureSetupMatchesInterpreter) {
#if PY_VERSION_HEX < 0x030C0000
  GTEST_SKIP()
      << "CPython 3.11 execution and deopt are outside shadow-only MR-03";
#endif
  const char* src = R"(
class ClosureBox:
    def value(self):
        scale = 7
        def inner():
            return scale + len(__class__.__name__)
        return self.x + inner()
)";
  Ref<> klass(compileAndGet(src, "ClosureBox"));
  ASSERT_NE(klass, nullptr);
  Ref<PyFunctionObject> method(
      Ref<PyFunctionObject>::steal(PyObject_GetAttrString(klass, "value")));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(PyFunction_Check(method));
  ASSERT_EQ(jit::compileFunction(method), jit::Result::OK);

  runCode(R"(
import types

class ClosureSubBox(ClosureBox):
    pass

closure_sub = ClosureSubBox()
closure_sub.x = 11
closure_baseline = types.FunctionType(
    ClosureBox.value.__code__,
    ClosureBox.value.__globals__,
    "closure_baseline",
    ClosureBox.value.__defaults__,
    ClosureBox.value.__closure__,
)

def capture_error(func, obj):
    try:
        func(obj)
    except Exception as exc:
        return (type(exc).__name__, str(exc))
    return ("return", None)

closure_guard_result = ClosureBox.value(closure_sub)
closure_baseline_result = closure_baseline(closure_sub)
closure_missing = ClosureSubBox()
closure_guard_error = capture_error(ClosureBox.value, closure_missing)
closure_baseline_error = capture_error(closure_baseline, closure_missing)
)");

  Ref<> guard_result(getGlobal("closure_guard_result"));
  Ref<> baseline_result(getGlobal("closure_baseline_result"));
  ASSERT_EQ(PyObject_RichCompareBool(guard_result, baseline_result, Py_EQ), 1);
  Ref<> guard_error(getGlobal("closure_guard_error"));
  Ref<> baseline_error(getGlobal("closure_baseline_error"));
  ASSERT_EQ(PyObject_RichCompareBool(guard_error, baseline_error, Py_EQ), 1);
}

TEST_F(
    HIR_BUILD_DEFERRED_TEST,
    InferredSelfGuardMissAfterGeneratorSetupMatchesInterpreter) {
#if PY_VERSION_HEX < 0x030C0000
  GTEST_SKIP()
      << "CPython 3.11 generator execution remains disabled until MR-10";
#endif
  const char* src = R"(
class YieldBox:
    def values(self):
        yield self.x
        return self.x + 1
)";
  Ref<> klass(compileAndGet(src, "YieldBox"));
  ASSERT_NE(klass, nullptr);
  Ref<PyFunctionObject> method(
      Ref<PyFunctionObject>::steal(PyObject_GetAttrString(klass, "values")));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(PyFunction_Check(method));
  ASSERT_EQ(jit::compileFunction(method), jit::Result::OK);

  runCode(R"(
import types

class YieldSubBox(YieldBox):
    pass

yield_sub = YieldSubBox()
yield_sub.x = 41
yield_baseline = types.FunctionType(
    YieldBox.values.__code__,
    YieldBox.values.__globals__,
    "yield_baseline",
    YieldBox.values.__defaults__,
    YieldBox.values.__closure__,
)

def consume(func, obj):
    gen = func(obj)
    first = next(gen)
    try:
        next(gen)
    except StopIteration as exc:
        return (first, exc.value)
    return (first, "not stopped")

def capture_next_error(func, obj):
    try:
        next(func(obj))
    except Exception as exc:
        return (type(exc).__name__, str(exc))
    return ("return", None)

yield_guard_result = consume(YieldBox.values, yield_sub)
yield_baseline_result = consume(yield_baseline, yield_sub)
yield_missing = YieldSubBox()
yield_guard_error = capture_next_error(YieldBox.values, yield_missing)
yield_baseline_error = capture_next_error(yield_baseline, yield_missing)
)");

  Ref<> guard_result(getGlobal("yield_guard_result"));
  Ref<> baseline_result(getGlobal("yield_baseline_result"));
  ASSERT_EQ(PyObject_RichCompareBool(guard_result, baseline_result, Py_EQ), 1);
  Ref<> guard_error(getGlobal("yield_guard_error"));
  Ref<> baseline_error(getGlobal("yield_baseline_error"));
  ASSERT_EQ(PyObject_RichCompareBool(guard_error, baseline_error, Py_EQ), 1);
}

TEST_F(HIRBuildTest, InferredSelfGuardForRaytraceVectorDot) {
  const char* src = R"(
class Vector:
    def dot(self, other):
        return (self.x * other.x) + (self.y * other.y) + (self.z * other.z)
)";
  Ref<> klass(compileAndGet(src, "Vector"));
  ASSERT_NE(klass, nullptr);
  Ref<PyFunctionObject> method(
      Ref<PyFunctionObject>::steal(PyObject_GetAttrString(klass, "dot")));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(PyFunction_Check(method));

  std::unique_ptr<Function> irfunc(buildHIR(method));
  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<ObjectUser[Vector:Exact]>"), 1)
      << hir;
  EXPECT_NE(hir.find("FrameState"), std::string::npos) << hir;
}

TEST_F(HIRBuildTest, InferredSelfGuardForSelfLoadMethod) {
  const char* src = R"(
class Vector:
    def dot(self, other):
        return (self.x * other.x) + (self.y * other.y) + (self.z * other.z)

    def magnitude(self):
        return self.dot(self)
)";
  Ref<> klass(compileAndGet(src, "Vector"));
  ASSERT_NE(klass, nullptr);
  Ref<PyFunctionObject> method(
      Ref<PyFunctionObject>::steal(
          PyObject_GetAttrString(klass, "magnitude")));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(PyFunction_Check(method));

  std::unique_ptr<Function> irfunc(buildHIR(method));
  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<ObjectUser[Vector:Exact]>"), 1)
      << hir;
}

TEST_F(HIRBuildTest, InferredSelfGuardSkipsMethodWithoutSelfAttrLoad) {
  const char* src = R"(
class Vec:
    def identity(self):
        return self
)";
  Ref<> klass(compileAndGet(src, "Vec"));
  ASSERT_NE(klass, nullptr);
  Ref<PyFunctionObject> method(
      Ref<PyFunctionObject>::steal(PyObject_GetAttrString(klass, "identity")));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(PyFunction_Check(method));

  std::unique_ptr<Function> irfunc(buildHIR(method));
  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<"), 0) << hir;
}

TEST_F(HIRBuildTest, InferredSelfGuardSkipsArgNotNamedSelf) {
  const char* src = R"(
class Vec:
    def dot(this, other):
        return this.x * other.x
)";
  Ref<> klass(compileAndGet(src, "Vec"));
  ASSERT_NE(klass, nullptr);
  Ref<PyFunctionObject> method(
      Ref<PyFunctionObject>::steal(PyObject_GetAttrString(klass, "dot")));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(PyFunction_Check(method));

  std::unique_ptr<Function> irfunc(buildHIR(method));
  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<"), 0) << hir;
}

TEST_F(HIRBuildTest, InferredSelfGuardSkipsExistingSubclass) {
  const char* src = R"(
class Vec:
    def dot(self, other):
        return self.x * other.x

class SubVec(Vec):
    pass
)";
  Ref<> klass(compileAndGet(src, "Vec"));
  ASSERT_NE(klass, nullptr);
  Ref<PyFunctionObject> method(
      Ref<PyFunctionObject>::steal(PyObject_GetAttrString(klass, "dot")));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(PyFunction_Check(method));

  std::unique_ptr<Function> irfunc(buildHIR(method));
  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<"), 0) << hir;
}

TEST_F(HIRBuildTest, InferredSelfGuardSkipsNestedQualname) {
  const char* src = R"(
class Outer:
    class Vec:
        def dot(self, other):
            return self.x * other.x
)";
  Ref<> outer(compileAndGet(src, "Outer"));
  ASSERT_NE(outer, nullptr);
  Ref<> klass(Ref<>::steal(PyObject_GetAttrString(outer, "Vec")));
  ASSERT_NE(klass, nullptr);
  Ref<PyFunctionObject> method(
      Ref<PyFunctionObject>::steal(PyObject_GetAttrString(klass, "dot")));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(PyFunction_Check(method));

  std::unique_ptr<Function> irfunc(buildHIR(method));
  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<"), 0) << hir;
}

TEST_F(HIRBuildTest, InferredSelfGuardSkipsCustomGetattribute) {
  const char* src = R"(
class Vec:
    def __getattribute__(self, name):
        return object.__getattribute__(self, name)

    def dot(self, other):
        return self.x * other.x
)";
  Ref<> klass(compileAndGet(src, "Vec"));
  ASSERT_NE(klass, nullptr);
  Ref<PyFunctionObject> method(
      Ref<PyFunctionObject>::steal(PyObject_GetAttrString(klass, "dot")));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(PyFunction_Check(method));

  std::unique_ptr<Function> irfunc(buildHIR(method));
  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<"), 0) << hir;
}

TEST_F(HIRBuildTest, InferredSelfGuardSkipsStaticmethodDescriptor) {
  const char* src = R"(
class Vec:
    @staticmethod
    def dot(self, other):
        return self.x * other.x
)";
  Ref<> klass(compileAndGet(src, "Vec"));
  ASSERT_NE(klass, nullptr);
  Ref<PyFunctionObject> method(
      Ref<PyFunctionObject>::steal(PyObject_GetAttrString(klass, "dot")));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(PyFunction_Check(method));

  std::unique_ptr<Function> irfunc(buildHIR(method));
  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<"), 0) << hir;
}

TEST_F(HIRBuildTest, FloatGlobalLoadKeepsGuardIs) {
  const char* src = R"(
G = 1.5

def test():
    return G
)";
  std::unique_ptr<Function> irfunc;
  ASSERT_NO_FATAL_FAILURE(CompileToHIR(src, "test", irfunc));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  expectColdGlobalGuardShape(*irfunc, hir, 0, 1);
}

TEST_F(HIRBuildTest, UnicodeGlobalLoadKeepsGuardIs) {
  const char* src = R"(
G = "foo"

def test():
    return G
)";
  std::unique_ptr<Function> irfunc;
  ASSERT_NO_FATAL_FAILURE(CompileToHIR(src, "test", irfunc));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  expectColdGlobalGuardShape(*irfunc, hir, 0, 1);
}

TEST_F(HIRBuildTest, BuiltinFunctionGlobalLoadKeepsGuardIs) {
  const char* src = R"(
def test():
    return len
)";
  std::unique_ptr<Function> irfunc;
  ASSERT_NO_FATAL_FAILURE(CompileToHIR(src, "test", irfunc));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  expectColdGlobalGuardShape(*irfunc, hir, 0, 1);
}

#if PY_VERSION_HEX >= 0x030E0000 && PY_VERSION_HEX < 0x030F0000
TEST_F(HIRBuildTest, ListPrefixReverseAssignEmitsRuntimeFastPath) {
  const char* src = R"(
def test(perm, k):
    perm[: k + 1] = perm[k::-1]
    return perm
)";
  std::unique_ptr<Function> irfunc;
  ASSERT_NO_FATAL_FAILURE(CompileToHIR(src, "test", irfunc));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(
      countStaticCallsTo(
          *irfunc, reinterpret_cast<void*>(JITRT_ListPrefixReverseAssign)),
      1)
      << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kBuildSlice), 0) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kStoreSubscr), 0) << hir;
}

TEST_F(HIRBuildTest, ListPrefixReverseAssignRejectsNonPrefixStore) {
  const char* src = R"(
def test(perm, k):
    perm[1 : k + 1] = perm[k::-1]
    return perm
)";
  std::unique_ptr<Function> irfunc;
  ASSERT_NO_FATAL_FAILURE(CompileToHIR(src, "test", irfunc));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_GE(countOpcode(*irfunc, Opcode::kBuildSlice), 1) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kStoreSubscr), 1) << hir;
}

TEST_F(HIRBuildTest, ListPrefixReverseAssignCanBeDisabled) {
  struct RestoreFlag {
    bool old_value;
    ~RestoreFlag() {
      getMutableConfig().hir_opts.list_prefix_reverse_assign = old_value;
    }
  } restore{getConfig().hir_opts.list_prefix_reverse_assign};

  getMutableConfig().hir_opts.list_prefix_reverse_assign = false;

  const char* src = R"(
def test(perm, k):
    perm[: k + 1] = perm[k::-1]
    return perm
)";
  std::unique_ptr<Function> irfunc;
  ASSERT_NO_FATAL_FAILURE(CompileToHIR(src, "test", irfunc));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_GE(countOpcode(*irfunc, Opcode::kBuildSlice), 1) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kStoreSubscr), 1) << hir;
}
#endif

#if PY_VERSION_HEX < 0x030C0000
TEST_F(HIRBuildTest, NoArgSuperMethodCall311LowersToLoadMethodSuper) {
  const char* src = R"(
class A:
    def f(self, x):
        return x

class B(A):
    def f(self, x):
        return super().f(x)

test = B.f
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  int load_method_super_count = 0;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsLoadMethodSuper()) {
        load_method_super_count++;
      }
    }
  }

  EXPECT_EQ(load_method_super_count, 1);
}

TEST_F(HIRBuildTest, NoArgSuperCellReceiver311LoadsReceiverCell) {
  const char* src = R"(
class A:
    @classmethod
    def f(cls):
        return 1

class B(A):
    @classmethod
    def f(cls):
        def capture():
            return cls
        return super().f()

test = B.__dict__["f"].__func__
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  int load_cell_item_count = 0;
  int load_method_super_count = 0;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsLoadCellItem()) {
        load_cell_item_count++;
      } else if (instr.IsLoadMethodSuper()) {
        load_method_super_count++;
      }
    }
  }

  EXPECT_EQ(load_cell_item_count, 2);
  EXPECT_EQ(load_method_super_count, 1);
}

TEST_F(HIRBuildTest, CallFunction311LowersToCallMethod) {
  const char* src = R"(
def test(x):
    return abs(x)
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kCallMethod), 1) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kVectorCall), 0) << hir;

  const CallMethod* call = nullptr;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsCallMethod()) {
        call = static_cast<const CallMethod*>(&instr);
      }
    }
  }
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(call->NumArgs(), 1u);
  ASSERT_NE(call->func(), nullptr);
  ASSERT_NE(call->func()->instr(), nullptr);
  // buildHIR() is not SSA: Register::type() stays TTop. Inspect the def.
  ASSERT_TRUE(call->func()->instr()->IsLoadConst()) << hir;
  EXPECT_TRUE(
      static_cast<const LoadConst*>(call->func()->instr())->type() <= TNullptr)
      << hir;
  ASSERT_NE(call->self(), nullptr);
  ASSERT_NE(call->self()->instr(), nullptr);
  EXPECT_TRUE(call->self()->instr()->IsLoadGlobal()) << hir;
  EXPECT_NE(call->frameState(), nullptr);
}

TEST_F(HIRBuildTest, LoadMethodCall311PreservesReceiverPair) {
  const char* src = R"(
class Box:
    def add(self, value):
        return value

def test(obj):
    return obj.add(1)
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_GE(countOpcode(*irfunc, Opcode::kLoadMethod), 1) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kCallMethod), 1) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kVectorCall), 0) << hir;

  const CallMethod* call = nullptr;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsCallMethod()) {
        call = static_cast<const CallMethod*>(&instr);
      }
    }
  }
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(call->NumArgs(), 1u);
  ASSERT_NE(call->func(), nullptr);
  ASSERT_NE(call->func()->instr(), nullptr);
  EXPECT_TRUE(call->func()->instr()->IsLoadMethod()) << hir;
  ASSERT_NE(call->self(), nullptr);
  ASSERT_NE(call->self()->instr(), nullptr);
  EXPECT_TRUE(call->self()->instr()->IsGetSecondOutput()) << hir;
  EXPECT_NE(call->frameState(), nullptr);
}

TEST_F(HIRBuildTest, KwNamesCall311SetsKwArgsFlag) {
  const char* src = R"(
def test(fn, a):
    return fn(a, k=1)
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kCallMethod), 1) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kVectorCall), 0) << hir;

  const CallMethod* call = nullptr;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsCallMethod()) {
        call = static_cast<const CallMethod*>(&instr);
      }
    }
  }
  ASSERT_NE(call, nullptr);
  EXPECT_TRUE(call->flags() & CallFlags::KwArgs) << hir;
  EXPECT_NE(call->frameState(), nullptr);
}

TEST_F(HIRBuildTest, CallFunctionEx311LowersToCallEx) {
  const char* src = R"(
def test(fn, args):
    return fn(*args)
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kCallEx), 1) << hir;

  const CallEx* call = nullptr;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsCallEx()) {
        call = static_cast<const CallEx*>(&instr);
      }
    }
  }
  ASSERT_NE(call, nullptr);
  EXPECT_FALSE(call->flags() & CallFlags::KwArgs) << hir;
  EXPECT_NE(call->frameState(), nullptr);
}

TEST_F(HIRBuildTest, CallFunctionExKw311SetsKwArgsFlag) {
  const char* src = R"(
def test(fn, args, kw):
    return fn(*args, **kw)
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kCallEx), 1) << hir;

  const CallEx* call = nullptr;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsCallEx()) {
        call = static_cast<const CallEx*>(&instr);
      }
    }
  }
  ASSERT_NE(call, nullptr);
  EXPECT_TRUE(call->flags() & CallFlags::KwArgs) << hir;
  EXPECT_NE(call->frameState(), nullptr);
}

TEST_F(HIRBuildTest, InPlaceBinaryOpSpecialization311DoesNotGuardLongs) {
  _Py_CODEUNIT bc[] = {
      _Py_MAKE_CODEUNIT(LOAD_FAST, 0),
      _Py_MAKE_CODEUNIT(LOAD_FAST, 1),
      _Py_MAKE_CODEUNIT(BINARY_OP_ADD_INT, NB_INPLACE_ADD),
      _Py_MAKE_CODEUNIT(CACHE, 0),
      _Py_MAKE_CODEUNIT(RETURN_VALUE, 0),
  };
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None, Py_None});
  ASSERT_NE(irfunc, nullptr);

  int guard_type_count = 0;
  int inplace_op_count = 0;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsGuardType()) {
        guard_type_count++;
      } else if (instr.IsInPlaceOp()) {
        inplace_op_count++;
      }
    }
  }

  EXPECT_EQ(guard_type_count, 0);
  EXPECT_EQ(inplace_op_count, 1);
}

TEST_F(HIRBuildTest, LoadGlobalModule311UsesIndexedValueGuard) {
  const char* src = R"(
value = object()

def test():
    return value
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  for (int i = 0; i < 200; ++i) {
    Ref<> result = Ref<>::steal(
        PyObject_CallNoArgs(reinterpret_cast<PyObject*>(func.get())));
    ASSERT_NE(result, nullptr);
  }

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  int call_static_count = 0;
  int guard_count = 0;
  int load_global_count = 0;
  int load_global_cached_count = 0;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsCallStatic()) {
        call_static_count++;
      } else if (instr.IsGuard()) {
        guard_count++;
      } else if (instr.IsLoadGlobal()) {
        load_global_count++;
      } else if (instr.IsLoadGlobalCached()) {
        load_global_cached_count++;
      }
    }
  }

  EXPECT_EQ(call_static_count, 1);
  EXPECT_EQ(guard_count, 1);
  EXPECT_EQ(load_global_count, 0);
  EXPECT_EQ(load_global_cached_count, 0);
}

TEST_F(HIRBuildTest, ColdLoadGlobalModule311UsesTheSharedVersionAllocator) {
  const char* src = R"(
value = object()

def test():
    return value
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  int call_static_count = 0;
  int guard_count = 0;
  int load_global_count = 0;
  int load_global_cached_count = 0;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsCallStatic()) {
        call_static_count++;
      } else if (instr.IsGuard()) {
        guard_count++;
      } else if (instr.IsLoadGlobal()) {
        load_global_count++;
      } else if (instr.IsLoadGlobalCached()) {
        load_global_cached_count++;
      }
    }
  }

  // A cold 3.11 dict starts with an unassigned keys version, but since
  // MR-09 the JIT shares the vendored specializer's allocator
  // (Ci_GetDictKeysVersion_311, issuing from the top half of the 32-bit
  // range), so even a cold module global load can version the keys and
  // take the guarded fast path.
  EXPECT_EQ(call_static_count, 1);
  EXPECT_EQ(guard_count, 1);
  EXPECT_EQ(load_global_count, 0);
  EXPECT_EQ(load_global_cached_count, 0);
}

static void expectCompactLongInPlaceFastPath(Function& irfunc) {
  reflowTypes(irfunc);
  Simplify{}.Run(irfunc);

  int compact_check_count = 0;
  int compact_unbox_count = 0;
  int int_binary_count = 0;
  int primitive_box_count = 0;
  int inplace_count = 0;
  int long_inplace_count = 0;
  for (auto& block : irfunc.cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsIsCompactLong()) {
        compact_check_count++;
      } else if (instr.IsCompactLongUnbox()) {
        compact_unbox_count++;
      } else if (instr.IsIntBinaryOp()) {
        int_binary_count++;
      } else if (instr.IsPrimitiveBox()) {
        primitive_box_count++;
      } else if (instr.IsInPlaceOp()) {
        inplace_count++;
      } else if (instr.IsLongInPlaceOp()) {
        long_inplace_count++;
      }
    }
  }

  EXPECT_EQ(compact_check_count, 2);
  EXPECT_EQ(compact_unbox_count, 2);
  EXPECT_EQ(int_binary_count, 2);
  EXPECT_EQ(primitive_box_count, 1);
  EXPECT_EQ(inplace_count, 0);
  EXPECT_EQ(long_inplace_count, 0);
}

TEST_F(HIRBuildTest, ExactLongInPlaceAddUsesCompactPrimitiveFastPath) {
  const char* hir = R"(fun test {
  bb 0 {
    v0 = LoadArg<0>
    v1 = LoadArg<1>
    v2 = GuardType<LongExact> v0
    v3 = GuardType<LongExact> v1
    v4 = InPlaceOp<Add> v2 v3 {
      FrameState {
        CurInstrOffset -2
      }
    }
    Return v4
  }
}
)";
  std::unique_ptr<Function> irfunc = HIRParser{}.ParseHIR(hir);
  ASSERT_NE(irfunc, nullptr);

  expectCompactLongInPlaceFastPath(*irfunc);
}

TEST_F(HIRBuildTest, ExactLongInPlaceSubtractUsesCompactPrimitiveFastPath) {
  const char* hir = R"(fun test {
  bb 0 {
    v0 = LoadArg<0>
    v1 = LoadArg<1>
    v2 = GuardType<LongExact> v0
    v3 = GuardType<LongExact> v1
    v4 = InPlaceOp<Subtract> v2 v3 {
      FrameState {
        CurInstrOffset -2
      }
    }
    Return v4
  }
}
)";
  std::unique_ptr<Function> irfunc = HIRParser{}.ParseHIR(hir);
  ASSERT_NE(irfunc, nullptr);

  expectCompactLongInPlaceFastPath(*irfunc);
}

TEST_F(HIRBuildTest, TryLoopReturningHandler311BuildsHIR) {
  const char* src = R"(
class Done(Exception):
    pass

def test(limit):
    try:
        i = 0
        while True:
            i += 1
            if i >= limit:
                raise Done(i)
    except Done as exc:
        return exc.args[0]
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  EXPECT_EQ(unsupportedShapeReason311(func->func_code), nullptr);
  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);
  // CPython 3.11 exception handlers are interpreter recovery surfaces: the
  // Raise deopts using the exception table rather than adding a HIR edge to
  // the handler.  MR-03 validates that this no-normal-return shape reaches
  // LIR/codegen safely; executing the handler belongs to the deopt MR.
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kReturn), 0);
  EXPECT_GT(countOpcode(*irfunc, Opcode::kRaise), 0);
}

TEST_F(HIRBuildTest, RaiseOnlyFunction311BuildsHIR) {
  const char* src = R"(
def test(value):
    raise RuntimeError(value)
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  EXPECT_EQ(unsupportedShapeReason311(func->func_code), nullptr);
  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kRaise), 1);
}

TEST_F(HIRBuildTest, AsyncGenExpressionFactory311RefusesAsyncOpcodes) {
  // A synchronous factory that *constructs* an async genexpression still
  // contains GET_AITER / ASYNC_GEN_WRAP in its own bytecode. Eligibility
  // must return the registered async reason, not SUPPORTED_OPCODE_FAILURE.
  const char* src = R"(
async def arange(n):
    yield n

def make_arange(n):
    return (i * 2 async for i in arange(n))
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "make_arange"));
  ASSERT_NE(func, nullptr);

  EXPECT_EQ(unsupportedShapeReason311(func->func_code), nullptr);
  EXPECT_STREQ(
      unsupportedOpcodeReason311(func->func_code), "INTERP_ONLY_ASYNC_CODE");
  EXPECT_THROW(buildHIR(func), std::runtime_error);
}

TEST_F(HIRBuildTest, OversizedBytecode311RefusesCodegenSpan) {
  // Mirrors test_compile.TestSpecifics.test_extended_arg at a size just
  // over the 64KiB shadow bytecode budget.
  std::string longexpr = "x = x or ";
  for (int i = 0; i < 2500; i++) {
    longexpr += "-x";
  }
  std::string src = "def test(x):\n";
  for (int i = 0; i < 5; i++) {
    src += "    " + longexpr + "\n";
  }
  src += "    return x\n";
  Ref<PyFunctionObject> func(compileAndGet(src.c_str(), "test"));
  ASSERT_NE(func, nullptr);
  ASSERT_GT(_PyCode_NBYTES(func->func_code), 65536);

  EXPECT_STREQ(
      unsupportedShapeReason311(func->func_code), "REFUSE_SHAPE_CODEGEN_SPAN");
  EXPECT_THROW(buildHIR(func), std::runtime_error);
}

TEST_F(HIRBuildTest, IntAccumulator311HasStableShapeReason) {
  const char* src = R"(
def test(value):
    value *= 2
    return value
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  EXPECT_STREQ(
      unsupportedShapeReason311(func->func_code),
      "REFUSE_SHAPE_INT_ACCUMULATOR_POLICY");
  EXPECT_THROW(buildHIR(func), std::runtime_error);
}
#endif

TEST_F(HIRBuildTest, GetLength) {
  //  0 LOAD_FAST  0
  //  2 GET_LENGTH
  //  4 RETURN_VALUE
  uint8_t bc[] = {LOAD_FAST, 0, GET_LEN, 0, RETURN_VALUE, 0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None});

#if PY_VERSION_HEX < 0x030C0000
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kCheckVar), 1);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kGetLength), 1);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kReturn), 1);
#else
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = GetLength v0 {
      FrameState {
        CurInstrOffset 2
        Locals<1> v0
        Stack<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<2> v0 v2
    }
    v3 = Assign v2
    v2 = Assign v0
    Return v3
  }
}
)";
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
#endif
}

#ifndef Py_GIL_DISABLED
TEST_F(HIRBuildTest, TupleSpecializedUnpackKeepsListFastPath) {
  const char* src = R"(
T = (1, 2, 3)

def test(seq):
    a, b, c = seq
    return a + b + c
)";

  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);
  Ref<> tuple(getGlobal("T"));
  ASSERT_NE(tuple.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(func.get()), tuple.get(), nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(isIntEquals(result, 6));
  }

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  EXPECT_EQ(countOpcode(*irfunc, Opcode::kUnpackSequence), 0);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kReserveStack), 0);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kCondBranchCheckType), 2);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadFieldAddress), 1);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadField), 1);
}

TEST_F(HIRBuildTest, ListSpecializedUnpackKeepsTupleFastPath) {
  const char* src = R"(
L = [1, 2, 3]

def test(seq):
    a, b, c = seq
    return a + b + c
)";

  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);
  Ref<> list(getGlobal("L"));
  ASSERT_NE(list.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(func.get()), list.get(), nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(isIntEquals(result, 6));
  }

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  EXPECT_EQ(countOpcode(*irfunc, Opcode::kUnpackSequence), 0);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kReserveStack), 0);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kCondBranchCheckType), 2);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadField), 1);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadFieldAddress), 1);
}

TEST_F(HIRBuildTest, TwoTupleSpecializedUnpackKeepsListFastPath) {
  const char* src = R"(
T = (1, 2)

def test(seq):
    a, b = seq
    return a + b
)";

  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);
  Ref<> tuple(getGlobal("T"));
  ASSERT_NE(tuple.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(func.get()), tuple.get(), nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(isIntEquals(result, 3));
  }

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  EXPECT_EQ(countOpcode(*irfunc, Opcode::kUnpackSequence), 0);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kReserveStack), 0);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kCondBranchCheckType), 2);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadFieldAddress), 1);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadField), 1);
}

TEST_F(HIR_BUILD_DEFERRED_TEST, SlotSpecializedLoadAndStoreUseFieldOps) {
#if PY_VERSION_HEX < 0x030C0000
  GTEST_SKIP() << "CPython 3.11 slot fast paths remain disabled until MR-09";
#endif
  const char* src = R"(
class SlotValue:
    __slots__ = ("value",)

obj = SlotValue()
obj.value = 0

def test(obj):
    obj.value = obj.value + 1
    return obj.value
)";

  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);
  Ref<> obj(getGlobal("obj"));
  ASSERT_NE(obj.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(func.get()), obj.get(), nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(isIntEquals(result, i + 1));
  }
  ASSERT_TRUE(hasSpecializedOpcode(func, LOAD_ATTR_SLOT));
  ASSERT_TRUE(hasSpecializedOpcode(func, STORE_ATTR_SLOT));
  ASSERT_TRUE(PyType_HasFeature(Py_TYPE(obj.get()), Py_TPFLAGS_HEAPTYPE));

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_GE(countOpcode(*irfunc, Opcode::kLoadAttr), 2) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadAttrCached), 0) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kStoreAttr), 0) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kStoreAttrCached), 0) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kLoadField), 3) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kStoreField), 1) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kCheckField), 2) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kPrimitiveCompare), 2) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kCondBranch), 2) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kGuard), 1) << hir;
}

TEST_F(HIR_BUILD_DEFERRED_TEST, SlotSpecializedLoadMethodUsesFieldOps) {
#if PY_VERSION_HEX < 0x030C0000
  GTEST_SKIP() << "CPython 3.11 slot fast paths remain disabled until MR-09";
#endif
  const char* src = R"(
def target():
    return 42

class SlotCallable:
    __slots__ = ("fn",)

obj = SlotCallable()
obj.fn = target

def test(obj):
    return obj.fn()
)";

  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);
  Ref<> obj(getGlobal("obj"));
  ASSERT_NE(obj.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(func.get()), obj.get(), nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(isIntEquals(result, 42));
  }
  ASSERT_TRUE(hasSpecializedOpcode(func, LOAD_ATTR_SLOT));
  ASSERT_TRUE(PyType_HasFeature(Py_TYPE(obj.get()), Py_TPFLAGS_HEAPTYPE));

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadAttr), 1) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadAttrCached), 0) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadMethod), 0) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kLoadField), 1) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kPrimitiveCompare), 1) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kCondBranch), 1) << hir;
}

TEST_F(
    HIR_BUILD_DEFERRED_TEST,
    SlotSpecializedLoadAttrUnsetSlotUsesCheckField) {
#if PY_VERSION_HEX < 0x030C0000
  GTEST_SKIP() << "CPython 3.11 slot fast paths remain disabled until MR-09";
#endif
  const char* src = R"(
class SlotValue:
    __slots__ = ("value",)

obj = SlotValue()
obj.value = 1

def test(obj):
    return obj.value
)";

  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);
  Ref<> obj(getGlobal("obj"));
  ASSERT_NE(obj.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(func.get()), obj.get(), nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(isIntEquals(result, 1));
  }
  ASSERT_TRUE(hasSpecializedOpcode(func, LOAD_ATTR_SLOT));
  ASSERT_TRUE(PyObject_DelAttrString(obj.get(), "value") == 0);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadAttr), 1) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadAttrCached), 0) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kLoadField), 2) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kCheckField), 1) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kPrimitiveCompare), 1) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kCondBranch), 1) << hir;
}

TEST_F(
    HIR_BUILD_DEFERRED_TEST,
    SlotSpecializedLoadAttrUnsetSlotWithGetattrFallsBack) {
#if PY_VERSION_HEX < 0x030C0000
  GTEST_SKIP() << "CPython 3.11 slot fast paths remain disabled until MR-09";
#endif
  const char* src = R"(
class SlotValue:
    __slots__ = ("value",)

    def __getattr__(self, name):
        if name == "value":
            return 42
        raise AttributeError(name)

obj = SlotValue()
obj.value = 1

def test(obj):
    return obj.value
)";

  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);
  Ref<> obj(getGlobal("obj"));
  ASSERT_NE(obj.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(func.get()), obj.get(), nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(isIntEquals(result, 1));
  }
  ASSERT_TRUE(hasSpecializedOpcode(func, LOAD_ATTR_SLOT));
  ASSERT_TRUE(PyObject_DelAttrString(obj.get(), "value") == 0);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_GE(countOpcode(*irfunc, Opcode::kLoadField), 1) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kCondBranch), 1) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kCheckField), 0) << hir;
  EXPECT_GE(
      countOpcode(*irfunc, Opcode::kLoadAttr) +
          countOpcode(*irfunc, Opcode::kLoadAttrCached),
      1)
      << hir;
}

#if PY_VERSION_HEX >= 0x030E0000
TEST_F(HIRBuildTest, SlotSpecializedLoadAttrWithHeaderOffsetFallsBack) {
  const char* src = R"(
class SlotValue:
    __slots__ = ("value",)

obj = SlotValue()
obj.value = 1

def test(obj):
    return obj.__class__
)";

  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);
  Ref<> obj(getGlobal("obj"));
  ASSERT_NE(obj.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(func.get()), obj.get(), nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(PyType_Check(result));
  }
  ASSERT_TRUE(hasSpecializedOpcode(func, LOAD_ATTR_SLOT));

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadField), 0) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kGuard), 0) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadAttr), 1) << hir;
}

TEST_F(HIRBuildTest, SlotSpecializedStoreAttrWithReplacedDescrFallsBack) {
  const char* src = R"(
class SlotValue:
    __slots__ = ("value",)

obj = SlotValue()
obj.value = 1

def test(obj, value):
    obj.value = value
)";

  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);
  Ref<> klass(getGlobal("SlotValue"));
  ASSERT_NE(klass.get(), nullptr);
  Ref<> obj(getGlobal("obj"));
  ASSERT_NE(obj.get(), nullptr);
  Ref<> value(Ref<>::steal(PyLong_FromLong(2)));
  ASSERT_NE(value.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(func.get()),
        obj.get(),
        value.get(),
        nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(result == Py_None);
  }
  ASSERT_TRUE(hasSpecializedOpcode(func, STORE_ATTR_SLOT));
  ASSERT_EQ(PyObject_SetAttrString(klass.get(), "value", Py_None), 0);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kStoreField), 0) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kStoreAttr), 1) << hir;
}

TEST_F(HIRBuildTest, SlotSpecializedStructseqLoadAttrUsesFieldOps) {
  const char* src = R"(
import os

stat_result = os.stat(".")

def test(stat_result):
    return stat_result.st_mode
)";

  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);
  Ref<> stat_result(getGlobal("stat_result"));
  ASSERT_NE(stat_result.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(func.get()), stat_result.get(), nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(PyLong_Check(result));
  }
  ASSERT_TRUE(hasSpecializedOpcode(func, LOAD_ATTR_SLOT));

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadAttr), 1) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kLoadField), 1) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kCondBranch), 1) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kPrimitiveCompare), 1) << hir;

  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(func.get()), stat_result.get(), nullptr));
  ASSERT_NE(result.get(), nullptr);
  ASSERT_TRUE(PyLong_Check(result));
}
#endif

#if PY_VERSION_HEX >= 0x030E0000 && !defined(Py_GIL_DISABLED)
TEST_F(HIRBuildTest, ManagedDictOnlySelfAttrDoesNotUseInlineValuesFastPath) {
  const char* src = R"(
class EncodingBytes(bytes):
    def __new__(cls, value):
        return bytes.__new__(cls, value)

    def __init__(self, value):
        self._position = 0

    def read_position(self):
        return self._position

obj = EncodingBytes(b"abcdef")
)";

  Ref<> klass(compileAndGet(src, "EncodingBytes"));
  ASSERT_NE(klass, nullptr);
  Ref<PyFunctionObject> method(Ref<PyFunctionObject>::steal(
      PyObject_GetAttrString(klass, "read_position")));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(PyFunction_Check(method));

  std::unique_ptr<Function> irfunc(buildHIR(method));
  ASSERT_NE(irfunc, nullptr);
  Compiler::runPasses(*irfunc, PassConfig::kSimplify);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<BytesUser[EncodingBytes:Exact]>"), 1)
      << hir;
  EXPECT_EQ(countSubstring(hir, "inline_values.valid"), 0) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kLoadAttrCached), 1) << hir;
}

TEST_F(HIRBuildTest, InlineValuesSelfAttrStillUsesSplitDictFastPath) {
  const char* src = R"(
class InlineBox:
    def __init__(self):
        self.value = 42

    def read_value(self):
        return self.value

obj = InlineBox()
)";

  Ref<> klass(compileAndGet(src, "InlineBox"));
  ASSERT_NE(klass, nullptr);
  Ref<PyFunctionObject> method(
      Ref<PyFunctionObject>::steal(PyObject_GetAttrString(klass, "read_value")));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(PyFunction_Check(method));

  std::unique_ptr<Function> irfunc(buildHIR(method));
  ASSERT_NE(irfunc, nullptr);
  Compiler::runPasses(*irfunc, PassConfig::kSimplify);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<ObjectUser[InlineBox:Exact]>"), 1)
      << hir;
  EXPECT_GE(countSubstring(hir, "inline_values.valid"), 1) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadAttrCached), 0) << hir;
}
#endif

#if PY_VERSION_HEX >= 0x030E0000
TEST_F(HIRBuildTest, LoadAttrMethodWithValuesLowersToHelperCall) {
  const char* src = R"(
class MethodValue:
    def __init__(self):
        self.base = 40

    def method(self, value):
        return self.base + value

obj = MethodValue()

def test(obj, value):
    return obj.method(value)
)";

  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);
  Ref<> obj(getGlobal("obj"));
  ASSERT_NE(obj.get(), nullptr);
  Ref<> value = Ref<>::steal(PyLong_FromLong(2));
  ASSERT_NE(value.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(func.get()),
        obj.get(),
        value.get(),
        nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(isIntEquals(result, 42));
  }
  ASSERT_TRUE(hasSpecializedOpcode(func, LOAD_ATTR_METHOD_WITH_VALUES));

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(
      countStaticCallsTo(
          *irfunc, reinterpret_cast<void*>(JITRT_LoadAttrMethodWithValues)),
      1)
      << hir;
  const CallStatic* helper_call = findStaticCallTo(
      *irfunc, reinterpret_cast<void*>(JITRT_LoadAttrMethodWithValues));
  ASSERT_NE(helper_call, nullptr) << hir;
  ASSERT_EQ(helper_call->NumArgs(), 5);
  Instr* descr_instr = helper_call->arg(3)->instr();
  ASSERT_NE(descr_instr, nullptr) << hir;
  ASSERT_TRUE(descr_instr->IsLoadConst()) << hir;
  const auto& descr_load = static_cast<const LoadConst&>(*descr_instr);
  EXPECT_EQ(descr_load.type().toString().rfind("CPtr[", 0), 0) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadAttr), 0) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadMethod), 0) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadMethodCached), 0) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kGetSecondOutput), 1) << hir;
}

TEST_F(HIRBuildTest, LoadAttrMethodWithValuesFallbacksPreserveSemantics) {
  const char* src = R"(
class C:
    def __init__(self, base):
        self.base = base

    def method(self, value):
        return self.base + value

class D:
    def __init__(self, base):
        self.base = base

    def method(self, value):
        return self.base + value

c_obj = C(40)
d_obj = D(200)

def test_switch(obj, value):
    return obj.method(value)

def shadow_c():
    c_obj.method = lambda value: value + 100

class GetattrCallable:
    def __getattr__(self, name):
        if name == "method":
            return lambda value: value + 500
        raise AttributeError(name)

getattr_obj = GetattrCallable()

class Replaceable:
    def __init__(self, base):
        self.base = base

    def method(self, value):
        return self.base + value

replace_obj = Replaceable(40)

def test_replace(obj, value):
    return obj.method(value)

def new_method(self, value):
    return self.base + value + 100

def replace_method():
    Replaceable.method = new_method

class PropertyReplaceable:
    def __init__(self, base):
        self.base = base

    def method(self, value):
        return self.base + value

property_obj = PropertyReplaceable(40)

def test_property_replace(obj, value):
    return obj.method(value)

def replace_with_property():
    PropertyReplaceable.method = property(
        lambda self: lambda value: self.base + value + 200
    )

class CallableMethod:
    def __call__(self, value):
        return value + 300

class CallableReplaceable:
    def __init__(self, base):
        self.base = base

    def method(self, value):
        return self.base + value

callable_obj = CallableReplaceable(40)

def test_callable_replace(obj, value):
    return obj.method(value)

def replace_with_callable():
    CallableReplaceable.method = CallableMethod()
)";

  Ref<PyFunctionObject> test_switch(compileAndGet(src, "test_switch"));
  ASSERT_NE(test_switch.get(), nullptr);
  Ref<> c_obj(getGlobal("c_obj"));
  ASSERT_NE(c_obj.get(), nullptr);
  Ref<> d_obj(getGlobal("d_obj"));
  ASSERT_NE(d_obj.get(), nullptr);
  Ref<> value = Ref<>::steal(PyLong_FromLong(2));
  ASSERT_NE(value.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(test_switch.get()),
        c_obj.get(),
        value.get(),
        nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(isIntEquals(result, 42));
  }
  ASSERT_TRUE(hasSpecializedOpcode(test_switch, LOAD_ATTR_METHOD_WITH_VALUES));
  ASSERT_EQ(jit::compileFunction(test_switch), jit::Result::OK);

  auto type_miss_result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(test_switch.get()),
      d_obj.get(),
      value.get(),
      nullptr));
  ASSERT_NE(type_miss_result.get(), nullptr);
  ASSERT_TRUE(isIntEquals(type_miss_result, 202));

  Ref<PyFunctionObject> shadow_c(getGlobal("shadow_c"));
  ASSERT_NE(shadow_c.get(), nullptr);
  auto shadow_result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(shadow_c.get()), nullptr));
  ASSERT_NE(shadow_result.get(), nullptr);

  auto shadowed_call_result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(test_switch.get()),
      c_obj.get(),
      value.get(),
      nullptr));
  ASSERT_NE(shadowed_call_result.get(), nullptr);
  ASSERT_TRUE(isIntEquals(shadowed_call_result, 102));

  Ref<> getattr_obj(getGlobal("getattr_obj"));
  ASSERT_NE(getattr_obj.get(), nullptr);
  auto getattr_result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(test_switch.get()),
      getattr_obj.get(),
      value.get(),
      nullptr));
  ASSERT_NE(getattr_result.get(), nullptr);
  ASSERT_TRUE(isIntEquals(getattr_result, 502));

  Ref<PyFunctionObject> test_replace(compileAndGet(src, "test_replace"));
  ASSERT_NE(test_replace.get(), nullptr);
  Ref<> replace_obj(getGlobal("replace_obj"));
  ASSERT_NE(replace_obj.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(test_replace.get()),
        replace_obj.get(),
        value.get(),
        nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(isIntEquals(result, 42));
  }
  ASSERT_TRUE(hasSpecializedOpcode(test_replace, LOAD_ATTR_METHOD_WITH_VALUES));
  ASSERT_EQ(jit::compileFunction(test_replace), jit::Result::OK);

  Ref<PyFunctionObject> replace_method(getGlobal("replace_method"));
  ASSERT_NE(replace_method.get(), nullptr);
  auto replace_result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(replace_method.get()), nullptr));
  ASSERT_NE(replace_result.get(), nullptr);

  auto replaced_call_result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(test_replace.get()),
      replace_obj.get(),
      value.get(),
      nullptr));
  ASSERT_NE(replaced_call_result.get(), nullptr);
  ASSERT_TRUE(isIntEquals(replaced_call_result, 142));

  Ref<PyFunctionObject> test_property_replace(
      compileAndGet(src, "test_property_replace"));
  ASSERT_NE(test_property_replace.get(), nullptr);
  Ref<> property_obj(getGlobal("property_obj"));
  ASSERT_NE(property_obj.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(test_property_replace.get()),
        property_obj.get(),
        value.get(),
        nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(isIntEquals(result, 42));
  }
  ASSERT_TRUE(
      hasSpecializedOpcode(test_property_replace, LOAD_ATTR_METHOD_WITH_VALUES));
  ASSERT_EQ(jit::compileFunction(test_property_replace), jit::Result::OK);

  Ref<PyFunctionObject> replace_with_property(getGlobal("replace_with_property"));
  ASSERT_NE(replace_with_property.get(), nullptr);
  auto property_replace_result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(replace_with_property.get()), nullptr));
  ASSERT_NE(property_replace_result.get(), nullptr);

  auto property_call_result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(test_property_replace.get()),
      property_obj.get(),
      value.get(),
      nullptr));
  ASSERT_NE(property_call_result.get(), nullptr);
  ASSERT_TRUE(isIntEquals(property_call_result, 242));

  Ref<PyFunctionObject> test_callable_replace(
      compileAndGet(src, "test_callable_replace"));
  ASSERT_NE(test_callable_replace.get(), nullptr);
  Ref<> callable_obj(getGlobal("callable_obj"));
  ASSERT_NE(callable_obj.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(test_callable_replace.get()),
        callable_obj.get(),
        value.get(),
        nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(isIntEquals(result, 42));
  }
  ASSERT_TRUE(
      hasSpecializedOpcode(test_callable_replace, LOAD_ATTR_METHOD_WITH_VALUES));
  ASSERT_EQ(jit::compileFunction(test_callable_replace), jit::Result::OK);

  Ref<PyFunctionObject> replace_with_callable(getGlobal("replace_with_callable"));
  ASSERT_NE(replace_with_callable.get(), nullptr);
  auto callable_replace_result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(replace_with_callable.get()), nullptr));
  ASSERT_NE(callable_replace_result.get(), nullptr);

  auto callable_call_result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(test_callable_replace.get()),
      callable_obj.get(),
      value.get(),
      nullptr));
  ASSERT_NE(callable_call_result.get(), nullptr);
  ASSERT_TRUE(isIntEquals(callable_call_result, 302));
}
#endif

TEST_F(HIR_BUILD_DEFERRED_TEST, MemberDescriptorStoreSimplifiesToStoreField) {
#if PY_VERSION_HEX < 0x030C0000
  GTEST_SKIP()
      << "CPython 3.11 descriptor fast paths remain disabled until MR-09";
#endif
  const char* src = R"(
class SlotValue:
    __slots__ = ("value",)

obj = SlotValue()
obj.value = 0

def test(value):
    obj.value = value
    return obj.value
)";

  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);
  Compiler::runPasses(*irfunc, PassConfig::kSimplify);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kStoreAttr), 0) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kStoreAttrCached), 0) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kStoreField), 1) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kLoadField), 1) << hir;
}

TEST_F(HIRBuildTest, ReadonlyMemberDescriptorStoreStaysGeneric) {
  // This test pins the CACHED store form, which requires the attribute
  // caches the 3.11 default keeps off until MR-09; enable them for the
  // scope of this compilation.
  bool old_attr_caches = getConfig().attr_caches;
  getMutableConfig().attr_caches = true;
  SCOPE_EXIT(getMutableConfig().attr_caches = old_attr_caches);
  const char* src = R"(
def test(value):
    test.__globals__ = value
)";

  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);
  Compiler::runPasses(*irfunc, PassConfig::kSimplify);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kStoreField), 0) << hir;
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kStoreAttr), 0) << hir;
  EXPECT_GE(countOpcode(*irfunc, Opcode::kStoreAttrCached), 1) << hir;
}

TEST_F(
    HIR_BUILD_DEFERRED_TEST,
    SlotLoadTypeVersionGuardFallsBackAfterDescriptorChange) {
#if PY_VERSION_HEX < 0x030C0000
  GTEST_SKIP() << "CPython 3.11 slot invalidation remains disabled until MR-09";
#endif
  const char* src = R"(
class SlotValue:
    __slots__ = ("value",)

obj = SlotValue()
obj.value = 1

def test(obj):
    return obj.value

def replace_descriptor():
    SlotValue.value = property(lambda self: 99)
)";

  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);
  Ref<> obj(getGlobal("obj"));
  ASSERT_NE(obj.get(), nullptr);
  Ref<PyFunctionObject> replace_descriptor(getGlobal("replace_descriptor"));
  ASSERT_NE(replace_descriptor.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(func.get()), obj.get(), nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(isIntEquals(result, 1));
  }
  ASSERT_TRUE(hasSpecializedOpcode(func, LOAD_ATTR_SLOT));
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);

  auto replace_result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(replace_descriptor.get()), nullptr));
  ASSERT_NE(replace_result.get(), nullptr);

  auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(func.get()), obj.get(), nullptr));
  ASSERT_NE(result.get(), nullptr);
  ASSERT_TRUE(isIntEquals(result, 99));
}

TEST_F(HIR_BUILD_DEFERRED_TEST, SplitDictLoadFallsBackAfterDescriptorChange) {
#if PY_VERSION_HEX < 0x030C0000
  GTEST_SKIP()
      << "CPython 3.11 attribute invalidation remains disabled until MR-09";
#endif
  const char* src = R"(
class Vector:
    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z

    def dot(self, other):
        return (self.x * other.x) + (self.y * other.y) + (self.z * other.z)

receiver = Vector(1.0, 2.0, 3.0)
other = Vector(4.0, 5.0, 6.0)

def replace_descriptor():
    Vector.x = property(lambda self: 10.0)
)";

  Ref<> klass(compileAndGet(src, "Vector"));
  ASSERT_NE(klass, nullptr);
  Ref<PyFunctionObject> method(
      Ref<PyFunctionObject>::steal(PyObject_GetAttrString(klass, "dot")));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(PyFunction_Check(method));
  Ref<> receiver(getGlobal("receiver"));
  ASSERT_NE(receiver, nullptr);
  Ref<> other(getGlobal("other"));
  ASSERT_NE(other, nullptr);
  Ref<PyFunctionObject> replace_descriptor(getGlobal("replace_descriptor"));
  ASSERT_NE(replace_descriptor, nullptr);

  ASSERT_EQ(jit::compileFunction(method), jit::Result::OK);

  auto replace_result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(replace_descriptor.get()), nullptr));
  ASSERT_NE(replace_result, nullptr);

  auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(method.get()),
      receiver.get(),
      other.get(),
      nullptr));
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyFloat_CheckExact(result));
  ASSERT_DOUBLE_EQ(PyFloat_AsDouble(result), 128.0);
}

TEST_F(
    HIR_BUILD_DEFERRED_TEST,
    SlotStoreTypeVersionGuardFallsBackAfterDescriptorChange) {
#if PY_VERSION_HEX < 0x030C0000
  GTEST_SKIP() << "CPython 3.11 slot invalidation remains disabled until MR-09";
#endif
  const char* src = R"(
events = []

class SlotValue:
    __slots__ = ("value",)

obj = SlotValue()
obj.value = 1

def test(obj, value):
    obj.value = value
    return "done"

def replace_descriptor():
    SlotValue.value = property(lambda self: 99, lambda self, value: events.append(value))
)";

  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);
  Ref<> obj(getGlobal("obj"));
  ASSERT_NE(obj.get(), nullptr);
  Ref<> events(getGlobal("events"));
  ASSERT_NE(events.get(), nullptr);
  ASSERT_TRUE(PyList_Check(events.get()));
  Ref<PyFunctionObject> replace_descriptor(getGlobal("replace_descriptor"));
  ASSERT_NE(replace_descriptor.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto value = Ref<>::steal(PyLong_FromLong(i));
    ASSERT_NE(value.get(), nullptr);
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(func.get()),
        obj.get(),
        value.get(),
        nullptr));
    ASSERT_NE(result.get(), nullptr);
  }
  ASSERT_TRUE(hasSpecializedOpcode(func, STORE_ATTR_SLOT));
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);

  auto replace_result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(replace_descriptor.get()), nullptr));
  ASSERT_NE(replace_result.get(), nullptr);

  auto value = Ref<>::steal(PyLong_FromLong(42));
  ASSERT_NE(value.get(), nullptr);
  auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(func.get()), obj.get(), value.get(), nullptr));
  ASSERT_NE(result.get(), nullptr);
  ASSERT_TRUE(PyUnicode_Check(result.get()));
  EXPECT_EQ(PyList_GET_SIZE(events.get()), 1);
  ASSERT_TRUE(isIntEquals(PyList_GET_ITEM(events.get(), 0), 42));
}
#endif

#if PY_VERSION_HEX >= 0x030C0000
TEST_F(
    HIRBuildTest,
    SingleOpNumericLeafSpecializedIntBinaryOpSkipsLongExactGuards) {
  std::unique_ptr<Function> irfunc = build_specialized_source(
      "def test(a, b):\n"
      "    return a + b\n",
      BINARY_OP_ADD_INT);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<LongExact>"), 0);
}

TEST_F(
    HIRBuildTest,
    MultiOpNumericLeafSpecializedIntBinaryOpKeepsLongExactGuards) {
  std::unique_ptr<Function> irfunc = build_specialized_source(
      "def test(a, b):\n"
      "    return (a + b) * (a - b)\n",
      {BINARY_OP_ADD_INT, BINARY_OP_MULTIPLY_FLOAT, BINARY_OP_SUBTRACT_FLOAT});

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<LongExact>"), 2);
}

TEST_F(
    HIRBuildTest,
    NoBackedgeAttributeSpecializedIntBinaryOpSkipsLongExactGuards) {
  std::unique_ptr<Function> irfunc = build_specialized_source(
      "def test(a, b):\n"
      "    return a.x + b.x\n",
      BINARY_OP_ADD_INT);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<LongExact>"), 0);
}

TEST_F(HIRBuildTest, BackedgeSpecializedIntBinaryOpKeepsLongExactGuards) {
  std::unique_ptr<Function> irfunc = build_specialized_source(
      "def test(a, b):\n"
      "    while True:\n"
      "        a + b\n",
      BINARY_OP_ADD_INT);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<LongExact>"), 2);
}

TEST_F(HIRBuildTest, NoBackedgeSpecializedIntCompareSkipsLongExactGuards) {
  std::unique_ptr<Function> irfunc = build_specialized_source(
      "def test(a, b):\n"
      "    return a < b\n",
      COMPARE_OP_INT);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<LongExact>"), 0);
}

TEST_F(
    HIRBuildTest,
    MultiOpNumericLeafSpecializedIntCompareKeepsLongExactGuards) {
  std::unique_ptr<Function> irfunc = build_specialized_source(
      "def test(a, b, c, d):\n"
      "    return (a + b) < (c + d)\n",
      {BINARY_OP_ADD_FLOAT, BINARY_OP_ADD_FLOAT, COMPARE_OP_INT});

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<LongExact>"), 2);
}

TEST_F(HIRBuildTest, BackedgeSpecializedIntCompareKeepsLongExactGuards) {
  std::unique_ptr<Function> irfunc = build_specialized_source(
      "def test(a, b):\n"
      "    while True:\n"
      "        a < b\n",
      COMPARE_OP_INT);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<LongExact>"), 2);
}

#if PY_VERSION_HEX >= 0x030E0000
TEST_F(
    HIRBuildTest,
    JumpBackwardJitOpcodeSpecializedIntBinaryOpKeepsLongExactGuards) {
  std::unique_ptr<Function> irfunc = build_specialized_source(
      "def test(a, b):\n"
      "    while True:\n"
      "        a + b\n",
      BINARY_OP_ADD_INT,
      JUMP_BACKWARD_JIT);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<LongExact>"), 2);
}

TEST_F(
    HIRBuildTest,
    JumpBackwardNoJitOpcodeSpecializedIntCompareKeepsLongExactGuards) {
  std::unique_ptr<Function> irfunc = build_specialized_source(
      "def test(a, b):\n"
      "    while True:\n"
      "        a < b\n",
      COMPARE_OP_INT,
      JUMP_BACKWARD_NO_JIT);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<LongExact>"), 2);
}
#endif
#endif

#if PY_VERSION_HEX < 0x030E0000
TEST_F(HIRBuildTest, LoadAssertionError) {
  // No LOAD_ASSERTION_ERROR on 3.14 and later
  //  0 LOAD_ASSERTION_ERROR
  //  2 RETURN_VALUE
  uint8_t bc[] = {LOAD_ASSERTION_ERROR, 0, RETURN_VALUE, 0};
  Ref<> bytecode = toByteString(bc);
  ASSERT_NE(bytecode.get(), nullptr);
  auto filename = Ref<>::steal(PyUnicode_FromString("filename"));
  auto funcname = Ref<>::steal(PyUnicode_FromString("funcname"));
  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto empty_bytes = Ref<>::steal(PyBytes_FromString(""));
  auto code = Ref<PyCodeObject>::steal(PyUnstable_Code_New(
      /*argcount=*/0,
      /*kwonlyargcount=*/0,
      /*nlocals=*/0,
      /*stacksize=*/0,
      /*flags=*/0,
      bytecode,
      /*consts=*/empty_tuple,
      /*names=*/empty_tuple,
      /*varnames=*/empty_tuple,
      /*freevars=*/empty_tuple,
      /*cellvars=*/empty_tuple,
      filename,
      funcname,
      /*_unused_qualname=*/funcname,
      /*firstlineno=*/0,
      /*linetable=*/empty_bytes,
      /*_unused_exceptiontable=*/empty_bytes));
  ASSERT_NE(code.get(), nullptr);

  auto func = Ref<PyFunctionObject>::steal(PyFunction_New(code, MakeGlobals()));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));

#if PY_VERSION_HEX < 0x030C0000
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
    }
    v1 = LoadConst<MortalTypeExact[AssertionError:obj]>
    Return v1
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
    }
    v1 = LoadConst<ImmortalTypeExact[AssertionError:obj]>
    Return v1
  }
}
)";
#endif
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}
#endif

TEST_F(HIRBuildTest, SetUpdate) {
  //  0 LOAD_FAST    0
  //  2 LOAD_FAST    1
  //  4 LOAD_FAST    2
  //  6 SET_UPDATE   1
  //  8 ROT_TWO
  //  10 POP_TOP
  //  12 RETURN_VALUE
  uint8_t bc[] = {
      LOAD_FAST,
      0,
      LOAD_FAST,
      1,
      LOAD_FAST,
      2,
      SET_UPDATE,
      1,

      SWAP,
      2,
      POP_TOP,
      0,
      RETURN_VALUE,
      0,
  };
  Ref<> bytecode = toByteString(bc);
  ASSERT_NE(bytecode.get(), nullptr);
  auto filename = Ref<>::steal(PyUnicode_FromString("filename"));
  auto funcname = Ref<>::steal(PyUnicode_FromString("funcname"));
  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto param0 = Ref<>::steal(PyUnicode_FromString("param0"));
  auto param1 = Ref<>::steal(PyUnicode_FromString("param1"));
  auto param2 = Ref<>::steal(PyUnicode_FromString("param2"));
  auto varnames =
      Ref<>::steal(PyTuple_Pack(3, param0.get(), param1.get(), param2.get()));
  auto empty_bytes = Ref<>::steal(PyBytes_FromString(""));
  auto code = Ref<PyCodeObject>::steal(PyUnstable_Code_New(
      /*argcount=*/3,
      /*kwonlyargcount=*/0,
      /*nlocals=*/3,
      /*stacksize=*/0,
      /*flags=*/0,
      bytecode,
      /*consts=*/empty_tuple,
      /*names=*/empty_tuple,
      varnames,
      /*freevars=*/empty_tuple,
      /*cellvars=*/empty_tuple,
      filename,
      funcname,
      /*_unused_qualname=*/funcname,
      /*firstlineno=*/0,
      /*linetable=*/empty_bytes,
      /*_unused_exceptiontable=*/empty_bytes));
  ASSERT_NE(code.get(), nullptr);

  auto func = Ref<PyFunctionObject>::steal(PyFunction_New(code, MakeGlobals()));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));

#if PY_VERSION_HEX < 0x030C0000
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kCheckVar), 3);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kSetUpdate), 1);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kReturn), 1);
#else
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadArg<1; "param1">
    v2 = LoadArg<2; "param2">
    v3 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<3> v0 v1 v2
    }
    v4 = SetUpdate v1 v2 {
      FrameState {
        CurInstrOffset 6
        Locals<3> v0 v1 v2
        Stack<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 8
      Locals<3> v0 v1 v2
      Stack<2> v0 v1
    }
    Return v1
  }
}
)";
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
#endif
}

class EdgeCaseTest : public RuntimeTest {};

TEST_F(EdgeCaseTest, IgnoreUnreachableLoops) {
  //  0 LOAD_CONST    0
  //  2 RETURN_VALUE
  //
  //  4 LOAD_CONST    0
  //  6 RETURN_VALUE
  //  8 JUMP_ABSOLUTE 4
  uint8_t bc[] = {
      LOAD_CONST,
      0,
      RETURN_VALUE,
      0,
      LOAD_CONST,
      0,
      RETURN_VALUE,
      0,
      JUMP_BACKWARD,
      2,
#if PY_VERSION_HEX >= 0x030E0000
      // inline-cache slot for 3.14+
      0,
      0
#endif
  };
  Ref<> bytecode = toByteString(bc);
  ASSERT_NE(bytecode.get(), nullptr);
  auto filename = Ref<>::steal(PyUnicode_FromString("filename"));
  auto funcname = Ref<>::steal(PyUnicode_FromString("funcname"));
  auto consts = Ref<>::steal(PyTuple_New(1));
  Py_INCREF(Py_None);
  PyTuple_SET_ITEM(consts.get(), 0, Py_None);
  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto empty_bytes = Ref<>::steal(PyBytes_FromString(""));
  auto code = Ref<PyCodeObject>::steal(PyUnstable_Code_New(
      /*argcount=*/0,
      /*kwonlyargcount=*/0,
      /*nlocals=*/0,
      /*stacksize=*/0,
      /*flags=*/0,
      bytecode,
      consts,
      /*names=*/empty_tuple,
      /*varnames=*/empty_tuple,
      /*freevars=*/empty_tuple,
      /*cellvars=*/empty_tuple,
      filename,
      funcname,
      /*_unused_qualname=*/funcname,
      /*firstlineno=*/0,
      /*linetable=*/empty_bytes,
      /*_unused_exceptiontable=*/empty_bytes));
  ASSERT_NE(code.get(), nullptr);

  auto func = Ref<PyFunctionObject>::steal(PyFunction_New(code, MakeGlobals()));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
    }
    v1 = LoadConst<ImmortalNoneType>
    Return v1
  }
}
)";
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

TEST_F(EdgeCaseTest, JumpBackwardNoInterrupt) {
  //  0 LOAD_CONST    0
  //  2 RETURN_VALUE
  //
  //  4 LOAD_CONST    0
  //  6 RETURN_VALUE
  //  8 JUMP_ABSOLUTE 4
  uint8_t bc[] = {
      LOAD_CONST,
      0,
      RETURN_VALUE,
      0,
      LOAD_CONST,
      0,
      RETURN_VALUE,
      0,
      JUMP_BACKWARD_NO_INTERRUPT,
      2,
  };
  Ref<> bytecode = toByteString(bc);
  ASSERT_NE(bytecode.get(), nullptr);
  auto filename = Ref<>::steal(PyUnicode_FromString("filename"));
  auto funcname = Ref<>::steal(PyUnicode_FromString("funcname"));
  auto consts = Ref<>::steal(PyTuple_New(1));
  Py_INCREF(Py_None);
  PyTuple_SET_ITEM(consts.get(), 0, Py_None);
  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto empty_bytes = Ref<>::steal(PyBytes_FromString(""));
  auto code = Ref<PyCodeObject>::steal(PyUnstable_Code_New(
      /*argcount=*/0,
      /*kwonlyargcount=*/0,
      /*nlocals=*/0,
      /*stacksize=*/0,
      /*flags=*/0,
      bytecode,
      consts,
      /*names=*/empty_tuple,
      /*varnames=*/empty_tuple,
      /*freevars=*/empty_tuple,
      /*cellvars=*/empty_tuple,
      filename,
      funcname,
      /*_unused_qualname=*/funcname,
      /*firstlineno=*/0,
      /*linetable=*/empty_bytes,
      /*_unused_exceptiontable=*/empty_bytes));
  ASSERT_NE(code.get(), nullptr);

  auto func = Ref<PyFunctionObject>::steal(PyFunction_New(code, MakeGlobals()));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc.get(), nullptr);
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
    }
    v1 = LoadConst<ImmortalNoneType>
    Return v1
  }
}
)";
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

class CppInlinerTest : public RuntimeTest {};

TEST_F(CppInlinerTest, ChangingCalleeFunctionCodeCausesDeopt) {
  const char* pycode = R"(
def other():
  return 2

other_code = other.__code__

def g():
  return 1

def f():
  return g()
)";
  // Compile f
  Ref<PyObject> pyfunc(compileAndGet(pycode, "f"));
  ASSERT_NE(pyfunc, nullptr) << "Failed compiling func";
  // Call f
  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto call_result1 =
      Ref<>::steal(PyObject_Call(pyfunc, empty_tuple, /*kwargs=*/nullptr));
  EXPECT_TRUE(isIntEquals(call_result1, 1));
  // Set __code__
  Ref<PyObject> other_code(getGlobal("other_code"));
  ASSERT_NE(other_code, nullptr) << "Failed to get other_code global";
  int result = PyObject_SetAttrString(pyfunc, "__code__", other_code);
  ASSERT_NE(result, -1) << "Failed to set __code__";
  // Call f again
  auto call_result2 =
      Ref<>::steal(PyObject_Call(pyfunc, empty_tuple, /*kwargs=*/nullptr));
  EXPECT_TRUE(isIntEquals(call_result2, 2));
}

TEST_F(CppInlinerTest, ColdCallThresholdZeroDoesNotPruneAll) {
  struct RestoreFlag {
    size_t old_value;
    ~RestoreFlag() {
      getMutableConfig().inliner_cold_call_threshold = old_value;
    }
  } restore{getConfig().inliner_cold_call_threshold};

  // Setting threshold to 0 should disable pruning, not prune everything.
  getMutableConfig().inliner_cold_call_threshold = 0;

  const char* pycode = R"(
def foo():
    return 4

def test():
    return foo()
)";
  std::unique_ptr<Function> irfunc;
  ASSERT_NO_FATAL_FAILURE(CompileToHIR(pycode, "test", irfunc));
  ASSERT_NE(irfunc, nullptr);

  // Run the inliner; should not crash or throw.
  InlineFunctionCalls inliner;
  EXPECT_NO_THROW(inliner.Run(*irfunc));
}

TEST_F(CppInlinerTest, CallFuncWithKeywordArgs) {
  const char* pycode = R"(
def f(a, b, c=3):
    return a + b + c

def test():
    return f(1, b=2)
)";
  Ref<PyObject> pyfunc(compileAndGet(pycode, "test"));
  ASSERT_NE(pyfunc, nullptr);
  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(pyfunc, empty_tuple, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(isIntEquals(result, 6));
}

class HIRCloneTest : public RuntimeTest {};

TEST_F(HIRCloneTest, CanCloneInstrs) {
  Environment env;
  auto v0 = env.AllocateRegister();
  std::unique_ptr<Instr> load_const(
      LoadConst::create(v0, Type::fromObject(Py_False)));
  std::unique_ptr<Instr> new_load(load_const->clone());
  ASSERT_TRUE(new_load->IsLoadConst());
  EXPECT_TRUE(
      static_cast<LoadConst*>(new_load.get())->type() ==
      static_cast<LoadConst*>(load_const.get())->type());
  EXPECT_NE(load_const, new_load);
  EXPECT_EQ(load_const->output()->instr(), load_const.get());
  EXPECT_EQ(new_load->output()->instr(), load_const.get());
}

TEST_F(HIRCloneTest, CanCloneBranches) {
  Environment env;
  CFG cfg;
  BasicBlock* from = cfg.AllocateBlock();
  BasicBlock* to = cfg.AllocateBlock();
  cfg.entry_block = from;
  from->append<Branch>(to);
  Instr* branch = from->GetTerminator();
  std::unique_ptr<Instr> new_branch(branch->clone());
  ASSERT_TRUE(new_branch->IsBranch());
  EXPECT_EQ(branch->block(), from);
  EXPECT_EQ(new_branch->block(), nullptr);

  Edge* orig_edge = static_cast<Branch*>(branch)->edge(0);
  // Make sure that the two edges are different pointers with the same fields
  Edge* dup_edge = static_cast<Branch*>(new_branch.get())->edge(0);
  EXPECT_NE(orig_edge, dup_edge);

  EXPECT_EQ(orig_edge->from(), dup_edge->from());
  EXPECT_TRUE(from->out_edges().contains(orig_edge));
  EXPECT_TRUE(from->out_edges().contains(dup_edge));

  EXPECT_EQ(orig_edge->to(), dup_edge->to());
  EXPECT_TRUE(to->in_edges().contains(orig_edge));
  EXPECT_TRUE(to->in_edges().contains(dup_edge));
}

TEST_F(HIRCloneTest, CanCloneBorrwedRefFields) {
  Environment env;
  auto v0 = env.AllocateRegister();
  auto name = Ref<>::steal(PyUnicode_FromString("test"));
  std::unique_ptr<Instr> check(CheckVar::create(v0, v0, name));
  std::unique_ptr<Instr> new_check(check->clone());
  ASSERT_TRUE(new_check->IsCheckVar());
  BorrowedRef<> orig_name = static_cast<CheckVar*>(check.get())->name();
  BorrowedRef<> dup_name = static_cast<CheckVar*>(new_check.get())->name();
  EXPECT_EQ(orig_name, dup_name);
}

TEST_F(HIRCloneTest, CanCloneVariadicOpInstr) {
  Environment env;
  auto out = env.AllocateRegister();
  auto v0 = env.AllocateRegister();

  // Create a CallStatic with no arguments
  std::unique_ptr<Instr> call_static_no_args(
      CallStatic::create(0, out, nullptr, Type::fromObject(Py_None)));
  std::unique_ptr<Instr> new_call_static_no_args(call_static_no_args->clone());
  ASSERT_NE(call_static_no_args.get(), new_call_static_no_args.get());
  ASSERT_TRUE(new_call_static_no_args->IsCallStatic());

  CallStatic* orig_call = static_cast<CallStatic*>(call_static_no_args.get());
  CallStatic* dup_call =
      static_cast<CallStatic*>(new_call_static_no_args.get());
  EXPECT_EQ(orig_call->addr(), dup_call->addr());
  EXPECT_EQ(orig_call->ret_type(), dup_call->ret_type());

  // Create a CallStatic with one argument
  std::unique_ptr<Instr> call_static_one_arg(
      CallStatic::create(1, out, nullptr, Type::fromObject(Py_None), v0));
  std::unique_ptr<Instr> new_call_static_one_arg(call_static_one_arg->clone());
  ASSERT_NE(call_static_one_arg.get(), new_call_static_one_arg.get());
  ASSERT_TRUE(new_call_static_one_arg->IsCallStatic());

  orig_call = static_cast<CallStatic*>(call_static_one_arg.get());
  dup_call = static_cast<CallStatic*>(new_call_static_one_arg.get());
  EXPECT_EQ(orig_call->addr(), dup_call->addr());
  EXPECT_EQ(orig_call->ret_type(), dup_call->ret_type());
  EXPECT_EQ(orig_call->GetOperand(0), dup_call->GetOperand(0));

  // Create a CallStatic with two arguments
  std::unique_ptr<Instr> call_static_two_args(
      CallStatic::create(2, out, nullptr, Type::fromObject(Py_None), v0, v0));
  std::unique_ptr<Instr> new_call_static_two_args(
      call_static_two_args->clone());
  ASSERT_NE(call_static_two_args.get(), new_call_static_two_args.get());
  ASSERT_TRUE(new_call_static_two_args->IsCallStatic());

  orig_call = static_cast<CallStatic*>(call_static_two_args.get());
  dup_call = static_cast<CallStatic*>(new_call_static_two_args.get());
  EXPECT_EQ(orig_call->addr(), dup_call->addr());
  EXPECT_EQ(orig_call->ret_type(), dup_call->ret_type());
  EXPECT_EQ(orig_call->GetOperand(0), dup_call->GetOperand(0));
  EXPECT_EQ(orig_call->GetOperand(1), dup_call->GetOperand(1));
}

TEST_F(HIRCloneTest, CanCloneDeoptBase) {
  const char* hir = R"(fun jittestmodule:test {
  bb 0 {
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v1 = LoadConst<ImmortalLongExact[1]>
    v0 = Assign v1
    v2 = LoadGlobal<0; "foo"> {
      FrameState {
        CurInstrOffset 6
        Locals<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 6
      Locals<1> v0
      Stack<1> v2
    }
    Return v2
  }
}
)";
  auto irfunc = HIRParser().ParseHIR(hir);
  ASSERT_NE(irfunc, nullptr);
  ASSERT_TRUE(checkFunc(*irfunc, std::cout));
  reflowTypes(*irfunc);
  RefcountInsertion().Run(*irfunc);
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v1:ImmortalLongExact[1] = LoadConst<ImmortalLongExact[1]>
    v2:Object = LoadGlobal<0> {
      LiveValues<1> unc:v1
      FrameState {
        CurInstrOffset 6
        Locals<1> v1
      }
    }
    Return v2
  }
}
)";
  ASSERT_EQ(fullPrinter().ToString(*irfunc), expected);
  BasicBlock* bb0 = irfunc->cfg.entry_block;
  Instr& load_global = *(++(bb0->rbegin()));
  ASSERT_TRUE(load_global.IsLoadGlobal());

  std::unique_ptr<Instr> dup_load(load_global.clone());
  ASSERT_TRUE(dup_load->IsLoadGlobal());

  LoadGlobal* orig = static_cast<LoadGlobal*>(&load_global);
  LoadGlobal* dup = static_cast<LoadGlobal*>(dup_load.get());

  EXPECT_EQ(orig->output(), dup->output());
  EXPECT_EQ(orig->name_idx(), dup->name_idx());

  FrameState* orig_fs = orig->frameState();
  FrameState* dup_fs = dup->frameState();
  // Should not be pointer equal, but have equal contents
  EXPECT_NE(orig_fs, dup_fs);
  EXPECT_TRUE(*orig_fs == *dup_fs);

  // Should have equal contents
  EXPECT_TRUE(orig->live_regs() == dup->live_regs());
}

#if PY_VERSION_HEX >= 0x030C0000
TEST_F(HIRBuildTest, MatchMapping) {
  uint8_t bc[] = {LOAD_FAST, 0, MATCH_MAPPING, 0, RETURN_VALUE, 0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None});

#if PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = LoadField<ob_type@8, Type, borrowed> v0
    v3 = LoadField<tp_flags@168, CUInt64, borrowed> v2
    v4 = LoadConst<CUInt64[64]>
    v5 = IntBinaryOp<And> v3 v4
    CondBranch<1, 2> v5
  }

  bb 1 (preds 0) {
    v6 = LoadConst<ImmortalBool[True]>
    Branch<3>
  }

  bb 2 (preds 0) {
    v6 = LoadConst<ImmortalBool[False]>
    Branch<3>
  }

  bb 3 (preds 1, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<2> v0 v6
    }
    v2 = Assign v0
    Return v6
  }
}
)";
#elif PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = LoadField<ob_type@8, Type, borrowed> v0
    v3 = LoadField<tp_flags@168, CUInt64, borrowed> v2
    v4 = LoadConst<CUInt64[64]>
    v5 = IntBinaryOp<And> v3 v4
    CondBranch<1, 2> v5
  }

  bb 1 (preds 0) {
    v6 = LoadConst<ImmortalBool[True]>
    Branch<3>
  }

  bb 2 (preds 0) {
    v6 = LoadConst<ImmortalBool[False]>
    Branch<3>
  }

  bb 3 (preds 1, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<2> v0 v6
    }
    v2 = Assign v0
    Return v6
  }
}
)";
#endif
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

TEST_F(HIRBuildTest, MatchSequence) {
  uint8_t bc[] = {LOAD_FAST, 0, MATCH_SEQUENCE, 0, RETURN_VALUE, 0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None});

#if PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = LoadField<ob_type@8, Type, borrowed> v0
    v3 = LoadField<tp_flags@168, CUInt64, borrowed> v2
    v4 = LoadConst<CUInt64[32]>
    v5 = IntBinaryOp<And> v3 v4
    CondBranch<1, 2> v5
  }

  bb 1 (preds 0) {
    v6 = LoadConst<ImmortalBool[True]>
    Branch<3>
  }

  bb 2 (preds 0) {
    v6 = LoadConst<ImmortalBool[False]>
    Branch<3>
  }

  bb 3 (preds 1, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<2> v0 v6
    }
    v2 = Assign v0
    Return v6
  }
}
)";
#elif PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = LoadField<ob_type@8, Type, borrowed> v0
    v3 = LoadField<tp_flags@168, CUInt64, borrowed> v2
    v4 = LoadConst<CUInt64[32]>
    v5 = IntBinaryOp<And> v3 v4
    CondBranch<1, 2> v5
  }

  bb 1 (preds 0) {
    v6 = LoadConst<ImmortalBool[True]>
    Branch<3>
  }

  bb 2 (preds 0) {
    v6 = LoadConst<ImmortalBool[False]>
    Branch<3>
  }

  bb 3 (preds 1, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<2> v0 v6
    }
    v2 = Assign v0
    Return v6
  }
}
)";
#endif
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}
#endif

#if PY_VERSION_HEX >= 0x030C0000
TEST_F(HIRBuildTest, MatchKeys) {
  uint8_t bc[] = {LOAD_FAST, 0, LOAD_FAST, 1, MATCH_KEYS, 0, RETURN_VALUE, 0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None, Py_None});

  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = MatchKeys v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    v4 = LoadConst<ImmortalNoneType>
    v5 = PrimitiveCompare<Equal> v3 v4
    CondBranch<1, 2> v5
  }

  bb 1 (preds 0) {
    v3 = RefineType<NoneType> v3
    Branch<3>
  }

  bb 2 (preds 0) {
    v3 = RefineType<TupleExact> v3
    Branch<3>
  }

  bb 3 (preds 1, 2) {
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<3> v0 v1 v3
    }
    v6 = Assign v3
    v3 = Assign v0
    v4 = Assign v1
    Return v6
  }
}
)";
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}
#endif

TEST_F(HIRBuildTest, ListExtend) {
  uint8_t bc[] = {LOAD_FAST, 0, LOAD_FAST, 1, LIST_EXTEND, 1, RETURN_VALUE, 0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None, Py_None});

#if PY_VERSION_HEX < 0x030C0000
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kCheckVar), 2);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kListExtend), 1);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kReturn), 1);
#else
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = ListExtend v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<1> v0
    }
    Return v0
  }
}
)";
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
#endif
}

#if PY_VERSION_HEX >= 0x030C0000
TEST_F(HIRBuildTest, ListToTuple) {
  uint8_t bc[] = {
      LOAD_FAST, 0, CALL_INTRINSIC_1, INTRINSIC_LIST_TO_TUPLE, RETURN_VALUE, 0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None});

#if PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = CallIntrinsic<INTRINSIC_LIST_TO_TUPLE> v0
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<1> v2
    }
    Return v2
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = CallIntrinsic<6> v0
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<1> v2
    }
    Return v2
  }
}
)";
#endif
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}
#endif

TEST_F(HIRBuildTest, BinaryOpSubscrDictSpecializationGuards) {
  const char* src = R"(
def test(container, key):
    return container[key]

for _ in range(100):
    test({"a": "b"}, "a")
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  const std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_NE(hir.find("GuardType<DictExact>"), std::string::npos) << hir;
  EXPECT_NE(hir.find("BinaryOp<Subscript>"), std::string::npos) << hir;
}

TEST_F(HIRBuildTest, BinaryOpSubscrListIntSpecializationGuards) {
  const char* src = R"(
def test(container, index):
    return container[index]

for _ in range(100):
    test(["a", "b"], 0)
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  const std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_NE(hir.find("GuardType<ListExact>"), std::string::npos) << hir;
  EXPECT_NE(hir.find("GuardType<LongExact>"), std::string::npos) << hir;
  EXPECT_NE(hir.find("BinaryOp<Subscript>"), std::string::npos) << hir;
}

TEST_F(HIRBuildTest, BinaryOpSubscrTupleIntSpecializationGuards) {
  const char* src = R"(
def test(container, index):
    return container[index]

for _ in range(100):
    test(("a", "b"), 0)
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  const std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_NE(hir.find("GuardType<TupleExact>"), std::string::npos) << hir;
  EXPECT_NE(hir.find("GuardType<LongExact>"), std::string::npos) << hir;
  EXPECT_NE(hir.find("BinaryOp<Subscript>"), std::string::npos) << hir;
}

TEST_F(HIRBuildTest, StoreSubscrListIntSpecializationGuards) {
  const char* src = R"(
def test(container, index, value):
    container[index] = value

for _ in range(100):
    test(["a", "b"], 0, "c")
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  const std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_NE(hir.find("GuardType<ListExact>"), std::string::npos) << hir;
  EXPECT_NE(hir.find("GuardType<LongExact>"), std::string::npos) << hir;
  EXPECT_NE(hir.find("StoreSubscr"), std::string::npos) << hir;
}

#if PY_VERSION_HEX >= 0x030C0000
TEST_F(HIRBuildTest, LoadFastAndClear) {
  uint8_t bc[] = {
      LOAD_FAST_AND_CLEAR, 1, LOAD_FAST_CHECK, 0, POP_TOP, 0, RETURN_VALUE, 0};

  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None, Py_None});

  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = Assign v1
    v1 = LoadConst<Nullptr>
    v0 = CheckVar<"param0"> v0 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
        Stack<1> v3
      }
    }
    Return v3
  }
}
)";

  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}
#endif

TEST_F(HIRBuildTest, AtQuiescentStateInEvalBreakerCheck) {
  const char* src = R"(
def test():
    return 1
)";
  Ref<PyFunctionObject> funcobj(compileAndGet(src, "test"));
  ASSERT_NE(funcobj, nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(funcobj));
  ASSERT_NE(irfunc, nullptr);

  bool found_at_quiescent_state = false;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsAtQuiescentState()) {
        found_at_quiescent_state = true;
        break;
      }
    }
    if (found_at_quiescent_state) {
      break;
    }
  }

  EXPECT_EQ(found_at_quiescent_state, kFreeThreadedBuild)
      << "AtQuiescentState presence should match the build mode";
}

class HIRBuilderExtendedTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr);
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};

TEST_F(HIRBuilderExtendedTest, BuildSimpleFunc) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);

  EXPECT_FALSE(irfunc->cfg.GetRPOTraversal().empty());
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithAdd) {
  const char* py_src = R"(
def add(a, b):
    return a + b
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "add"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}
TEST_F(HIRBuilderExtendedTest, BuildFuncWithCompare) {
  const char* py_src = R"(
def cmp(a, b):
    return a > b
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "cmp"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithIf) {
  const char* py_src = R"(
def branch(x):
    if x > 0:
        return 1
    return 0
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "branch"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);

  auto traversal = irfunc->cfg.GetRPOTraversal();
  EXPECT_GE(traversal.size(), 2);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithLoop) {
  const char* py_src = R"(
def loop(n):
    total = 0
    for i in range(n):
        total += i
    return total
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "loop"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithAttrAccess) {
  const char* py_src = R"(
class MyClass:
    x = 10

def get_x():
    return MyClass.x
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "get_x"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithMethodCall) {
  const char* py_src = R"(
def call_method():
    return [1, 2, 3].append(4)
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "call_method"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithTuple) {
  const char* py_src = R"(
def make_tuple():
    return (1, 2, 3)
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "make_tuple"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithList) {
  const char* py_src = R"(
def make_list():
    return [1, 2, 3]
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "make_list"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithDict) {
  const char* py_src = R"(
def make_dict():
    return {"a": 1, "b": 2}
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "make_dict"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithSubscript) {
  const char* py_src = R"(
def get_item(lst, idx):
    return lst[idx]
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "get_item"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithUnaryOp) {
  const char* py_src = R"(
def negate(x):
    return -x
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "negate"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithNotOp) {
  const char* py_src = R"(
def not_op(x):
    return not x
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "not_op"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithBoolOp) {
  const char* py_src = R"(
def bool_and(a, b):
    return a and b
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "bool_and"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithTryExcept) {
  const char* py_src = R"(
def try_except():
    try:
        return 1
    except:
        return 0
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "try_except"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildGeneratorFunc) {
  const char* py_src = R"(
def gen():
    yield 1
    yield 2
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithGlobalAccess) {
  const char* py_src = R"(
GLOBAL_VAR = 42

def read_global():
    return GLOBAL_VAR
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "read_global"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithClosure) {
  const char* py_src = R"(
def outer():
    x = 10
    def inner():
        return x
    return inner()
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "outer"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithIsOperator) {
  const char* py_src = R"(
def is_none(x):
    return x is None
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "is_none"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithInOperator) {
  const char* py_src = R"(
def contains(lst, val):
    return val in lst
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "contains"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithFString) {
  const char* py_src = R"(
def greet(name):
    return f"hello {name}"
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "greet"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithStarExpr) {
  const char* py_src = R"(
def spread():
    return [*range(3)]
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "spread"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithAugAssign) {
  const char* py_src = R"(
def aug_assign(x):
    x += 1
    x -= 1
    x *= 2
    return x
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "aug_assign"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithDelete) {
  const char* py_src = R"(
def delete_var():
    x = 1
    del x
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "delete_var"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithAssert) {
  const char* py_src = R"(
def assert_true():
    assert True
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "assert_true"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithRaise) {
  const char* py_src = R"(
def raise_error():
    raise ValueError("error")
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "raise_error"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithWhileLoop) {
  const char* py_src = R"(
def while_loop(n):
    i = 0
    while i < n:
        i += 1
    return i
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "while_loop"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithBreakContinue) {
  const char* py_src = R"(
def break_continue():
    for i in range(10):
        if i == 3:
            continue
        if i == 7:
            break
    return i
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "break_continue"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithSetLiteral) {
  const char* py_src = R"(
def make_set():
    return {1, 2, 3}
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "make_set"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithSlice) {
  const char* py_src = R"(
def slice_list(lst):
    return lst[1:3]
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "slice_list"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithWalrus) {
  const char* py_src = R"(
def walrus(lst):
    if (n := len(lst)) > 0:
        return n
    return 0
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "walrus"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithMultipleReturns) {
  const char* py_src = R"(
def multi_ret(x):
    if x > 0:
        return 1
    elif x < 0:
        return -1
    else:
        return 0
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "multi_ret"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}
