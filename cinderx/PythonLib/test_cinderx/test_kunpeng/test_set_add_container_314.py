# Copyright (c) Meta Platforms, Inc. and affiliates.
"""SET_ADD's interpreter type guard and consumed-value ownership."""

import os
import subprocess
import sys
import unittest


PROBE = r'''
import dis
import gc
import sys
import types
import weakref

import cinderx
cinderx.init()
cinderx.install_frame_evaluator()
assert cinderx.is_frame_evaluator_installed()
import cinderx.jit as jit

mode = sys.argv[1]
case = sys.argv[2]
protected = len(sys.argv) > 3

def shell(container, factory):
    pass

# Keep a valid stack and vary only the SET_ADD container. This avoids
# unrelated malformed-bytecode failures and lets the value die at SET_ADD.
ops = [
    ("RESUME", 0), ("LOAD_FAST", 0), ("LOAD_FAST", 1),
    ("PUSH_NULL", 0), ("CALL", 0),
]
bytecode = bytearray()
for name, arg in ops:
    bytecode.extend((dis.opmap[name], arg))
    bytecode.extend(b"\0\0" * dis._inline_cache_entries.get(name, 0))
bytecode.extend((dis.opmap["SET_ADD"], 1, dis.opmap["RETURN_VALUE"], 0))
fn = types.FunctionType(shell.__code__.replace(
    co_code=bytes(bytecode), co_stacksize=4, co_exceptiontable=b"",
), globals())
if protected:
    def protected_shell(container, factory):
        try:
            return {factory() for _ in (0,)}
        except (TypeError, ValueError):
            return "caught"
    code = bytearray(protected_shell.__code__.co_code)
    # Preserve every offset, stack effect, and compiler exception-table entry.
    # Substitute the empty set producer with the supplied container producer.
    producers = [i for i in dis.get_instructions(protected_shell)
                 if i.opname == "BUILD_SET" and i.arg == 0]
    assert len(producers) == 1
    code[producers[0].offset:producers[0].offset + 2] = bytes((dis.opmap["LOAD_FAST"], 0))
    fn = types.FunctionType(protected_shell.__code__.replace(co_code=bytes(code)), globals())
    assert fn.__code__.co_exceptiontable
assert any(i.opname == "SET_ADD" for i in dis.get_instructions(fn))
if mode == "interpreter":
    jit.jit_suppress(fn)
else:
    assert jit.force_compile(fn)
    assert jit.is_jit_compiled(fn)

events = []
refs = []
unraisable = []
sys.unraisablehook = lambda args: unraisable.append(str(args.exc_value))
class Value:
    def __hash__(self):
        events.append("hash")
        if case == "hash_error":
            raise ValueError("hash failed")
        return 123
    def __del__(self):
        events.append("del")
        # Reenter Python after the failing opcode has removed its value.
        assert {i for i in range(3)} == {0, 1, 2}

def factory():
    v = Value()
    refs.append(weakref.ref(v))
    return v

class SetSubclass(set):
    pass

if case == "normal" or case == "hash_error":
    container = set()
elif case == "subclass":
    container = SetSubclass()
else:
    container = {"list": [], "dict": {}, "frozenset": frozenset(),
                 "object": object()}[case]

if case == "normal" or (case == "subclass" and mode == "jit"):
    assert fn(container, factory) is container
    assert len(container) == 1
    assert events == ["hash"]
    assert refs[0]() is not None
    container.clear()
else:
    if protected:
        assert fn(container, factory) == "caught"
    else:
        try:
            fn(container, factory)
        except (TypeError, ValueError, SystemError) as exc:
            if case == "hash_error":
                assert type(exc) is ValueError and str(exc) == "hash failed"
            else:
                assert type(exc) is (TypeError if mode == "interpreter" else SystemError)
                if mode == "interpreter":
                    assert str(exc) == "'%s' object is not a set" % type(container).__name__
        else:
            raise AssertionError("SET_ADD accepted an invalid container")
gc.collect()
assert refs[0]() is None
assert events.count("del") == 1, events
assert not unraisable, unraisable
if case not in ("normal", "hash_error") and not (case == "subclass" and mode == "jit"):
    assert "hash" not in events, events
assert not jit.is_jit_compiled(fn) if mode == "interpreter" else jit.is_jit_compiled(fn)
# A fresh call after unwinding also checks that the interpreter remains usable.
assert {i for i in range(8)} == set(range(8))
print("SET_ADD", mode, case, events, "OK")
'''


@unittest.skipUnless(sys.version_info[:2] == (3, 14), "CPython 3.14 SET_ADD")
class SetAddContainer314Tests(unittest.TestCase):
    def run_probe(self, mode, case, protected=False):
        env = {
            k: v for k, v in os.environ.items()
            if not k.startswith(("PYTHONJIT", "CINDERX_", "PARALLEL_GC_"))
        }
        env.update(CINDERX_JIT_MODE="execute", PYTHONJITAUTO="auto:1000000")
        command = [sys.executable, "-c", PROBE, mode, case]
        if protected:
            command.append("protected")
        proc = subprocess.run(
            command,
            env=env, capture_output=True, text=True, timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("OK", proc.stdout)

    def test_interpreter_container_and_value_ownership(self):
        for case in ("normal", "hash_error", "list", "dict", "frozenset", "object", "subclass"):
            with self.subTest(case=case):
                self.run_probe("interpreter", case)

    def test_jit_public_set_api_boundary(self):
        # PySet_Add supports subclasses; the interpreter's private helper
        # requires exact sets. Keep that existing distinction explicit.
        for case in ("normal", "hash_error", "list", "dict", "object", "subclass"):
            with self.subTest(case=case):
                self.run_probe("jit", case)

    def test_interpreter_same_frame_handler(self):
        for case in ("hash_error", "list", "dict", "frozenset", "object", "subclass"):
            with self.subTest(case=case):
                self.run_probe("interpreter", case, protected=True)


if __name__ == "__main__":
    unittest.main()
