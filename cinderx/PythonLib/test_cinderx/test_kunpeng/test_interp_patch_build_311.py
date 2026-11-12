# Copyright (c) Meta Platforms, Inc. and affiliates.
"""An edit to any interpreter patch must regenerate an incremental build."""

import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest


@unittest.skipUnless(sys.platform.startswith("linux"), "requires Linux build tools")
class InterpreterPatchBuild311Tests(unittest.TestCase):
    def test_patch_dependencies(self):
        if any(shutil.which(tool) is None for tool in ("cmake", "make", "git")):
            self.skipTest("requires CMake, Make and Git")
        repo = Path(__file__).resolve().parents[4]
        upstream = repo / "cinderx/Interpreter/3.11"
        if not (upstream / "upstream/ceval.c").is_file():
            self.skipTest("requires the source checkout")

        def run(*command):
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        def digest(build):
            return [hashlib.sha256((build / "ci311-attr" / name).read_bytes()).hexdigest()
                    for name in ("ceval.c", "specialize.c")]

        with tempfile.TemporaryDirectory(prefix="interp311-patch-build-") as directory:
            root = Path(directory)
            source = root / "project"
            target = source / "cinderx/Interpreter/3.11"
            shutil.copytree(upstream / "upstream", target / "upstream")
            shutil.copytree(upstream / "attr-patches", target / "attr-patches")
            (source / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.18)
project(attr_patch_dependencies NONE)
set(PROJECT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cinderx")
include("${PROJECT_SOURCE_DIR}/Interpreter/3.11/attr-patches/apply.cmake")
add_custom_target(patched ALL DEPENDS
 "${CMAKE_CURRENT_BINARY_DIR}/ci311-attr/ceval.c"
 "${CMAKE_CURRENT_BINARY_DIR}/ci311-attr/specialize.c")
''', encoding="utf-8")
            build = root / "incremental"
            configure = ("cmake", "-G", "Unix Makefiles", "-S", str(source), "-B")
            run(*configure, str(build))
            run("cmake", "--build", str(build))
            patches = sorted((target / "attr-patches").glob("*.patch"))
            self.assertTrue(patches)
            for patch in patches:
                with self.subTest(patch=patch.name):
                    before = digest(build)
                    # Some generators compare whole-second timestamps.
                    time.sleep(1.1)
                    lines = patch.read_text(encoding="utf-8").splitlines()
                    index = next(i for i, line in enumerate(lines)
                                 if line.startswith("+ ") and not line[1:].strip().startswith(("/*", "*")))
                    lines[index] += " /* dependency-test */"
                    patch.write_text("\n".join(lines) + "\n", encoding="utf-8")
                    run("cmake", "--build", str(build))
                    after = digest(build)
                    self.assertNotEqual(after, before)
                    clean = root / ("clean-" + patch.stem)
                    run(*configure, str(clean))
                    self.assertEqual(digest(clean), after)
