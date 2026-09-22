// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/function.h"
#include "cinderx/Jit/hir/tree_iter_state_machine_pass.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <unordered_map>

class TreeIterStateMachineRuntimeTest : public RuntimeTest {};

TEST_F(TreeIterStateMachineRuntimeTest, HelperFailuresPropagate) {
  runStockCode(R"(
import os
import platform
import sys

import cinderx.jit as jit
from cinderx.jit import _deopt_gen

jit.compile_after_n_calls(1000000)

def state_machine_expected():
    enabled = os.environ.get("PYTHONJITTREEITERSTATEMACHINE", "1").lower()
    arch_supported = platform.machine().lower() in ("aarch64", "arm64")
    return enabled not in ("0", "false", "no") and arch_supported

class Node:
    __slots__ = ("value", "left", "right")

    def __init__(self, value, left=None, right=None):
        self.value = value
        self.left = left
        self.right = right

    def __iter__(self):
        if self.left is not None:
            yield from self.left
        yield self.value
        if self.right is not None:
            yield from self.right

assert jit.force_compile(Node.__iter__)
counts = jit.get_function_hir_opcode_counts(Node.__iter__)
if state_machine_expected():
    assert counts.get("EnsureTreeIterState", 0) > 0, counts

root = Node(4, Node(2, Node(1), Node(3)), Node(6, Node(5), Node(7)))
gen = iter(root)
first = next(gen)
if state_machine_expected():
    assert not _deopt_gen(gen), "TreeIter deopt must fail closed while active"
else:
    assert _deopt_gen(gen)
assert [first, *list(gen)] == [1, 2, 3, 4, 5, 6, 7]

class Other:
    pass

try:
    list(Node(1, left=Other()))
except TypeError as exc:
    msg = str(exc)
    assert "TreeIter child type" in msg or "'Other' object is not iterable" in msg, msg
else:
    raise AssertionError("child type mismatch should fail")

if state_machine_expected():
    root = Node(1)
    root.left = root
    old_limit = sys.getrecursionlimit()
    try:
        sys.setrecursionlimit(50)
        try:
            list(root)
        except RecursionError:
            pass
        else:
            raise AssertionError("cycle should raise RecursionError")
    finally:
        sys.setrecursionlimit(old_limit)

class SplitNode:
    def __init__(self, left, value, right):
        self.left = left
        self.value = value
        self.right = right

    def __iter__(self):
        if self.left:
            yield from self.left
        yield self.value
        if self.right:
            yield from self.right

assert jit.force_compile(SplitNode.__iter__)
split_root = SplitNode(None, 1, None)
for i in range(50):
    setattr(split_root, f"extra_{i}", i)
assert list(split_root) == [1]
assert list(SplitNode(0, 1, [])) == [1]

class FalsyChild:
    checks = 0

    def __bool__(self):
        type(self).checks += 1
        return False

    def __iter__(self):
        raise AssertionError("falsy child must not be iterated")

child = FalsyChild()
assert list(SplitNode(child, 1, child)) == [1]
assert FalsyChild.checks == 2
del gen, root, split_root, child, Node, SplitNode, Other, FalsyChild
)");
}

TEST_F(TreeIterStateMachineRuntimeTest, GuardMustControlYieldFrom) {
  runStockCode(R"(
import cinderx.jit as jit

jit.compile_after_n_calls(1000000)

class MisplacedGuardNode:
    __slots__ = ("value", "left", "right")

    def __init__(self, value, left=None, right=None):
        self.value = value
        self.left = left
        self.right = right

    def __iter__(self):
        if self.left is not None:
            marker = 1
        yield from self.left
        yield self.value
        if self.right is not None:
            yield from self.right

assert jit.force_compile(MisplacedGuardNode.__iter__)
counts = jit.get_function_hir_opcode_counts(MisplacedGuardNode.__iter__)
assert counts.get("EnsureTreeIterState", 0) == 0, counts

try:
    list(MisplacedGuardNode(42))
except TypeError:
    pass
else:
    raise AssertionError("bare yield-from None must keep raising TypeError")
del MisplacedGuardNode
)");
}

TEST_F(TreeIterStateMachineRuntimeTest, PhiCycleInYieldFromTraceTerminates) {
  // Hand-built HIR that satisfies the matcher's shape requirements (one
  // InitialYield, LoadArg(0), one plain YieldValue, two yield-from
  // YieldValues) but routes both yield-from iterables through a self-
  // referencing Phi.  Before the shared on-path cycle guard, each Phi arm
  // restarted the trace depth from zero, so a cyclic dataflow region
  // recursed until the stack overflowed.  The pass must now bail out
  // conservatively and leave the function unmodified.
  Ref<> stock = compileStockAndGet(
      R"(
class Node:
    def __iter__(self):
        yield self.value
        yield from self.left
        yield from self.right

target = Node.__iter__
)",
      "target");

  jit::hir::Function func;
  auto b0 = func.cfg.entry_block = func.cfg.AllocateBlock();
  auto b1 = func.cfg.AllocateBlock();
  auto v_self = func.env.AllocateRegister();
  auto v_init = func.env.AllocateRegister();
  auto v_plain = func.env.AllocateRegister();
  auto v_yf1 = func.env.AllocateRegister();
  auto v_yf2 = func.env.AllocateRegister();
  auto v_phi = func.env.AllocateRegister();

  jit::hir::FrameState frame;
  b0->append<jit::hir::LoadArg>(v_self, 0);
  b0->append<jit::hir::InitialYield>(v_init, frame);
  b0->append<jit::hir::YieldValue>(v_plain, v_self, frame);
  auto* yf_first = b0->append<jit::hir::YieldValue>(v_yf1, v_self, frame);
  auto* yf_second = b0->append<jit::hir::YieldValue>(v_yf2, v_self, frame);
  yf_first->setYieldFromIter(v_phi);
  yf_second->setYieldFromIter(v_phi);
  b0->append<jit::hir::Branch>(b1);

  std::unordered_map<jit::hir::BasicBlock*, jit::hir::Register*> phi_args{
      {b1, v_phi}};
  b1->append<jit::hir::Phi>(v_phi, phi_args);
  b1->append<jit::hir::Branch>(b1);

  func.setCode(reinterpret_cast<PyFunctionObject*>(stock.get())->func_code);
  jit::hir::TreeIterStateMachinePass().Run(func);

  // Conservative exit: the function must not have been rewritten into a
  // state machine.
  EXPECT_EQ(
      func.CountInstrs([](const jit::hir::Instr& instr) {
        return instr.opcode() == jit::hir::Opcode::kEnsureTreeIterState;
      }),
      0);
}
