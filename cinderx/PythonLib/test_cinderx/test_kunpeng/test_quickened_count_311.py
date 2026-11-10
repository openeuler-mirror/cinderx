# Copyright (c) Meta Platforms, Inc. and affiliates.
"""The stock quickened count owns both evaluator paths and code lifetimes."""

import os
import subprocess
import sys
import textwrap
import tempfile
from pathlib import Path
import unittest


BATCH = r'''
import sys, gc, types, weakref
cached_count = sys._getquickenedcount
import cinderx, _cinderx
cinderx.init()
_cinderx.remove_frame_evaluator()
assert sys._getquickenedcount is cached_count

def normal(x): return x + 1

def raises(x): raise ValueError(x)

def invoke(functions, rounds, raising):
    for _ in range(rounds):
        for function in functions:
            try:
                value = function(2)
            except ValueError as exc:
                assert raising and exc.args == (2,)
            else:
                assert not raising and value == 3

def exercise(stock_calls, cinder_calls, cycles, hold_code, raising, expected=64):
    if stock_calls is None:
        return
    functions, refs, codes = [], [], []
    template = raises if raising else normal
    for _ in range(64):
        code = template.__code__.replace()
        function = types.FunctionType(code, globals())
        if cycles:
            function.cycle = function
        functions.append(function)
        refs.append(weakref.ref(code))
        if hold_code:
            codes.append(code)
    del function, code
    before = cached_count()
    assert before >= 0
    invoke(functions, stock_calls, raising)
    _cinderx.install_frame_evaluator()
    assert _cinderx.is_frame_evaluator_installed()
    invoke(functions, cinder_calls, raising)
    _cinderx.remove_frame_evaluator()
    assert cached_count() == before + expected, (before, cached_count())
    # Reentry and evaluator toggles must not credit an already-quickened code.
    _cinderx.install_frame_evaluator()
    invoke(functions, 10 if expected else 0, raising)
    _cinderx.remove_frame_evaluator()
    assert cached_count() == before + expected
    functions.clear()
    gc.collect()
    if hold_code:
        assert cached_count() == before + expected
        codes.clear()
        gc.collect()
    for ref in refs:
        assert ref() is None
    assert cached_count() == before, (before, cached_count())

# Warm the measuring code itself under stock before taking exact deltas.
for _ in range(20):
    exercise(None, None, None, None, None)
    invoke([normal], 1, False)
    invoke([raises], 1, True)
for cycle in range(4):
    exercise(0, 0, False, False, False, 0)
    exercise(4, 0, False, False, False, 0)
    if MODE != 'execute':
        exercise(0, 3, False, False, False, 0)
    exercise(0, 10, False, False, False)
    exercise(0, 10, True, False, False)
    exercise(0, 10, False, True, False)
    exercise(8, 10, False, False, False)  # Stock quickened first.
    exercise(4, 10, False, False, False)  # Cross-evaluator warmup.
    exercise(0, 10, False, False, True)
assert sys._getquickenedcount is cached_count
print('balanced code lifetimes')
'''

GENERATOR = r'''
import sys, gc, types, weakref
count = sys._getquickenedcount
import cinderx, _cinderx
cinderx.init()
_cinderx.remove_frame_evaluator()
def template():
    yield 1
    yield 2

def begin(functions):
    gens = []
    for function in functions:
        gen = function()
        assert next(gen) == 1
        gens.append(gen)
    return gens
for _ in range(20):
    begin([])
functions, refs = [], []
for _ in range(64):
    code = template.__code__.replace()
    functions.append(types.FunctionType(code, globals()))
    refs.append(weakref.ref(code))
del code
before = count()
_cinderx.install_frame_evaluator()
gens = begin(functions)
_cinderx.remove_frame_evaluator()
assert count() == before + 64, (before, count())
functions.clear()
gc.collect()
assert count() == before + 64
for gen in gens:
    gen.close()
del gen
gens.clear()
gc.collect()
for ref in refs:
    assert ref() is None
assert count() == before, (before, count())
print('balanced suspended code')
'''

OBSERVABILITY = r'''
import sys, types
import cinderx, _cinderx
cinderx.init()
_cinderx.remove_frame_evaluator()

def target(x):
    return x + 1

def collect(installed, profile):
    function = types.FunctionType(target.__code__.replace(), globals())
    events = []
    hidden = []
    def record(frame, event, arg):
        if 'cinderx quickening accounting' in frame.f_code.co_filename:
            hidden.append(event)
        if frame.f_code is function.__code__:
            events.append((event, frame.f_lineno - frame.f_code.co_firstlineno))
        return record
    setter = sys.setprofile if profile else sys.settrace
    if installed:
        _cinderx.install_frame_evaluator()
    setter(record)
    for _ in range(10):
        assert function(2) == 3
    setter(None)
    _cinderx.remove_frame_evaluator()
    assert not hidden, hidden
    return events

for profile in (False, True):
    expected = collect(False, profile)
    assert collect(True, profile) == expected

# Every function in a cold chain quickens at a different recursive depth.
def leaf(depth, seen):
    seen[0] = depth
    return child(depth + 1, seen)

def run_chain(installed):
    child = lambda depth, seen: seen[0]
    for _ in range(100):
        child = types.FunctionType(leaf.__code__.replace(), {'child': child})
    if installed:
        _cinderx.install_frame_evaluator()
    seen = [-1]
    try:
        child(0, seen)
    except RecursionError:
        pass
    else:
        raise AssertionError('recursion limit was not reached')
    _cinderx.remove_frame_evaluator()
    return seen[0]

old_limit = sys.getrecursionlimit()
sys.setrecursionlimit(70)
try:
    expected = run_chain(False)
    assert run_chain(True) == expected, (expected, run_chain(True))
finally:
    sys.setrecursionlimit(old_limit)
print('trace profile and recursion preserved')
'''


JIT_LIFETIME = r'''
import sys, gc, types, weakref
count = sys._getquickenedcount
import cinderx, _cinderx
cinderx.init()
_cinderx.install_frame_evaluator()
import cinderjit

def template(x): return x

def round_trip(function):
    assert function(3) == 3
    assert cinderjit.force_compile(function)
    before = _cinderx._get_trigger_stats()['machine_code_entries']
    assert function(3) == 3
    assert _cinderx._get_trigger_stats()['machine_code_entries'] > before
    assert cinderjit.force_uncompile(function)

for _ in range(20):
    round_trip(template)
_cinderx.remove_frame_evaluator()
gc.collect()
baseline = count()
functions, refs = [], []
for _ in range(64):
    code = template.__code__.replace()
    functions.append(types.FunctionType(code, globals()))
    refs.append(weakref.ref(code))
del code
_cinderx.install_frame_evaluator()
for function in functions:
    round_trip(function)
del function
_cinderx.remove_frame_evaluator()
assert count() == baseline + 64, (baseline, count())
functions.clear()
gc.collect()
for ref in refs:
    assert ref() is None
assert count() == baseline, (baseline, count())
print('JIT retirement balances the stock counter')
'''


@unittest.skipUnless(sys.version_info[:3] == (3, 11, 6), "CPython 3.11.6 accounting")
class QuickenedCount311Tests(unittest.TestCase):
    def run_probe(self, source, marker, mode="execute"):
        env = {k: v for k, v in os.environ.items() if k != "PYTHONPATH"
               and not k.startswith(("CINDERX_", "PYTHONJIT", "SPIKE_", "PARALLEL_GC_"))}
        env.update(CINDERX_JIT_MODE=mode, PYTHONJITAUTO="1000000")
        proc = subprocess.run([sys.executable, "-c", f"MODE = {mode!r}\n" + textwrap.dedent(source)],
                              env=env, capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn(marker, proc.stdout)

    def test_both_evaluators_account_and_retire_code_once(self):
        for mode in ("off", "observe", "shadow", "execute"):
            with self.subTest(mode=mode):
                self.run_probe(BATCH, 'balanced code lifetimes', mode)

    def test_suspended_generator_keeps_its_code_counted(self):
        self.run_probe(GENERATOR, 'balanced suspended code')

    def test_jit_retirement_balances_the_stock_counter(self):
        self.run_probe(JIT_LIFETIME, 'JIT retirement balances the stock counter')

    @unittest.skipUnless(hasattr(sys, "gettotalrefcount"), "real -R requires Py_DEBUG")
    def test_real_reference_leak_is_still_reported_by_regrtest(self):
        with tempfile.TemporaryDirectory() as directory:
            module = "test_quickened_counter_real_leak"
            Path(directory, module + ".py").write_text(
                "import unittest, cinderx, _cinderx\n"
                "cinderx.init()\n_cinderx.install_frame_evaluator()\n"
                "assert _cinderx.is_frame_evaluator_installed()\n"
                "leaked = []\n"
                "class Leak(unittest.TestCase):\n"
                "    def test_retained_objects(self):\n"
                "        leaked.append(bytearray(64))\n")
            env = {k: v for k, v in os.environ.items() if k != "PYTHONPATH"
                   and not k.startswith(("CINDERX_", "PYTHONJIT", "SPIKE_"))}
            env.update(CINDERX_JIT_MODE="execute", PYTHONJITAUTO="1000000")
            proc = subprocess.run(
                [sys.executable, "-m", "test", "--testdir", directory,
                 "-R", "2:3", module], cwd=directory, env=env,
                capture_output=True, text=True, timeout=120)
            output = proc.stdout + proc.stderr
            self.assertEqual(proc.returncode, 2, output)
            self.assertRegex(output, r"leaked \[[-0-9, ]+\] references")
            self.assertIn("Result: FAILURE", output)

    def test_accounting_is_invisible_to_python_hooks_and_recursion(self):
        self.run_probe(OBSERVABILITY, 'trace profile and recursion preserved')


if __name__ == '__main__':
    unittest.main()
