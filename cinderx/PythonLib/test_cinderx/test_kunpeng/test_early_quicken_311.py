# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Mode-isolated coverage for CPython 3.11 execute quickening policy."""

import os
import subprocess
import sys
import textwrap
import unittest


PROBE = textwrap.dedent(
    """
    import dis
    import cinderx

    cinderx.init()
    if MODE == "execute":
        import cinderjit

    class Box:
        def __init__(self):
            self.value = 1

    box = Box()

    def read(obj):
        return obj.value

    read(box)
    read(box)
    opnames = [instr.opname for instr in dis.get_instructions(read, adaptive=True)]

    if MODE == "execute":
        assert "LOAD_ATTR_INSTANCE_VALUE" in opnames, opnames
        assert cinderjit.is_jit_compiled(read), opnames
    else:
        assert "LOAD_ATTR_INSTANCE_VALUE" not in opnames, opnames
    """
)

FAILED_INIT_PROBE = textwrap.dedent(
    """
    import dis

    try:
        import _cinderx
    except RuntimeError as exc:
        assert "IMMORTALIZE" in str(exc), str(exc)
        pass
    else:
        raise AssertionError("execute initialization unexpectedly succeeded")

    class Box:
        def __init__(self):
            self.value = 1

    box = Box()

    def read(obj):
        return obj.value

    read(box)
    read(box)
    opnames = [instr.opname for instr in dis.get_instructions(read, adaptive=True)]
    assert "LOAD_ATTR_INSTANCE_VALUE" not in opnames, opnames
    """
)


@unittest.skipUnless(
    sys.version_info[:3] == (3, 11, 6),
    "EARLYQUICKEN targets CPython 3.11.6",
)
class EarlyQuicken311Test(unittest.TestCase):
    def run_probe(self, mode: str) -> None:
        env = {
            key: value
            for key, value in os.environ.items()
            if key != "PYTHONPATH"
            and not key.startswith(("PYTHONJIT", "CINDERX_", "PARALLEL_GC_"))
        }
        env.update(
            CINDERX_JIT_MODE=mode,
            PYTHONJITAUTO="2",
            PYTHONJITLIGHTWEIGHTFRAME="0",
        )
        source = f"MODE = {mode!r}\n" + PROBE
        proc = subprocess.run(
            [sys.executable, "-c", source],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])

    def test_execute_quickens_before_numeric_auto_jit(self) -> None:
        self.run_probe("execute")

    def test_shadow_keeps_stock_warmup_step(self) -> None:
        self.run_probe("shadow")

    def test_failed_execute_init_does_not_publish_warmup_step(self) -> None:
        env = {
            key: value
            for key, value in os.environ.items()
            if key != "PYTHONPATH"
            and not key.startswith(("PYTHONJIT", "CINDERX_", "PARALLEL_GC_"))
        }
        env.update(
            CINDERX_JIT_MODE="execute",
            PYTHONJITAUTO="2",
            PYTHONJITIMMORTALIZECOMPILEDFUNCTIONS="1",
            PYTHONJITLIGHTWEIGHTFRAME="0",
        )
        proc = subprocess.run(
            [sys.executable, "-c", FAILED_INIT_PROBE],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])


if __name__ == "__main__":
    unittest.main()
