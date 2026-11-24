# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import os.path
import re
import subprocess
import sys
import unittest

import cinderx

cinderx.init()

from cinderx.test_support import ENCODING, skip_unless_jit, subprocess_env


class PerfMapTests(unittest.TestCase):
    @skip_unless_jit("Runs a subprocess with the JIT enabled")
    @unittest.skipIf(sys.version_info[:2] == (3, 11), "Requires lazy_compile")
    def test_forked_pid_map(self) -> None:
        helper_file = os.path.join(
            os.path.dirname(__file__),
            "perf_fork_helper.py",
        )
        proc: subprocess.CompletedProcess[str] = subprocess.run(
            [
                sys.executable,
                "-X",
                "jit-perfmap",
                "-X",
                # Disable the inliner as it screws up this test's expectations.
                "jit-enable-hir-inliner=0",
                helper_file,
            ],
            stdout=subprocess.PIPE,
            encoding=ENCODING,
            env=subprocess_env(),
        )
        self.assertEqual(proc.returncode, 0)

        def find_mapped_funcs(which: str) -> set[str]:
            pattern = rf"{which}\(([0-9]+)\) computed "
            m = re.search(pattern, proc.stdout)
            self.assertIsNotNone(
                m, f"Couldn't find /{pattern}/ in stdout:\n\n{proc.stdout}"
            )
            pid = int(m[1])
            map_contents = ""
            try:
                with open(f"/tmp/perf-{pid}.map") as f:
                    map_contents = f.read()
            except FileNotFoundError:
                self.fail(f"{which} process (pid {pid}) did not generate a map")

            funcs = set(re.findall("__CINDER_JIT:__main__:(.+)", map_contents))
            return funcs

        self.assertEqual(find_mapped_funcs("parent"), {"main", "parent", "compute"})
        self.assertEqual(find_mapped_funcs("child1"), {"main", "child1", "compute"})
        self.assertEqual(find_mapped_funcs("child2"), {"main", "child2", "compute"})

    @unittest.skipUnless(
        sys.platform == "linux" and sys.version_info[:2] == (3, 11),
        "Exercises the Linux CPython 3.11 perf map writer",
    )
    @skip_unless_jit("Requires the CPython 3.11 execute mode")
    def test_forked_pid_map_cp311(self) -> None:
        helper_file = os.path.join(os.path.dirname(__file__), "perf_fork_helper.py")
        proc = subprocess.run(
            [sys.executable, "-X", "jit-perfmap", helper_file, "--cp311"],
            capture_output=True,
            encoding=ENCODING,
            env={
                **subprocess_env(),
                "CINDERX_PLUGIN_ENABLE": "1",
                "CINDERX_EVAL_MODE": "cinder",
                "CINDERX_JIT_MODE": "execute",
                "PYTHONJITAUTO": "10000",
            },
            timeout=30,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
