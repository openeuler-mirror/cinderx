#!/bin/bash
# Debug arm of the MR-01 build matrix: the full source set compiles with
# assertions and debug info.  Catches assertion-only and -O0-only breakage
# the release legs cannot see.
set -euo pipefail
BUILD_DIR=${1:?usage: run_debug_build_311.sh <build_dir>}
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)

FLAGS=$(python3.11 -c '
import sys
sys.path.insert(0, sys.argv[1])
from cmake_options import cmake_feature_options
opts = cmake_feature_options(py_version="3.11")
print(" ".join(f"-D{k}={v}" for k, v in sorted(opts.items())))
' "$REPO_ROOT/ci_pipeline")
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug \
  $FLAGS > "$BUILD_DIR-configure.log" 2>&1
make -C "$BUILD_DIR" -j"$(nproc)" > "$BUILD_DIR-build.log" 2>&1
echo "debug build ok: $BUILD_DIR"
