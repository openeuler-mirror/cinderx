# Copyright (c) Meta Platforms, Inc. and affiliates.

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD_ID_MODULE = ROOT / "cmake" / "CinderXBuildId.cmake"


def resolve_build_id(
    source_dir: Path, explicit: str = ""
) -> tuple[subprocess.CompletedProcess[str], str | None]:
    with tempfile.TemporaryDirectory() as temporary_dir:
        temporary_path = Path(temporary_dir)
        output_path = temporary_path / "build-id.txt"
        script_path = temporary_path / "resolve.cmake"
        script_path.write_text(
            f'include("{BUILD_ID_MODULE}")\n'
            "cinderx_resolve_core_build_id(\n"
            '  "${SOURCE_DIR}" "${EXPLICIT_BUILD_ID}" resolved_build_id)\n'
            f'file(WRITE "{output_path}" "${{resolved_build_id}}")\n'
        )
        result = subprocess.run(
            [
                "cmake",
                f"-DSOURCE_DIR={source_dir}",
                f"-DEXPLICIT_BUILD_ID={explicit}",
                "-P",
                str(script_path),
            ],
            text=True,
            capture_output=True,
        )
        resolved = output_path.read_text() if output_path.exists() else None
        return result, resolved


def test_source_manifest_includes_runtime_capability_inputs() -> None:
    manifest = (ROOT / "MANIFEST.in").read_text().splitlines()
    assert "include cmake/CinderXBuildId.cmake" in manifest
    assert "include cinderx/runtime_capabilities_build.h.in" in manifest


def test_explicit_build_id_is_validated() -> None:
    with tempfile.TemporaryDirectory() as source_dir:
        valid_result, valid_id = resolve_build_id(Path(source_dir), "release-1.2+linux")
        assert valid_result.returncode == 0, valid_result.stderr
        assert valid_id == "release-1.2+linux"

        invalid_result, _ = resolve_build_id(Path(source_dir), "release/1.2")
        assert invalid_result.returncode != 0
        assert "must be 1-128 characters" in invalid_result.stderr

        too_long_result, _ = resolve_build_id(Path(source_dir), "a" * 129)
        assert too_long_result.returncode != 0
        assert "must be 1-128 characters" in too_long_result.stderr


def test_sdist_version_is_used_without_git() -> None:
    with tempfile.TemporaryDirectory() as source_dir:
        source_path = Path(source_dir)
        (source_path / "PKG-INFO").write_text(
            "Metadata-Version: 2.4\nVersion: 1.2.3+src\n"
        )
        result, build_id = resolve_build_id(source_path)
        assert result.returncode == 0, result.stderr
        assert build_id == "sdist-1.2.3+src"


def test_enclosing_git_checkout_is_not_used_for_sdist() -> None:
    git = shutil.which("git")
    if git is None:
        raise unittest.SkipTest("Git is required to exercise enclosing-checkout rejection")
    top_level = subprocess.run(
        [git, "-C", str(ROOT), "rev-parse", "--show-toplevel"],
        text=True,
        capture_output=True,
    )
    if (
        top_level.returncode != 0
        or Path(top_level.stdout.strip()).resolve() != ROOT.resolve()
    ):
        raise unittest.SkipTest("Tests are not running from the root Git checkout")

    with tempfile.TemporaryDirectory(dir=ROOT) as source_dir:
        source_path = Path(source_dir)
        (source_path / "PKG-INFO").write_text("Version: 2.0.0\n")
        result, build_id = resolve_build_id(source_path)
        assert result.returncode == 0, result.stderr
        assert build_id == "sdist-2.0.0"


def test_missing_trustworthy_source_fails_closed() -> None:
    with tempfile.TemporaryDirectory() as source_dir:
        result, _ = resolve_build_id(Path(source_dir))
        assert result.returncode != 0
        assert "Cannot determine a trustworthy CINDERX_CORE_BUILD_ID" in result.stderr


if __name__ == "__main__":
    tests = (
        test_source_manifest_includes_runtime_capability_inputs,
        test_explicit_build_id_is_validated,
        test_sdist_version_is_used_without_git,
        test_enclosing_git_checkout_is_not_used_for_sdist,
        test_missing_trustworthy_source_fails_closed,
    )
    suite = unittest.TestSuite(unittest.FunctionTestCase(test) for test in tests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
