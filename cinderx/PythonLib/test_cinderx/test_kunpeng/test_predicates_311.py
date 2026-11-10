# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Predicate fast paths, Python fallbacks, and artifact-owned cache lifetime."""

import os
import subprocess
import sys
import textwrap
import unittest


PREAMBLE = """
import abc
import gc
import weakref
import _cinderx, cinderx
cinderx.init()
_cinderx.install_frame_evaluator()
import cinderjit
assert cinderjit.is_enabled()

def compiled(fn):
    assert cinderjit.force_compile(fn)
    assert cinderjit.is_jit_compiled(fn)
    return fn
"""


@unittest.skipUnless(sys.version_info[:2] == (3, 11), "CPython 3.11 predicates")
class Predicates311Test(unittest.TestCase):
    def run_probe(self, source, **extra_env):
        env = {
            key: value
            for key, value in os.environ.items()
            if key != "PYTHONPATH"
            and not key.startswith(("PYTHONJIT", "CINDERX_", "PARALLEL_GC_"))
        }
        env.update(
            CINDERX_JIT_MODE="execute",
            PYTHONJITAUTO="1000000",
            PYTHONJITLIGHTWEIGHTFRAME="0",
        )
        env.update(extra_env)
        proc = subprocess.run(
            [sys.executable, "-c", PREAMBLE + textwrap.dedent(source)],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr[-6000:])

    def test_isinstance_dynamic_protocols_and_type_invalidation(self):
        self.run_probe("""
            @compiled
            def check(obj, cls):
                return isinstance(obj, cls)

            class Base: pass
            class Other: pass
            class Child(Base): pass
            obj = Child()
            for _ in range(20):
                assert check(obj, Base)
                assert not check(obj, Other)
                assert check(obj, (Other, Base))
            Child.__bases__ = (Other,)
            assert not check(obj, Base)
            assert check(obj, Other)
            assert check(obj, (Base, Other))

            class Proxy:
                @property
                def __class__(self):
                    return self.cls
            proxy = Proxy()
            proxy.cls = Base
            assert check(proxy, Base)
            proxy.cls = Other
            assert not check(proxy, Base)
            assert check(proxy, (Base, Other))

            class Abstract(metaclass=abc.ABCMeta): pass
            assert not check(obj, Abstract)
            Abstract.register(Other)
            assert check(obj, Abstract)

            class Meta(type): pass
            class Target(metaclass=Meta): pass
            assert not check(obj, Target)
            Meta.__instancecheck__ = lambda self, value: True
            assert check(obj, Target)
            assert check(obj, (Target,))

            assert check(1, (int, 42))  # tuple short circuit before invalid key
            try:
                check('x', (int, 42))
            except TypeError:
                pass
            else:
                raise AssertionError('invalid class accepted')
        """)

    def test_getattr_slots_descriptors_defaults_and_exception_edges(self):
        self.run_probe("""
            @compiled
            def get(obj, default):
                return getattr(obj, 'value', default)

            class Box:
                __slots__ = ('value',)
            box = Box()
            default = object()
            assert get(box, default) is default
            box.value = object()
            assert get(box, default) is box.value
            del box.value
            assert get(box, default) is default

            Box.value = property(lambda self: 'property')
            assert get(box, default) == 'property'
            calls = []
            def missing(self):
                calls.append('missing')
                raise AttributeError('hidden')
            Box.value = property(missing)
            assert get(box, default) is default
            assert calls == ['missing']
            def broken(self):
                calls.append('broken')
                raise ValueError('visible')
            Box.value = property(broken)

            @compiled
            def caught(obj, default):
                try:
                    return getattr(obj, 'value', default)
                except ValueError:
                    return 'caught'
            assert caught(box, default) == 'caught'
            assert calls == ['missing', 'broken']
        """)

    def test_string_literals_and_subclass_comparisons(self):
        self.run_probe("""
            def eq(value):
                if value == 'needle':
                    return 1
                return 0
            def ne(value):
                return value != 'needle'
            for _ in range(100):
                eq('needle')
                ne('needle')
            compiled(eq)
            compiled(ne)
            for value in ('needle', ''.join(['nee', 'dle']), '', 'need',
                          'needles', 'néedle', '😀eedle', 'nee\\x00le'):
                assert eq(value) == int(value == 'needle')
                assert ne(value) == (value != 'needle')
            class Custom(str):
                def __eq__(self, other): return True
                def __ne__(self, other): return False
            assert eq(Custom('different')) == 1
            assert ne(Custom('different')) is False
        """)

    def test_direct_int_calls_keep_large_integer_comparisons_in_jit(self):
        self.run_probe("""
            @compiled
            def masked(address, mask):
                packed = int(address)
                return (packed & int(mask)) != packed

            cases = (
                (0, 0),
                (1, 1),
                (2**32 - 1, 2**32 - 1),
                (2**100, 2**128 - 1),
                (2**100 + 1, 2**100),
                (-2**100, -1),
            )
            before = _cinderx._get_trigger_stats()
            for address, mask in cases:
                assert masked(address, mask) == ((address & mask) != address)
            after = _cinderx._get_trigger_stats()
            entries = after['machine_code_entries'] - before['machine_code_entries']
            assert entries == len(cases), (before, after)
            assert after['organic_deopt_hits'] == before['organic_deopt_hits'], (
                before, after)
        """)

    def test_concurrent_compilation_reclaims_cells_and_class_keys(self):
        self.run_probe("""
            def count():
                return cinderjit._jit311_lifecycle_snapshot()['jit']['predicate_caches']
            baseline = count()
            for cycle in range(2):
                units = []
                refs = []
                for i in range(128):
                    ns = {'__name__': 'predicate_batch'}
                    if i % 2:
                        source = 'def unit(obj, cls):\\n    return isinstance(obj, cls)\\n'
                    else:
                        source = "def unit(obj, default):\\n    return getattr(obj, 'value', default)\\n"
                    exec(compile(source, f'predicate_{cycle}_{i}.py', 'exec'), ns)
                    fn = ns['unit']
                    refs.append(weakref.ref(fn.__code__))
                    units.append(fn)
                    assert cinderjit._jit311_register_for_compile(fn)
                cinderjit._jit311_multithreaded_compile_test()
                assert all(cinderjit.is_jit_compiled(fn) for fn in units)
                assert count() > baseline
                class Key: pass
                keyref = weakref.ref(Key)
                obj = object()
                before = _cinderx._get_trigger_stats()['machine_code_entries']
                for i, fn in enumerate(units):
                    if i % 2:
                        assert fn(obj, (Key,)) is False
                    else:
                        assert fn(obj, Key) is Key
                assert _cinderx._get_trigger_stats()['machine_code_entries'] - before == len(units)
                del Key
                gc.collect()
                assert keyref() is None  # populated caches do not retain class keys
                for fn in units:
                    assert cinderjit.force_uncompile(fn)
                del fn, ns, units
                for _ in range(3):
                    gc.collect()
                assert all(ref() is None for ref in refs)
                assert count() == baseline, (count(), baseline)
        """, PYTHONJITMULTITHREADEDCOMPILETEST="1",
            PYTHONJITBATCHCOMPILEWORKERS="8")

    def test_active_artifact_keeps_cells_across_reentrant_retirement(self):
        self.run_probe("""
            def count():
                return cinderjit._jit311_lifecycle_snapshot()['jit']['predicate_caches']
            baseline = count()
            actions = []
            class Box:
                @property
                def value(self):
                    if actions:
                        actions.pop()()
                    return 42
            @compiled
            def target(obj, default):
                return (getattr(obj, 'value', default),
                        getattr(obj, 'value', default))
            live = count()
            assert live > baseline
            def retire():
                assert cinderjit.force_uncompile(target)
                gc.collect()
                assert count() == live  # interrupted native frame owns cells
            actions.append(retire)
            before = _cinderx._get_trigger_stats()['machine_code_entries']
            assert target(Box(), None) == (42, 42)
            assert _cinderx._get_trigger_stats()['machine_code_entries'] > before
            for _ in range(3):
                gc.collect()
            assert count() == baseline, (count(), baseline)
        """)
