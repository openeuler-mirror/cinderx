#!/bin/bash
# Debug arm of the MR-01 build matrix: the full source set compiles with
# assertions and debug info.  Catches assertion-only and -O0-only breakage
# the release legs cannot see.
set -euo pipefail
BUILD_DIR=${1:?usage: run_debug_build_311.sh <build_dir>}
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TEST_PYTHON=${CINDERX_TEST_PYTHON:-python3.11}
PYTHON_ROOT=$("$TEST_PYTHON" -c 'import sys; print(sys.base_prefix)')
BUILD_JOBS=${CINDERX_TEST_JOBS:-$(nproc)}
case "$BUILD_JOBS" in
  ''|*[!0-9]*) echo "CINDERX_TEST_JOBS must be a positive integer"; exit 2 ;;
esac
[ "$BUILD_JOBS" -gt 0 ] \
  || { echo "CINDERX_TEST_JOBS must be greater than zero"; exit 2; }

FLAGS=$("$TEST_PYTHON" -c '
import sys
sys.path.insert(0, sys.argv[1])
from cmake_options import cmake_feature_options
opts = cmake_feature_options(py_version="3.11")
print(" ".join(f"-D{k}={v}" for k, v in sorted(opts.items())))
' "$REPO_ROOT/ci_pipeline")
if [ -n "${CINDERX_LOCAL_DEPS_DIR:-${CINDERX_LOCAL_DEPS:-}}" ]; then
  FLAGS="$FLAGS -DCINDERX_LOCAL_DEPS_DIR=${CINDERX_LOCAL_DEPS_DIR:-$CINDERX_LOCAL_DEPS}"
fi
PYTHON_CMAKE_ARGS=(
  -DPython_ROOT_DIR="$PYTHON_ROOT"
  -DPython_EXECUTABLE="$TEST_PYTHON"
)
if [ -n "${CINDERX_TEST_PYTHON_INCLUDE_DIR:-}" ]; then
  PYTHON_CMAKE_ARGS+=(
    -DPython_INCLUDE_DIR="$CINDERX_TEST_PYTHON_INCLUDE_DIR"
  )
fi
if [ -n "${CINDERX_TEST_PYTHON_LIBRARY:-}" ]; then
  PYTHON_CMAKE_ARGS+=(
    -DPython_LIBRARY="$CINDERX_TEST_PYTHON_LIBRARY"
  )
fi
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug \
  "${PYTHON_CMAKE_ARGS[@]}" \
  $FLAGS > "$BUILD_DIR-configure.log" 2>&1
make -C "$BUILD_DIR" -j"$BUILD_JOBS" > "$BUILD_DIR-build.log" 2>&1
echo "debug build ok: $BUILD_DIR"
