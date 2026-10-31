# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Artifact ownership and concurrent compilation of generic subscript caches."""

import json
import os
import re
import subprocess
import sys
import textwrap
import unittest


PREAMBLE = """
import gc
import weakref
import _cinderx, cinderx
cinderx.init()
_cinderx.install_frame_evaluator()
import cinderjit
assert cinderjit.is_enabled()

def cache_count():
    return cinderjit._jit311_lifecycle_snapshot()['jit']['subscript_caches']

def collect():
    for _ in range(3):
        gc.collect()

baseline = cache_count()
"""

BATCH_PROBE = PREAMBLE + """
class Box:
    value = 42
    def __getitem__(self, key):
        return self.value
    def __setitem__(self, key, value):
        self.value = value

for cycle in range(2):
    units = []
    refs = []
    for i in range(2048):
        ns = {'__name__': 'subscript_batch'}
        if i % 2:
            source = 'def unit(obj, key, value):\\n    obj[key] = value\\n'
        else:
            source = 'def unit(obj, key):\\n    return obj[key]\\n'
        exec(compile(source, f'unit_{cycle}_{i}.py', 'exec'), ns)
        fn = ns['unit']
        refs.append(weakref.ref(fn.__code__))
        units.append(fn)
        assert cinderjit._jit311_register_for_compile(fn)
    cinderjit._jit311_multithreaded_compile_test()
    assert all(cinderjit.is_jit_compiled(fn) for fn in units)
    assert cache_count() == baseline + len(units), cache_count()
    box = Box()
    before = _cinderx._get_trigger_stats()['machine_code_entries']
    for i, fn in enumerate(units):
        if i % 2:
            assert fn(box, 'key', i) is None
            assert box.value == i
        else:
            assert fn(box, 'key') == box.value
    assert _cinderx._get_trigger_stats()['machine_code_entries'] - before == 2048
    for fn in units:
        assert cinderjit.force_uncompile(fn)
    del fn, ns, units
    collect()
    assert not any(ref() is not None for ref in refs)
    assert cache_count() == baseline, cache_count()
print('batch caches reclaimed')
"""

LATIN1_FIRST_COMPILE_PROBE = r"""
import array
import dis
import gc
import json
import sys
import _cinderx, cinderx
cinderx.init()
_cinderx.install_frame_evaluator()
import cinderjit
assert cinderjit.is_enabled()
assert cinderjit._jit311_multithreaded_compile_test_enabled()

def array_load(obj):
    return obj[0]

# Compile cold: the integer literal primes only the array type cache. Warming
# this function first would also request STRCHR and hide first-use races.
assert cinderjit.force_compile(array_load)
assert array_load(array.array('d', [3.5])) == 3.5
assert cinderjit.force_uncompile(array_load)
characters = tuple(map(chr, range(256)))
units = []
for index in range(256):
    namespace = {'__name__': 'latin1_batch'}
    exec(compile('def unit(obj): return obj[0]', f'latin1_{index}.py', 'exec'), namespace)
    func = namespace['unit']
    for _ in range(300):
        assert func('Alpha') == 'A'
    assert not cinderjit.is_jit_compiled(func)
    assert 'BINARY_SUBSCR_ADAPTIVE' in [i.opname for i in dis.get_instructions(func, adaptive=True)]
    units.append(func)
    assert cinderjit._jit311_register_for_compile(func)
gc.collect()
before = tuple(map(sys.getrefcount, characters))
cinderjit._jit311_multithreaded_compile_test()
assert all(cinderjit.is_jit_compiled(func) for func in units)
after = tuple(map(sys.getrefcount, characters))
delta = [a - b for a, b in zip(after, before)]
assert delta == [1] * 256, delta
entries = _cinderx._get_trigger_stats()['machine_code_entries']
for func in units:
    for value in ('Alpha', '\x00tail', '\x7ftail', '\u00e9tail', '\u03bbtail', '\U0001f642tail'):
        answer = func(value)
        assert answer == value[0] and type(answer) is str
    del answer
assert _cinderx._get_trigger_stats()['machine_code_entries'] - entries == len(units) * 6
for func in units:
    assert cinderjit.force_uncompile(func)
gc.collect()
assert tuple(map(sys.getrefcount, characters)) == after
print('latin1 first compile ' + json.dumps(
    {'units': len(units), 'refcount_delta': delta, 'entries': len(units) * 6}))
"""

FAILURE_PROBE = PREAMBLE + """
for i in range(32):
    ns = {}
    exec(compile('def target(obj, key): return obj[key]', f'failed_{i}.py', 'exec'), ns)
    fn = ns['target']
    ref = weakref.ref(fn.__code__)
    result = cinderjit._jit311_compile_diagnostic(fn)
    assert result['reason'] == 'REFUSE_SHAPE_LIR_SIZE_LIMIT', result
    assert result['compiled'] is False
    assert cache_count() == baseline, cache_count()
    del fn, ns
    collect()
    assert ref() is None

units = []
refs = []
for i in range(64):
    ns = {}
    exec(compile('def target(obj, key): return obj[key]', f'batch_fail_{i}.py', 'exec'), ns)
    fn = ns['target']
    refs.append(weakref.ref(fn.__code__))
    units.append(fn)
    assert cinderjit._jit311_register_for_compile(fn)
cinderjit._jit311_multithreaded_compile_test()
assert not any(cinderjit.is_jit_compiled(fn) for fn in units)
assert cache_count() == baseline, cache_count()
del fn, ns, units
collect()
assert not any(ref() is not None for ref in refs)
assert cache_count() == baseline
print('failed compiles reclaimed')
"""

PUBLICATION_PROBE = PREAMBLE + """
for step in range(1, 8):
    ns = {}
    exec(compile('def target(obj, key): return obj[key]', f'publish_{step}.py', 'exec'), ns)
    fn = ns['target']
    ref = weakref.ref(fn.__code__)
    assert cinderjit._jit311_compile_with_publish_failure(fn, step)
    assert not cinderjit.is_jit_compiled(fn)
    assert cache_count() == baseline, (step, cache_count())
    del fn, ns
    collect()
    assert ref() is None
    assert cache_count() == baseline
print('failed publications reclaimed')
"""

RETIREMENT_PROBE = PREAMBLE + """
import os
mode = os.environ['SUBSCRIPT_RETIRE_MODE']
actions = []
class Box:
    def __getitem__(self, key):
        if actions:
            actions.pop()()
        return 42

def target(obj, key):
    return obj[key], obj[key]

assert cinderjit.force_compile(target)
assert cache_count() == baseline + 2
assert target(Box(), 'key') == (42, 42)
def retire():
    if mode in ('uncompile', 'recompile'):
        assert cinderjit.force_uncompile(target)
    elif mode == 'dictionary':
        target.__dict__.clear()
    elif mode == 'disable':
        cinderjit.disable()
    else:
        _cinderx.remove_frame_evaluator()
    collect()
    # The interrupted invocation can still read either cache cell.
    assert cache_count() == baseline + 2, (mode, cache_count())
    if mode == 'recompile':
        assert cinderjit.force_compile(target)
        assert cache_count() == baseline + 4

actions.append(retire)
before = _cinderx._get_trigger_stats()['machine_code_entries']
assert target(Box(), 'key') == (42, 42)
assert _cinderx._get_trigger_stats()['machine_code_entries'] > before
cinderjit.enable()
_cinderx.install_frame_evaluator()
cinderjit.force_uncompile(target)
collect()
assert cache_count() == baseline, (mode, cache_count())
print('active caches survived retirement')
"""

DESCRIPTOR_PROBE = PREAMBLE + """
actions = []
def dispatch(self, key):
    if actions:
        actions.pop()()
    return 41

class Box:
    def __getitem__(self, key):
        return dispatch(self, key)

def target(obj, key):
    return obj[key], obj[key]

old_method = weakref.ref(Box.__getitem__)
assert cinderjit.force_compile(Box.__getitem__)
assert cinderjit.force_compile(target)
assert cache_count() == baseline + 2
assert target(Box(), 'key') == (41, 41)
def replace():
    fn = Box.__getitem__
    Box.__getitem__ = lambda self, key: 42
    assert cinderjit.force_uncompile(fn)
    del fn
    collect()
    # The cache's temporary call owner protects an active no-ref JIT frame.
    assert old_method() is not None

actions.append(replace)
assert target(Box(), 'key') == (41, 42)
collect()
assert old_method() is None
assert target(Box(), 'key') == (42, 42)
assert cinderjit.force_uncompile(target)
collect()
assert cache_count() == baseline
print('descriptor replacement safe')
"""

GENERATOR_PROBE = PREAMBLE + """
class Box:
    def __getitem__(self, key):
        return 42

def target(obj, key):
    yield obj[key]
    yield obj[key]

assert cinderjit.force_compile(target)
assert cache_count() == baseline + 2
gen = target(Box(), 'key')
assert next(gen) == 42
assert cinderjit.force_uncompile(target)
collect()
assert cache_count() == baseline + 2
assert next(gen) == 42
gen.close()
del gen
collect()
assert cache_count() == baseline
print('suspended generator reclaimed')
"""


@unittest.skipUnless(sys.version_info[:2] == (3, 11), "CP311 subscript caches")
class SubscriptCacheLifetime311Tests(unittest.TestCase):
    def _run(self, source, marker, **extra):
        env = {
            key: value for key, value in os.environ.items()
            if key != "PYTHONPATH"
            and not key.startswith(("CINDERX_", "PYTHONJIT", "SPIKE_"))
        }
        env.update(CINDERX_PLUGIN_ENABLE="0", CINDERX_JIT_MODE="execute",
                   PYTHONJITAUTO="1000000", **extra)
        proc = subprocess.run([sys.executable, "-c", textwrap.dedent(source)],
                              env=env, text=True, capture_output=True, timeout=180)
        self.assertEqual(proc.returncode, 0, proc.stdout[-1000:] + proc.stderr[-3000:])
        self.assertIn(marker, proc.stdout)
        return proc

    def test_parallel_cache_allocation_and_collection(self):
        for workers in (1, 8, 8, 8):
            with self.subTest(workers=workers):
                proc = self._run(BATCH_PROBE, "batch caches reclaimed",
                                 PYTHONJITMULTITHREADEDCOMPILETEST="1",
                                 PYTHONJITBATCHCOMPILEWORKERS=str(workers))
                if workers == 8:
                    self.assertIn("compiles attempted: 2048", proc.stderr)

    def test_first_parallel_latin1_table_initialization(self):
        serial = None
        for workers in (1, 8, 8, 8):
            with self.subTest(workers=workers):
                proc = self._run(
                    LATIN1_FIRST_COMPILE_PROBE, "latin1 first compile ",
                    PYTHONJITMULTITHREADEDCOMPILETEST="1",
                    PYTHONJITBATCHCOMPILEWORKERS=str(workers),
                    PYTHONJITDUMPFINALHIR="1")
                report = json.loads(proc.stdout.split("latin1 first compile ", 1)[1])
                if serial is None:
                    serial = report
                else:
                    self.assertEqual(report, serial)
                sections = re.split(
                    r"JIT: [^\n]* -- Optimized HIR for ", proc.stderr)[1:]
                array_hir = [s for s in sections if s.startswith("__main__:array_load:")]
                self.assertEqual(len(array_hir), 1)
                self.assertNotRegex(array_hir[0], r"UnicodeExact = LoadArrayItem")
                units = [s for s in sections if s.startswith("latin1_batch:unit:")]
                self.assertEqual(len(units), 256)
                tables = set()
                for hir in units:
                    self.assertRegex(hir, r"CUInt8 = LoadArrayItem")
                    self.assertRegex(hir, r"UnicodeExact = LoadArrayItem")
                    tables.update(re.findall(r"LoadConst<CPtr\[(0x[0-9a-f]+)\]>", hir))
                self.assertEqual(len(tables), 1)
                if workers == 8:
                    self.assertIn("compiles attempted: 256, compiles retried: 0", proc.stderr)

    def test_failed_compilation_reclaims_caches(self):
        for workers in (1, 8):
            with self.subTest(workers=workers):
                self._run(FAILURE_PROBE, "failed compiles reclaimed",
                          PYTHONJITMAXLIRINSTRS="1",
                          PYTHONJITMULTITHREADEDCOMPILETEST="1",
                          PYTHONJITBATCHCOMPILEWORKERS=str(workers))

    def test_failed_publication_reclaims_caches(self):
        self._run(PUBLICATION_PROBE, "failed publications reclaimed")

    def test_active_retirement_keeps_caches_alive(self):
        for mode in ("uncompile", "recompile", "dictionary", "disable", "evaluator"):
            with self.subTest(mode=mode):
                self._run(RETIREMENT_PROBE, "active caches survived retirement",
                          SUBSCRIPT_RETIRE_MODE=mode)

    def test_descriptor_replacement_keeps_callable_alive(self):
        self._run(DESCRIPTOR_PROBE, "descriptor replacement safe")

    def test_suspended_generator_retains_its_caches(self):
        self._run(GENERATOR_PROBE, "suspended generator reclaimed")


if __name__ == "__main__":
    unittest.main()
