#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Compare PGO file changes while pip installs a prepared local wheel.

Run this only in a disposable instrumented build directory.  The program does
not delete or reset profile files.  It first runs pip with CinderX loading
disabled, then repeats the same offline installation after processing the
current build's ``cinderx.pth``.  The JSON report records the loaded extension
path and which profile files changed in each case.  These files are diagnostic
evidence and must not be used by the final PGO build.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


PROFILE_SUFFIXES = (".gcda", ".profraw", ".profdata")


def snapshot(root: Path) -> dict[str, str]:
    result = {}
    for path in root.rglob("*"):
        if path.is_file() and path.name.endswith(PROFILE_SUFFIXES):
            result[str(path.relative_to(root))] = hashlib.sha256(
                path.read_bytes()
            ).hexdigest()
    return result


def changed(before: dict[str, str], after: dict[str, str]) -> list[str]:
    return sorted(
        name
        for name in set(before) | set(after)
        if before.get(name) != after.get(name)
    )


def pip_arguments(wheel: Path, target: Path) -> list[str]:
    return [
        "install",
        "--no-index",
        "--no-deps",
        "--target",
        str(target),
        str(wheel),
    ]


def run_probe(python: str, build_lib: Path, wheel: Path, profile_root: Path) -> dict:
    environment = os.environ.copy()
    for name in ("PYTHONHOME", "PYTHONPATH", "PYTHONUSERBASE"):
        environment.pop(name, None)
    environment.update(
        {
            "CINDERX_PLUGIN_ENABLE": "0",
            "PYTHONNOUSERSITE": "1",
            "PYTHONHASHSEED": "0",
            "LLVM_PROFILE_FILE": str(profile_root / "probe-%p-%m.profraw"),
        }
    )

    report = {}
    with tempfile.TemporaryDirectory(prefix="cinderx-pgo-pip-") as directory:
        temporary = Path(directory)
        before = snapshot(profile_root)
        clean = subprocess.run(
            [python, "-m", "pip", *pip_arguments(wheel, temporary / "clean")],
            check=True,
            capture_output=True,
            text=True,
            env=environment,
        )
        after_clean = snapshot(profile_root)
        report["loading_disabled"] = {
            "profile_files_changed": changed(before, after_clean),
            "stdout": clean.stdout[-2000:],
        }

        code = """
import runpy, site, sys
site.addsitedir(sys.argv.pop(1))
import _cinderx
print(f"CINDERX_EXTENSION={_cinderx.__file__}")
sys.argv[0] = "pip"
runpy.run_module("pip", run_name="__main__")
"""
        with_loading = dict(environment)
        with_loading["CINDERX_PLUGIN_ENABLE"] = "1"
        loaded = subprocess.run(
            [
                python,
                "-c",
                code,
                str(build_lib),
                *pip_arguments(wheel, temporary / "loaded"),
            ],
            check=True,
            capture_output=True,
            text=True,
            env=with_loading,
        )
        after_loaded = snapshot(profile_root)
        extension_lines = [
            line
            for line in loaded.stdout.splitlines()
            if line.startswith("CINDERX_EXTENSION=")
        ]
        report["loading_enabled"] = {
            "extension": extension_lines[-1].partition("=")[2]
            if extension_lines
            else None,
            "profile_files_changed": changed(after_clean, after_loaded),
            "stdout": loaded.stdout[-2000:],
        }
    return report


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--build-lib", type=Path, required=True)
    parser.add_argument("--wheel", type=Path, required=True)
    parser.add_argument("--profile-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    report = run_probe(
        arguments.python,
        arguments.build_lib.resolve(),
        arguments.wheel.resolve(),
        arguments.profile_root.resolve(),
    )
    arguments.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(arguments.output)


if __name__ == "__main__":
    main()
