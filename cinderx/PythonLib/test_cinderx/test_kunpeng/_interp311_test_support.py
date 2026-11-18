# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Isolate CPython 3.11 evaluator configuration in subprocesses."""

import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import sysconfig
import tempfile
import unittest


FIXTURES = Path(__file__).with_name("fixtures") / "attr_path"
SUPPORTED = sys.platform.startswith("linux") and sys.version_info[:3] == (3, 11, 6)


@unittest.skipUnless(SUPPORTED, "requires Linux and CPython 3.11.6")
class Interpreter311Case(unittest.TestCase):
    probes = ()

    @classmethod
    def setUpClass(cls):
        if importlib.util.find_spec("_cinderx") is None:
            raise unittest.SkipTest("requires an installed CinderX wheel")
        directory = tempfile.TemporaryDirectory(prefix="interp311-tests-")
        cls.addClassCleanup(directory.cleanup)
        cls.work = Path(directory.name)
        if not cls.probes:
            return
        compiler = shlex.split(os.environ.get("CC") or sysconfig.get_config_var("CC") or "cc")
        if not compiler or shutil.which(compiler[0]) is None:
            raise unittest.SkipTest("requires a C compiler for test probes")
        includes = sorted({sysconfig.get_path("include"), sysconfig.get_path("platinclude")})
        suffix = sysconfig.get_config_var("EXT_SUFFIX")
        for stem in cls.probes:
            command = [*compiler, "-shared", "-fPIC", "-O2"]
            command.extend("-I" + path for path in includes)
            command.extend((str(FIXTURES / f"{stem}_probe.c"), "-o", str(cls.work / f"_{stem}_probe{suffix}")))
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            if result.returncode:
                raise RuntimeError("test probe compilation failed:\n" + result.stdout + result.stderr)

    def run_case(self, case, mode="cinderx", args=(), *, output=False, **overrides):
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("ATTR_PATH_", "CINDERX_", "PYTHONJIT", "AUTO_JIT", "PARALLEL_GC_"))}
        env.update(ATTR_PATH_DIAGNOSTICS="1", CINDERX_PLUGIN_ENABLE="0",
                   CINDERX_JIT_MODE="off", CINDERX_JIT_DISABLE="1", PYTHONJITDISABLE="1",
                   PYTHONHASHSEED="0", PYTHONNOUSERSITE="1", PYTHONDONTWRITEBYTECODE="1")
        if mode is not None:
            env["ATTR_PATH_MODE"] = mode
        env.update(overrides)
        env["PYTHONPATH"] = os.pathsep.join(
            path for path in (str(self.work), env.get("PYTHONPATH", "")) if path
        )
        with tempfile.TemporaryDirectory(dir=self.work) as directory:
            result_path = Path(directory) / "result.json"
            if output:
                env["ATTR_PATH_TEST_OUTPUT"] = str(result_path)
            result = subprocess.run(
                [sys.executable, str(FIXTURES / case), *args],
                cwd=directory, env=env, capture_output=True, text=True, timeout=180,
            )
            self.assertEqual(result.returncode, 0, result.stdout[-4000:] + result.stderr[-4000:])
            if output:
                return json.loads(result_path.read_text(encoding="utf-8"))
