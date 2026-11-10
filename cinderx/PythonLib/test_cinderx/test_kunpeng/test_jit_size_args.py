# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Malformed size arguments must fail the import, not crash the process.

``-X jit-max-code-size=<size>`` and ``PYTHONJITMAXCODESIZE`` take a byte count
with an optional ``k``/``m``/``g`` suffix.  A value that cannot be parsed used
to trip a ``JIT_CHECK`` while ``cinderx`` was initializing, which aborts the
whole interpreter with SIGABRT.  It must instead surface as a regular Python
exception from ``import cinderx`` that names the offending input.

The option is parsed once at startup, so every case runs in a fresh child
interpreter.  On CPython 3.11 the options are only parsed when the JIT mode is
requested through ``CINDERX_JIT_MODE``, so the child asks for ``shadow`` mode.
"""

import os
import subprocess
import sys
import unittest

from cinderx.test_support import ENCODING, subprocess_env


_SOURCE = """
import sys
import cinderx
import cinderx.jit as jit

print("IMPORTED")
if jit.is_enabled() and sys.version_info >= (3, 12):
    print("MAX_BYTES", jit.get_allocator_stats()["max_bytes"])
"""

_ENV_VAR = "PYTHONJITMAXCODESIZE"

# (value, text that must appear in the error message)
_BAD_VALUES = [
    ("", "Input string is empty"),
    ("   ", "Input string is empty"),
    ("abc", "Invalid character in input string: abc"),
    ("1X", "Invalid character in input string: 1X"),
    ("k", "Invalid unsigned integer in input string: 'k'"),
    ("-1", "Invalid unsigned integer in input string: '-1'"),
    ("1.5k", "Invalid unsigned integer in input string: '1.5k'"),
    (
        "99999999999999999999",
        "Invalid unsigned integer in input string: '99999999999999999999'",
    ),
    (
        "18446744073709551615K",
        "Unsigned Integer overflow in input string: '18446744073709551615K'",
    ),
    (
        "17179869184G",
        "Unsigned Integer overflow in input string: '17179869184G'",
    ),
]

# (value, expected max code size in bytes)
_GOOD_VALUES = [
    ("0", 0),
    ("1024", 1024),
    ("1k", 1024),
    ("1K", 1024),
    (" 1024 k", 1024 * 1024),
    ("2M", 2 * 1024 * 1024),
    ("1g", 1024 * 1024 * 1024),
]


def _run_child(
    *, xoption: str | None = None, env_value: str | None = None
) -> subprocess.CompletedProcess[str]:
    env = subprocess_env()
    for key, value in os.environ.items():
        # Do not let the parent's JIT configuration leak into the child.
        if key.startswith(("CINDERX_", "PYTHONJIT")):
            continue
        env.setdefault(key, value)
    if sys.version_info < (3, 12):
        env["CINDERX_JIT_MODE"] = "shadow"
    if env_value is not None:
        env[_ENV_VAR] = env_value

    args = [sys.executable]
    if xoption is not None:
        args.extend(["-X", f"jit-max-code-size={xoption}"])
    args.extend(["-c", _SOURCE])
    return subprocess.run(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        encoding=ENCODING,
        env=env,
        timeout=120,
    )


class SizeArgumentTest(unittest.TestCase):
    def _assert_rejected(
        self, proc: subprocess.CompletedProcess[str], text: str
    ) -> None:
        details = f"stdout={proc.stdout!r}\nstderr={proc.stderr!r}"
        # A negative return code means the child was killed by a signal
        # (SIGABRT from a failed JIT_CHECK); a clean failure is a positive one.
        self.assertGreater(proc.returncode, 0, details)
        self.assertNotIn("IMPORTED", proc.stdout, details)
        self.assertNotIn("Assertion failed", proc.stderr, details)
        self.assertIn("RuntimeError", proc.stderr, details)
        self.assertIn(text, proc.stderr, details)

    def test_bad_x_option_raises_runtime_error(self) -> None:
        for value, text in _BAD_VALUES:
            with self.subTest(value=value):
                self._assert_rejected(_run_child(xoption=value), text)

    def test_bad_environment_variable_raises_runtime_error(self) -> None:
        # An empty variable counts as unset, so skip the empty string here.
        for value, text in _BAD_VALUES:
            if value == "":
                continue
            with self.subTest(value=value):
                self._assert_rejected(_run_child(env_value=value), text)

    def test_good_values_still_parse(self) -> None:
        for value, expected in _GOOD_VALUES:
            with self.subTest(value=value):
                proc = _run_child(xoption=value)
                details = f"stdout={proc.stdout!r}\nstderr={proc.stderr!r}"
                self.assertEqual(proc.returncode, 0, details)
                self.assertIn("IMPORTED", proc.stdout, details)
                if sys.version_info >= (3, 12):
                    self.assertIn(f"MAX_BYTES {expected}", proc.stdout, details)


if __name__ == "__main__":
    unittest.main()
