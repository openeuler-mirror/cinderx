# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Per-function JIT size budgets.

A function whose HIR (``jit-max-hir-instrs`` / ``jit-max-hir-blocks``) or LIR
(``jit-max-lir-instrs`` / ``jit-max-lir-blocks``) is larger than the budget is
not compiled: the compile is abandoned and the function keeps running in the
interpreter.  The HIR budget is checked when the optimizer starts; the LIR
budget is checked right after lowering to LIR, before register allocation.

The budgets are startup options, so every case runs in a fresh child
interpreter.  Each limited run is paired with a control run of the same
program under the default limits: the control proves that the JIT is really
enabled in the child and that the function is compilable, so the fallback in
the limited run can only be caused by the limit.

Compiling any real function produces more than one HIR/LIR instruction and,
for a branchy function, more than one basic block, so a limit of 1 is
guaranteed to trip regardless of Python version or architecture.  A limit of 0
is not special: it rejects every function.
"""

import json
import os
import subprocess
import sys
import unittest

from cinderx.test_support import (
    ENCODING,
    get_cinderjit_xargs,
    skip_unless_jit,
    subprocess_env,
)


_HIR_REASON = "REFUSE_SHAPE_HIR_SIZE_LIMIT"
_LIR_REASON = "REFUSE_SHAPE_LIR_SIZE_LIMIT"

_TINY_HIR_INSTRS_LIMIT = "jit-max-hir-instrs=1"
_TINY_HIR_BLOCKS_LIMIT = "jit-max-hir-blocks=1"
_TINY_LIR_INSTRS_LIMIT = "jit-max-lir-instrs=1"
_TINY_LIR_BLOCKS_LIMIT = "jit-max-lir-blocks=1"

# Shared by every child program.  `compile_or_refuse` compiles `func` and
# reports what happened; on CPython 3.11 `refusal_reason` asks the scheduling
# gate why a function is not compiled.
_PRELUDE = """
import sys
import cinderx.jit as jit

assert jit.is_enabled(), "the JIT is not enabled in the child process"


def compile_or_refuse(func):
    try:
        jit.force_compile(func)
    except RuntimeError as exc:
        return str(exc)
    return None


def refusal_reason(func):
    if sys.version_info >= (3, 12):
        return None
    import cinderjit

    return cinderjit._jit311_compile_diagnostic(func)["reason"]
"""

# A trivial straight-line function: more than one instruction (HIR or LIR).
_STRAIGHT_LINE = (
    _PRELUDE
    + """
def sample(x):
    return x + 1


def probe(x):
    return x + 2


refused = compile_or_refuse(sample)
if refused is None:
    assert jit.is_jit_compiled(sample), "force_compile returned but did not compile"
    print("COMPILED_OK")
else:
    assert not jit.is_jit_compiled(sample), "expected interpreter fallback"
    print("REFUSED", refused)
    print("REASON", refusal_reason(probe))

# Either way the function still computes the right answer.
for i in range(5):
    assert sample(i) == i + 1, "wrong result"
print("RESULT_OK")
"""
)

# A branchy function: more than one basic block (HIR or LIR).
_BRANCHY = (
    _PRELUDE
    + """
def branchy(x):
    if x > 0:
        return x * 2
    return -x


def probe(x):
    if x > 0:
        return x * 3
    return -x


refused = compile_or_refuse(branchy)
if refused is None:
    assert jit.is_jit_compiled(branchy), "force_compile returned but did not compile"
    print("COMPILED_OK")
else:
    assert not jit.is_jit_compiled(branchy), "expected interpreter fallback"
    print("REFUSED", refused)
    print("REASON", refusal_reason(probe))

for arg, expected in ((5, 10), (-3, 3), (0, 0)):
    assert branchy(arg) == expected, "wrong result"
print("RESULT_OK")
"""
)

# Calls an over-budget function many times through the lazy-compile entry.  The
# first call attempts the compile; once it is abandoned the function must go
# back to the interpreter entry and never try again.
_REPEATED_CALLS = (
    _PRELUDE
    + """
import cinderjit

def sample(x):
    return x + 1


cinderjit._clear_autojit_gate_stats()
assert jit.lazy_compile(sample)
for i in range(50):
    assert sample(i) == i + 1, "wrong result"
stats = cinderjit._autojit_gate_stats()
print("COMPILED" if jit.is_jit_compiled(sample) else "INTERPRETED")
print("OK", stats["forced_compile_ok"], "FALLBACK", stats["forced_compile_fallback"])
"""
)


def _child_env() -> dict[str, str]:
    # subprocess_env() only carries the module search path.  The JIT itself is
    # configured through CINDERX_* / PYTHONJIT* variables (and -X options), and
    # on CPython 3.11 a child without them would run with the JIT off.
    env = subprocess_env()
    for key, value in os.environ.items():
        if key.startswith(("CINDERX_", "PYTHONJIT")):
            env[key] = value
    return env


def _run_child(
    source: str, limit_options: list[str]
) -> subprocess.CompletedProcess[str]:
    args = [sys.executable, *get_cinderjit_xargs()]
    for option in limit_options:
        args.extend(["-X", option])
    args.extend(["-c", source])
    return subprocess.run(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        encoding=ENCODING,
        env=_child_env(),
        timeout=300,
    )


class SizeLimitTest(unittest.TestCase):
    def _assert_child_ok(self, proc: subprocess.CompletedProcess[str]) -> None:
        self.assertEqual(
            proc.returncode,
            0,
            f"child failed\nstdout={proc.stdout!r}\nstderr={proc.stderr!r}",
        )

    def _assert_compiles_by_default(self, source: str) -> None:
        proc = _run_child(source, [])
        self._assert_child_ok(proc)
        self.assertIn("COMPILED_OK", proc.stdout)
        self.assertIn("RESULT_OK", proc.stdout)

    def _assert_falls_back(self, source: str, limit: str, reason: str) -> None:
        self._assert_compiles_by_default(source)

        proc = _run_child(source, [limit])
        self._assert_child_ok(proc)
        self.assertNotIn("COMPILED_OK", proc.stdout)
        self.assertIn("REFUSED", proc.stdout)
        self.assertIn("RESULT_OK", proc.stdout)
        if sys.version_info < (3, 12):
            # CPython 3.11 reports a typed refusal, not a generic failure.
            self.assertIn(f"REASON {reason}", proc.stdout)

    @skip_unless_jit("size limits are exercised against a running JIT")
    def test_compiles_under_default_limit(self) -> None:
        self._assert_compiles_by_default(_STRAIGHT_LINE)
        self._assert_compiles_by_default(_BRANCHY)

    @skip_unless_jit("size limits are exercised against a running JIT")
    def test_hir_instrs_limit_triggers_fallback(self) -> None:
        self._assert_falls_back(_STRAIGHT_LINE, _TINY_HIR_INSTRS_LIMIT, _HIR_REASON)

    @skip_unless_jit("size limits are exercised against a running JIT")
    def test_hir_blocks_limit_triggers_fallback(self) -> None:
        self._assert_falls_back(_BRANCHY, _TINY_HIR_BLOCKS_LIMIT, _HIR_REASON)

    @skip_unless_jit("size limits are exercised against a running JIT")
    def test_lir_instrs_limit_triggers_fallback(self) -> None:
        self._assert_falls_back(_STRAIGHT_LINE, _TINY_LIR_INSTRS_LIMIT, _LIR_REASON)

    @skip_unless_jit("size limits are exercised against a running JIT")
    def test_lir_blocks_limit_triggers_fallback(self) -> None:
        self._assert_falls_back(_BRANCHY, _TINY_LIR_BLOCKS_LIMIT, _LIR_REASON)

    @skip_unless_jit("size limits are exercised against a running JIT")
    def test_zero_limit_rejects_every_function(self) -> None:
        # Unlike jit-max-code-size, 0 does not mean "unlimited".
        self._assert_falls_back(_STRAIGHT_LINE, "jit-max-hir-instrs=0", _HIR_REASON)
        self._assert_falls_back(_STRAIGHT_LINE, "jit-max-lir-instrs=0", _LIR_REASON)

    @skip_unless_jit("size limits are exercised against a running JIT")
    @unittest.skipIf(
        sys.version_info < (3, 12),
        "lazy_compile is withheld on the CPython 3.11 execute surface",
    )
    def test_over_budget_function_is_attempted_once(self) -> None:
        # Control: under the default limits the first call compiles, and the
        # counters see exactly one successful forced compile.
        proc = _run_child(_REPEATED_CALLS, [])
        self._assert_child_ok(proc)
        self.assertIn("COMPILED", proc.stdout.split())
        self.assertIn("OK 1 FALLBACK 0", proc.stdout)

        # Over budget: one failed attempt on the first call, then 49 calls that
        # go straight to the interpreter without trying again.
        for limit in (_TINY_HIR_INSTRS_LIMIT, _TINY_LIR_INSTRS_LIMIT):
            with self.subTest(limit=limit):
                proc = _run_child(_REPEATED_CALLS, [limit])
                self._assert_child_ok(proc)
                self.assertIn("INTERPRETED", proc.stdout.split())
                self.assertIn("OK 0 FALLBACK 1", proc.stdout)


# CPython 3.11 children use the explicit canary/execute bootstrap of the 3.11
# test suite rather than the plugin environment of the parent.
_BOOT_311 = """
import json
import _cinderx
import cinderx

cinderx.init()
_cinderx.install_frame_evaluator()
import cinderjit
"""

# What the scheduling gate says about one compile, and what the compile cost
# the CodeRuntime pool.
_DIAGNOSTIC_311 = (
    _BOOT_311
    + """
def target(value):
    return value + 1


def live():
    return cinderjit._jit311_lifecycle_snapshot()["jit"]["code_runtimes_live"]


before = live()
diagnostic = cinderjit._jit311_compile_diagnostic(target)
after = live()
print(json.dumps({
    "reason": diagnostic["reason"],
    "compiled": diagnostic["compiled"],
    "result": target(41),
    "live_before": before,
    "live_after": after,
}))
"""
)

# An automatically scheduled function: one attempt for the code object, then
# the interpreter, however many times it is called or recreated.
_AUTO_311 = (
    _BOOT_311
    + """
def factory(k):
    def hot(x):
        return x + k
    return hot


funcs = []
for k in range(3):
    f = factory(k)
    for i in range(120):
        assert f(i) == i + k, "wrong result"
    funcs.append(f)
events = [e for e in _cinderx._get_observe_stats()["events"]
          if e["qualname"].endswith("hot")]
print(json.dumps({
    "results": [e["result"] for e in events],
    "compiled": [cinderjit.is_jit_compiled(f) for f in funcs],
}))
"""
)


def _run_child_311(
    source: str, limit_options: list[str], *, mode: str, auto: str
) -> dict[str, object]:
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("CINDERX_", "PYTHONJIT", "PARALLEL_GC_"))
    }
    env.update(subprocess_env())
    env["CINDERX_JIT_MODE"] = mode
    env["PYTHONJITAUTO"] = auto
    args = [sys.executable]
    for option in limit_options:
        args.extend(["-X", option])
    args.extend(["-c", source])
    proc = subprocess.run(
        args,
        capture_output=True,
        encoding=ENCODING,
        env=env,
        timeout=300,
    )
    if proc.returncode != 0:
        raise AssertionError(
            f"child failed\nstdout={proc.stdout!r}\nstderr={proc.stderr!r}"
        )
    return json.loads(proc.stdout.strip().splitlines()[-1])


@unittest.skipUnless(
    sys.version_info[:2] == (3, 11), "CPython 3.11 refusal reporting and lifecycle"
)
class SizeLimit311Test(unittest.TestCase):
    def test_diagnostic_names_the_size_reason(self) -> None:
        control = _run_child_311(_DIAGNOSTIC_311, [], mode="canary", auto="1000000")
        self.assertIsNone(control["reason"])
        self.assertTrue(control["compiled"])
        for limit, reason in (
            (_TINY_HIR_INSTRS_LIMIT, _HIR_REASON),
            (_TINY_HIR_BLOCKS_LIMIT, _HIR_REASON),
            (_TINY_LIR_INSTRS_LIMIT, _LIR_REASON),
            (_TINY_LIR_BLOCKS_LIMIT, _LIR_REASON),
        ):
            with self.subTest(limit=limit):
                result = _run_child_311(
                    _DIAGNOSTIC_311, [limit], mode="canary", auto="1000000"
                )
                self.assertEqual(result["reason"], reason)
                self.assertFalse(result["compiled"])
                self.assertEqual(result["result"], 42)

    def test_refused_compile_leaves_no_code_runtime_behind(self) -> None:
        for limit in (
            _TINY_HIR_INSTRS_LIMIT,
            _TINY_LIR_INSTRS_LIMIT,
            _TINY_LIR_BLOCKS_LIMIT,
        ):
            with self.subTest(limit=limit):
                result = _run_child_311(
                    _DIAGNOSTIC_311, [limit], mode="canary", auto="1000000"
                )
                self.assertEqual(result["live_before"], result["live_after"])

    def test_auto_scheduling_attempts_an_over_budget_code_object_once(self) -> None:
        control = _run_child_311(_AUTO_311, [], mode="execute", auto="30")
        self.assertEqual(control["results"], ["installed"])
        self.assertIn(True, control["compiled"])
        for limit, reason in (
            (_TINY_HIR_INSTRS_LIMIT, _HIR_REASON),
            (_TINY_LIR_INSTRS_LIMIT, _LIR_REASON),
        ):
            with self.subTest(limit=limit):
                result = _run_child_311(_AUTO_311, [limit], mode="execute", auto="30")
                # One verdict for the code object; the two later function
                # objects built from it are never offered to the compiler.
                self.assertEqual(result["results"], [reason])
                self.assertEqual(result["compiled"], [False, False, False])


# Batch-compiles every registered unit with a pool of worker threads.  With a
# tiny LIR limit every unit is abandoned by its worker on the over-budget
# path inside NativeGenerator::compile; the abandoned CodeRuntimes must be
# handed to the context and released on the main thread once the workers
# join, not dropped on the workers themselves.
_MULTITHREADED = (
    _PRELUDE
    + """
import gc

import cinderx


def make_unit(k):
    def unit(x):
        return x * k

    return unit


units = [make_unit(k) for k in range(1, 13)]
for k, f in enumerate(units, start=1):
    assert f(3) == 3 * k, "wrong result"
    # Register the unit for the batch compiler; one call is below every auto
    # threshold, so the functions are not compiled yet.
    assert jit.lazy_compile(f)
pre = sum(jit.is_jit_compiled(f) for f in units)
gc.collect()
refs_before = sys.getrefcount(globals())

cinderx.jit.multithreaded_compile_test()

post = sum(jit.is_jit_compiled(f) for f in units)
gc.collect()
refs_after = sys.getrefcount(globals())
print("PRE", pre, "POST", post)
print("REFS", refs_before, refs_after)
for k, f in enumerate(units, start=1):
    assert f(4) == 4 * k, "wrong result"
print("RESULT_OK")
"""
)


@unittest.skipIf(
    sys.version_info < (3, 12),
    "the CPython 3.11 surface runs this scenario through its own bootstrap",
)
class MultithreadedSizeLimitTest(unittest.TestCase):
    _MT_OPTIONS = ["jit-multithreaded-compile-test", "jit-batch-compile-workers=4"]

    def _run(self, options: list[str]) -> subprocess.CompletedProcess[str]:
        proc = _run_child(_MULTITHREADED, options)
        self.assertEqual(
            proc.returncode,
            0,
            f"child failed\nstdout={proc.stdout!r}\nstderr={proc.stderr!r}",
        )
        self.assertIn("RESULT_OK", proc.stdout)
        return proc

    @skip_unless_jit("size limits are exercised against a running JIT")
    def test_worker_pool_compiles_registered_units(self) -> None:
        proc = self._run(self._MT_OPTIONS)
        self.assertIn("POST 12", proc.stdout)

    @skip_unless_jit("size limits are exercised against a running JIT")
    def test_over_budget_units_fall_back_and_still_work(self) -> None:
        proc = self._run(self._MT_OPTIONS + [_TINY_LIR_INSTRS_LIMIT])
        # One call per unit is below every auto threshold, so nothing compiled
        # before the batch; the worker-side refusal must compile nothing more.
        self.assertIn("PRE 0 POST 0", proc.stdout)
        # Every abandoned CodeRuntime held references to the shared globals
        # dict; once the batch is over they must all have been given back.
        refs = [l for l in proc.stdout.splitlines() if l.startswith("REFS ")]
        self.assertEqual(len(refs), 1, proc.stdout)
        _, before, after = refs[0].split()
        self.assertEqual(before, after, "abandoned CodeRuntimes leaked references")


# CPython 3.11 reaches the same worker pool through its private entry points
# (_jit311_register_for_compile / _jit311_multithreaded_compile_test), which
# the repository's own lifecycle checks also use.  On 3.11 the release of an
# abandoned CodeRuntime additionally returns it to the CodeRuntime pool, so
# this case checks both the pool census and the shared reference count.
_MULTITHREADED_311 = (
    _BOOT_311
    + """
import gc
import sys


def make_unit(k):
    def unit(x):
        return x * k

    return unit


def live():
    return cinderjit._jit311_lifecycle_snapshot()["jit"]["code_runtimes_live"]


units = [make_unit(k) for k in range(1, 13)]
for k, f in enumerate(units, start=1):
    assert f(3) == 3 * k, "wrong result"
    assert cinderjit._jit311_register_for_compile(f) is True
gc.collect()
live_before = live()
refs_before = sys.getrefcount(globals())

cinderjit._jit311_multithreaded_compile_test()

gc.collect()
print(json.dumps({
    "compiled": sum(cinderjit.is_jit_compiled(f) for f in units),
    "results_ok": all(f(4) == 4 * k for k, f in enumerate(units, start=1)),
    "live_before": live_before,
    "live_after": live(),
    "refs_before": refs_before,
    "refs_after": sys.getrefcount(globals()),
}))
"""
)


@unittest.skipUnless(
    sys.version_info[:2] == (3, 11), "CPython 3.11 worker pool and CodeRuntime pool"
)
class MultithreadedSizeLimit311Test(unittest.TestCase):
    _MT_OPTIONS = ["jit-multithreaded-compile-test", "jit-batch-compile-workers=4"]

    def _run(self, limits: list[str]) -> dict[str, object]:
        return _run_child_311(
            _MULTITHREADED_311,
            self._MT_OPTIONS + limits,
            mode="canary",
            auto="1000000",
        )

    def test_worker_pool_compiles_registered_units(self) -> None:
        result = self._run([])
        self.assertEqual(result["compiled"], 12)
        self.assertTrue(result["results_ok"])

    def test_over_budget_units_fall_back_and_release_everything(self) -> None:
        result = self._run([_TINY_LIR_INSTRS_LIMIT])
        # The worker-side refusal compiles nothing, the functions still run in
        # the interpreter, and the abandoned CodeRuntimes were released on the
        # main thread: none left in the pool census, no reference left behind.
        self.assertEqual(result["compiled"], 0)
        self.assertTrue(result["results_ok"])
        self.assertEqual(result["live_before"], result["live_after"])
        self.assertEqual(result["refs_before"], result["refs_after"])


if __name__ == "__main__":
    unittest.main()
