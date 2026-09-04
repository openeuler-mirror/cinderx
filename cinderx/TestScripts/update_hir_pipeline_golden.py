#!/usr/bin/env python3
"""Regenerate the HIR pipeline golden files (INFRA-1).

Runs the runtime_tests binary with UPDATE_HIR_PIPELINE_GOLDEN=1 so every
HIRPipelineGolden test case rewrites its golden instead of comparing
against it.  The trace is captured twice per case and asserted equal
before the golden is written, so regenerated baselines are deterministic
by construction.

Goldens are fingerprints (one line per pass: name, occurrence, FNV-1a of
the post-pass HIR).  To diagnose what changed behind a fingerprint, rerun
the failing case with HIR_PIPELINE_FULL_DUMP=1 — the full per-pass HIR of
the actual trace is printed to stdout and is never committed.

Usage:
    python3 cinderx/TestScripts/update_hir_pipeline_golden.py \
        build/rt-golden/runtime_tests [--gtest_filter=HIRPipelineGolden.*]

Review the resulting diff (git diff -- cinderx/RuntimeTests/hir_pipeline_golden)
before committing: an intentional trace change must be justified by the
code change that caused it, not by regenerating until the gate is green.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "runtime_tests",
        help="Path to the built runtime_tests binary",
    )
    parser.add_argument(
        "gtest_args",
        nargs=argparse.REMAINDER,
        help="Extra gtest arguments (default: only the golden suite)",
    )
    args = parser.parse_args()
    if not args.gtest_args:
        args.gtest_args = ["--gtest_filter=HIRPipelineGolden.*"]
    return args


def main() -> int:
    args = parse_args()
    if not os.path.isfile(args.runtime_tests) or not os.access(
        args.runtime_tests, os.X_OK
    ):
        print(f"error: {args.runtime_tests} is not an executable file", file=sys.stderr)
        return 2

    env = dict(os.environ)
    env["UPDATE_HIR_PIPELINE_GOLDEN"] = "1"

    print(f"[golden-update] running {args.runtime_tests} {args.gtest_args}")
    completed = subprocess.run([args.runtime_tests, *args.gtest_args], env=env)
    if completed.returncode != 0:
        print(
            "error: runtime_tests failed while regenerating goldens; "
            "a non-deterministic trace fails even in update mode",
            file=sys.stderr,
        )
        return completed.returncode

    print(
        "[golden-update] done; review the diff under "
        "cinderx/RuntimeTests/hir_pipeline_golden before committing"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
