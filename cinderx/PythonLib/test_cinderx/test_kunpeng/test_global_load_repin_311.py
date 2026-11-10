# Copyright (c) Meta Platforms, Inc. and affiliates.

"""CPython 3.11 LOAD_GLOBAL through the per-site cell: a key added to or
removed from the module or builtins dict re-pins the site instead of
deoptimizing it, a same-key rebinding is read live, and the answers stay
the interpreter's throughout."""

import os
import subprocess
import sys
import textwrap
import unittest


PREAMBLE = """
import _cinderx
import cinderx
import sys

cinderx.init()
_cinderx.install_frame_evaluator()
import cinderjit

def trigger_stats():
    s = _cinderx._get_trigger_stats()
    return s["machine_code_entries"], s["organic_deopt_hits"]
"""


# A new module global after the compile: the builtin-form and module-form
# loads keep their machine code, nothing deopts, every call enters.
NEW_KEY_PROBE = PREAMBLE + """
limit = 3
def uses(x):
    return len(x) + limit

data = [1, 2, 3]
assert cinderjit.force_compile(uses) is True
entries, deopts = trigger_stats()
for i in range(200):
    globals()["fresh_key_%d" % i] = i
    assert uses(data) == 6
e2, d2 = trigger_stats()
assert e2 - entries == 200, (e2 - entries)
assert d2 == deopts, (d2 - deopts)
assert cinderjit.is_jit_compiled(uses)
print("new key ok")
"""


# A module global shadows a builtin after the compile, then stops shadowing
# it, and the builtins dict itself gains and loses a key: the site answers
# like the interpreter at every step and re-pins without deopts.  (len and
# isinstance are additionally pinned by identity for their inline forms;
# shadowing one of those still deopts once, by design, which the second
# half checks.)
SHADOW_PROBE = PREAMBLE + """
import builtins

def largest(x):
    return max(x)

def length(x):
    return len(x)

data = [1, 2, 3]
assert cinderjit.force_compile(largest) is True
assert cinderjit.force_compile(length) is True
entries, deopts = trigger_stats()
assert largest(data) == 3
globals()["max"] = lambda x: 42
assert largest(data) == 42, largest(data)
del globals()["max"]
assert largest(data) == 3
builtins.brand_new_builtin = 7
assert largest(data) == 3
del builtins.brand_new_builtin
assert largest(data) == 3
e2, d2 = trigger_stats()
assert e2 - entries == 5, (e2 - entries)
assert d2 == deopts, (d2 - deopts)
assert cinderjit.is_jit_compiled(largest)
# The identity-pinned builtin: shadowing deopts the pin exactly once and
# the answer is still the interpreter's.
entries, deopts = trigger_stats()
globals()["len"] = lambda x: 42
assert length(data) == 42, length(data)
del globals()["len"]
e2, d2 = trigger_stats()
assert d2 - deopts == 1, (d2 - deopts)
assert length(data) == 3
print("shadow ok")
"""


# A same-key rebinding of a module global is read live; a deleted module
# global raises NameError from the compiled frame like the interpreter, and
# re-binding it later brings the site back.
REBIND_PROBE = PREAMBLE + """
value = 1
def read():
    return value

assert cinderjit.force_compile(read) is True
entries, deopts = trigger_stats()
assert read() == 1
globals()["value"] = 2
assert read() == 2
e2, d2 = trigger_stats()
assert e2 - entries == 2, (e2 - entries)
assert d2 == deopts, (d2 - deopts)
del globals()["value"]
try:
    read()
except NameError as exc:
    assert "value" in str(exc), exc
else:
    raise AssertionError("deleted global did not raise")
# (On 3.11 an exception raised inside machine code is delegated to the
# interpreter as a deopt; that one is the NameError's, not the load's.)
globals()["value"] = 3
entries, deopts = trigger_stats()
for _ in range(50):
    assert read() == 3
e2, d2 = trigger_stats()
assert e2 - entries == 50, (e2 - entries)
assert d2 == deopts, (d2 - deopts)
assert cinderjit.is_jit_compiled(read)
print("rebind ok")
"""


# A name resolved in the module dict at compile time has a site that checks
# the globals keys version only.  Deleting the global and supplying the
# name from builtins must still answer like the interpreter (the site runs
# unpinned until the global returns), with no deopt; a module global later
# shadowing it is seen through the globals version.
GLOBALS_ONLY_PROBE = PREAMBLE + """
import builtins

def helper():
    return "module"

def call():
    return helper()

assert cinderjit.force_compile(call) is True
entries, deopts = trigger_stats()
assert call() == "module"
del globals()["helper"]
builtins.helper = lambda: "builtin"
for _ in range(20):
    assert call() == "builtin", call()
globals()["helper"] = lambda: "module again"
assert call() == "module again", call()
del builtins.helper
assert call() == "module again"
e2, d2 = trigger_stats()
assert e2 - entries == 23, (e2 - entries)
assert d2 == deopts, (d2 - deopts)
assert cinderjit.is_jit_compiled(call)
print("globals only ok")
"""


# Growing the module past several key-table resizes keeps the pinned slot
# honest (the versions move, the slot is re-taken).
RESIZE_PROBE = PREAMBLE + """
anchor = 10
def read():
    return anchor

assert cinderjit.force_compile(read) is True
entries, deopts = trigger_stats()
for i in range(5000):
    globals()["grow_%d" % i] = i
    if i % 500 == 0:
        globals()["anchor"] = 10 + i
        assert read() == 10 + i, (i, read())
assert read() == 10 + 4500
e2, d2 = trigger_stats()
assert d2 == deopts, (d2 - deopts)
assert cinderjit.is_jit_compiled(read)
print("resize ok")
"""


@unittest.skipUnless(
    sys.version_info[:2] == (3, 11), "CPython 3.11 LOAD_GLOBAL cell"
)
class GlobalLoadRepin311Test(unittest.TestCase):
    def _run(self, probe, marker):
        env = {
            key: value
            for key, value in os.environ.items()
            if key in ("PATH", "HOME", "LD_LIBRARY_PATH", "PYTHONPATH")
        }
        env.update(
            CINDERX_PLUGIN_ENABLE="1",
            CINDERX_EVAL_MODE="cinder",
            CINDERX_JIT_MODE="canary",
            PYTHONJITAUTO="1000000",
            PYTHONJITLIGHTWEIGHTFRAME="0",
        )
        proc = subprocess.run(
            [sys.executable, "-c", textwrap.dedent(probe)],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        self.assertIn(marker, proc.stdout)

    def test_new_module_global_keeps_machine_code(self):
        self._run(NEW_KEY_PROBE, "new key ok")

    def test_shadowing_and_unshadowing_a_builtin(self):
        self._run(SHADOW_PROBE, "shadow ok")

    def test_rebinding_and_deleting_a_module_global(self):
        self._run(REBIND_PROBE, "rebind ok")

    def test_module_name_supplied_from_builtins_after_deletion(self):
        self._run(GLOBALS_ONLY_PROBE, "globals only ok")

    def test_key_table_resizes_retake_the_slot(self):
        self._run(RESIZE_PROBE, "resize ok")


if __name__ == "__main__":
    unittest.main()
