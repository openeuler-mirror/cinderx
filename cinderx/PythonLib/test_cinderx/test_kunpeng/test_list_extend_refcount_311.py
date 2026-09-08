# Copyright (c) Meta Platforms, Inc. and affiliates.
"""CPython 3.11 LIST_EXTEND JIT reference ownership regression."""

import os
import subprocess
import sys
import textwrap
import unittest


PROBE = textwrap.dedent(
    """
    import sys

    import _cinderx
    import cinderx

    cinderx.init()
    _cinderx.install_frame_evaluator()
    import cinderjit

    def extend(iterable):
        return [1, *iterable]

    assert cinderjit.force_compile(extend) is True
    assert cinderjit.is_jit_compiled(extend)

    before = sys.getrefcount(None)
    for _index in range(1000):
        result = extend((2, 3))
        assert result == [1, 2, 3]
        del result
    after = sys.getrefcount(None)
    assert after == before, (before, after)

    try:
        extend(1)
    except TypeError as exc:
        assert str(exc) == "Value after * must be an iterable, not int"
    else:
        raise AssertionError("LIST_EXTEND accepted a non-iterable")
    """
)


@unittest.skipUnless(
    sys.version_info[:3] == (3, 11, 6),
    "the LIST_EXTEND execute regression targets vendored CPython 3.11.6",
)
class ListExtendRefcount311Test(unittest.TestCase):
    def test_execute_does_not_leak_none_references(self) -> None:
        env = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith(("PYTHONJIT", "CINDERX_", "PARALLEL_GC_"))
        }
        env.update(
            CINDERX_JIT_MODE="execute",
            PYTHONJITAUTO="1000000",
        )
        proc = subprocess.run(
            [sys.executable, "-c", PROBE],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])


if __name__ == "__main__":
    unittest.main()
