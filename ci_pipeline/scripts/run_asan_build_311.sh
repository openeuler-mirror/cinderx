#!/bin/bash
# AddressSanitizer RuntimeTests leg for CPython 3.11.  The independent Debug
# job owns the full compile-only matrix; this runner builds one instrumented
# RuntimeTests binary and runs both its Green and Canary populations.
set -euo pipefail
# ASAN frames are several times larger than unsanitized ones.  The 3.11
# C-stack hard limit is derived from the thread's mapped stack; keep the
# main thread from sitting inside that margin during canary lifecycle
# tests that compile and enter machine code.
ulimit -s unlimited || true
# Same execution hygiene as the green gate: C collation for every manifest
# comparison, and no inherited GTEST_*/TESTBRIDGE_* that could shard or
# filter away the population this leg is supposed to instrument.
export LC_ALL=C
# The sanitizer leg is a compare-only gate, never a Golden Sample generator.
export UPDATE_HIR_PIPELINE_GOLDEN=0
while IFS='=' read -r name _; do
  case "$name" in GTEST_*|TESTBRIDGE_*) unset "$name" ;; esac
done < <(env)
if [ "${1:-}" = "--verify-golden-update-env" ]; then
  # Self-test entry: cover the direct RuntimeTests invocation without paying
  # for either sanitizer build.
  printf '%s\n' "$UPDATE_HIR_PIPELINE_GOLDEN"
  exit 0
fi
BUILD_DIR=${1:?usage: run_asan_build_311.sh <build_dir>}
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
if [ -n "${CINDERX_RUNTIME_TEST_PYTHON:-}" ]; then
  TEST_PYTHON=$CINDERX_RUNTIME_TEST_PYTHON
  PYTHON_INCLUDE_DIR=${CINDERX_RUNTIME_TEST_PYTHON_INCLUDE_DIR:-}
  PYTHON_LIBRARY=${CINDERX_RUNTIME_TEST_PYTHON_LIBRARY:-}
  PYTHON_EXTENSIONS_DIR=${CINDERX_RUNTIME_TEST_PYTHON_EXTENSIONS_DIR:-}
else
  TEST_PYTHON=${CINDERX_TEST_PYTHON:-python3.11}
  PYTHON_INCLUDE_DIR=${CINDERX_TEST_PYTHON_INCLUDE_DIR:-}
  PYTHON_LIBRARY=${CINDERX_TEST_PYTHON_LIBRARY:-}
  PYTHON_EXTENSIONS_DIR=${CINDERX_TEST_PYTHON_EXTENSIONS_DIR:-}
fi
PYTHON_ROOT=$("$TEST_PYTHON" -c 'import sys; print(sys.base_prefix)')
BUILD_JOBS=${CINDERX_TEST_JOBS:-$(nproc)}
case "$BUILD_JOBS" in
  ''|*[!0-9]*) echo "CINDERX_TEST_JOBS must be a positive integer"; exit 2 ;;
esac
[ "$BUILD_JOBS" -gt 0 ] \
  || { echo "CINDERX_TEST_JOBS must be greater than zero"; exit 2; }

# CMake's default probe picks /usr/bin/cc, which on the build image is an
# older toolchain that ships no sanitizer runtime; the wheel and the green
# gate build with the first gcc on PATH.  Use that same toolchain here, and
# let CI override through CC/CXX.
ASAN_CC=${CC:-$(command -v gcc)}
ASAN_CXX=${CXX:-$(command -v g++)}
# Preflight: prove the toolchain can link an instrumented program before
# spending a full build on a CMake TryCompile dump that says "-lasan".
PREFLIGHT=$(mktemp -d)
trap 'rm -rf "$PREFLIGHT"' EXIT
printf 'int main(void) { return 0; }\n' > "$PREFLIGHT/probe.c"
if ! "$ASAN_CC" -fsanitize=address "$PREFLIGHT/probe.c" \
     -o "$PREFLIGHT/probe" > "$PREFLIGHT/probe.log" 2>&1; then
  echo "asan leg: $ASAN_CC cannot link with -fsanitize=address"
  echo "(install the sanitizer runtime, or point CC/CXX at a toolchain"
  echo "that ships it)"
  cat "$PREFLIGHT/probe.log"
  exit 1
fi

# The sanitizer runtime lives with the toolchain, which is not on the
# loader's default path here (the project already carries libstdc++ the same
# way).  Bake it in as an rpath rather than exporting LD_LIBRARY_PATH, which
# would also reorder library resolution for every child process the build
# and the tests spawn.
ASAN_LIBDIR=$(dirname "$("$ASAN_CC" -print-file-name=libasan.so)")
ASAN_RPATH=""
if [ -d "$ASAN_LIBDIR" ]; then
  ASAN_RPATH="-Wl,-rpath,$ASAN_LIBDIR"
fi

FLAGS=$("$TEST_PYTHON" -c '
import sys
sys.path.insert(0, sys.argv[1])
from cmake_options import cmake_feature_options
opts = cmake_feature_options(py_version="3.11")
print(" ".join(f"-D{k}={v}" for k, v in sorted(opts.items())))
' "$REPO_ROOT/ci_pipeline")
if [ -n "${CINDERX_LOCAL_DEPS_DIR:-}${CINDERX_LOCAL_DEPS:-}" ]; then
  FLAGS="$FLAGS -DCINDERX_LOCAL_DEPS_DIR=${CINDERX_LOCAL_DEPS_DIR:-$CINDERX_LOCAL_DEPS}"
fi
PYTHON_CMAKE_ARGS=(
  -DPython_ROOT_DIR="$PYTHON_ROOT"
  -DPython_EXECUTABLE="$TEST_PYTHON"
)
if [ -n "$PYTHON_INCLUDE_DIR" ]; then
  PYTHON_CMAKE_ARGS+=(
    -DPython_INCLUDE_DIR="$PYTHON_INCLUDE_DIR"
  )
fi
if [ -n "$PYTHON_LIBRARY" ]; then
  PYTHON_CMAKE_ARGS+=(
    -DPython_LIBRARY="$PYTHON_LIBRARY"
  )
fi
# Minimal execution set: the green-family population must run clean under
# the sanitizer.  Leak detection stays off -- CPython's own finalization
# leaks are upstream noise; addressability errors are the contract here.
#
# Configure at RelWithDebInfo.  With assertions live, the vendored 3.11 interpreter
# compiles in CPython's own assert helpers (_PyThreadState_CheckConsistency,
# _Py_CheckSlotResult), which a release libpython does not export, so no
# executable can link.  The separate Debug job keeps assertion coverage.
#
# Leak detection stays off for the whole executed arm, not just the later
# gtest invocations.  CMake's gtest_discover_tests POST_BUILD runs the
# instrumented binary under a timeout helper that looks like ptrace to
# LeakSanitizer; LSAN then fatals after listing tests and make deletes
# the binary.  detect_leaks=0 is the same contract as the run below.
export ASAN_OPTIONS="${ASAN_OPTIONS:+${ASAN_OPTIONS}:}detect_leaks=0"
EXEC_DIR="$BUILD_DIR"
cmake -S "$REPO_ROOT" -B "$EXEC_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER="$ASAN_CC" -DCMAKE_CXX_COMPILER="$ASAN_CXX" \
  "${PYTHON_CMAKE_ARGS[@]}" \
  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address $ASAN_RPATH" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address $ASAN_RPATH" \
  -DENABLE_RUNTIME_TESTS=ON \
  $FLAGS > "$EXEC_DIR-configure.log" 2>&1
make -C "$EXEC_DIR" -j"$BUILD_JOBS" runtime_tests > "$EXEC_DIR-build.log" 2>&1
BIN=$(find "$EXEC_DIR" -name runtime_tests -type f | head -1)
[ -n "$BIN" ] || { echo "asan runtime_tests binary missing"; exit 1; }
RUNTIME_TEST_ENV=()
if [ -n "$PYTHON_EXTENSIONS_DIR" ]; then
  RUNTIME_TEST_ENV+=(
    "PYTHONPATH=$PYTHON_EXTENSIONS_DIR${PYTHONPATH:+:$PYTHONPATH}"
  )
fi
MANIFEST="$REPO_ROOT/ci_pipeline/jit311/data/rt311_green_families.txt"
env "${RUNTIME_TEST_ENV[@]}" "$BIN" --gtest_list_tests 2>/dev/null \
  | awk '/^[A-Za-z_][A-Za-z0-9_\/]*\./ { suite = $1 }
         /^  [A-Za-z_]/ { print suite $1 }' \
  | sort -u > "$EXEC_DIR-registered.txt"
EXPECTED=$(awk -F. 'NR == FNR { fam[$1] = 1; next } ($1 in fam)' \
  "$MANIFEST" "$EXEC_DIR-registered.txt" | wc -l | tr -d ' ')
FILTER=$(awk '{printf "%s.*:", $1}' "$MANIFEST")
set +e
(cd "$REPO_ROOT/cinderx" && \
  env ASAN_OPTIONS=detect_leaks=0 "${RUNTIME_TEST_ENV[@]}" \
    "$BIN" --gtest_filter="${FILTER%:}") \
  > "$EXEC_DIR-run.log" 2>&1
EXEC_CODE=$?
set -e
if [ "$EXEC_CODE" != 0 ]; then
  echo "asan minimal execution set FAILED (exit $EXEC_CODE)"
  tail -30 "$EXEC_DIR-run.log"
  exit 1
fi
# Exit status alone would accept a mistyped filter (zero matches, exit 0) or
# a sanitizer-induced skip, so hand the log to the green gate's own verdict:
# no skips, and PASSED exactly equal to the manifest population.
bash "$REPO_ROOT/ci_pipeline/scripts/run_rt311_green.sh" \
  --verify-green-log "$EXEC_DIR-run.log" "$EXPECTED"
echo "asan minimal execution set ok ($EXPECTED tests)"

# The green families skip every case that installs machine code, because on
# 3.11 only the executing mode installs.  Under a sanitizer that is exactly
# the population worth running: the install, entry and lifecycle paths are
# where a use-after-free would live, and this is the only build that would
# report one.  The population is derived from the same sources the green
# gate derives it from, so the two legs cannot cover different sets.
CANARY_CASES=$(bash "$REPO_ROOT/ci_pipeline/scripts/run_rt311_green.sh" \
  --verify-canary-population)
CANARY_EXPECTED=$(printf '%s\n' "$CANARY_CASES" | grep -c .)
[ "$CANARY_EXPECTED" -ge 1 ] || {
  echo "asan: no mode-gated cases found; the executable-compile family"
  echo "would be sanitized in neither leg"
  exit 1
}
set +e
(cd "$REPO_ROOT/cinderx" && \
  env ASAN_OPTIONS=detect_leaks=0 CINDERX_JIT_MODE=canary \
    "${RUNTIME_TEST_ENV[@]}" "$BIN" \
    --gtest_filter="$(printf '%s\n' "$CANARY_CASES" | paste -sd: -)") \
  > "$EXEC_DIR-canary.log" 2>&1
CANARY_CODE=$?
set -e
if [ "$CANARY_CODE" != 0 ]; then
  echo "asan canary-mode RuntimeTests FAILED (exit $CANARY_CODE)"
  tail -40 "$EXEC_DIR-canary.log"
  exit 1
fi
bash "$REPO_ROOT/ci_pipeline/scripts/run_rt311_green.sh" \
  --verify-green-log "$EXEC_DIR-canary.log" "$CANARY_EXPECTED"
echo "asan canary-mode RuntimeTests ok ($CANARY_EXPECTED tests)"

# RuntimeTests exercise the native compiler and execute-mode cases, but they
# do not prove that the Python extension itself is instrumented and safe to
# initialize, enter, deopt and tear down in a real interpreter.  Build only
# the extension target from this existing ASAN tree, attest its instrumentation
# and run the bounded canary.  This restores the Python/native boundary proof
# without bringing back the removed duplicate full Debug+ASAN build.
make -C "$EXEC_DIR" -j"$BUILD_JOBS" _cinderx \
  > "$EXEC_DIR-extension-build.log" 2>&1 || {
    echo "asan extension build FAILED"
    tail -20 "$EXEC_DIR-extension-build.log"
    exit 1
  }
ASAN_EXT=$(find "$EXEC_DIR" -name '_cinderx*.so' -type f | head -1)
[ -n "$ASAN_EXT" ] || { echo "asan extension missing after build"; exit 1; }

ASAN_SYMS=$(nm -D "$ASAN_EXT" 2>/dev/null | grep -c __asan || true)
ASAN_NEEDED=$(readelf -d "$ASAN_EXT" 2>/dev/null | grep -c libasan || true)
if [ "$ASAN_SYMS" -lt 1 ] || [ "$ASAN_NEEDED" -lt 1 ]; then
  echo "asan extension is not instrumented (__asan symbols: $ASAN_SYMS,"
  echo "libasan NEEDED: $ASAN_NEEDED); the canary would sanitize nothing"
  exit 1
fi
echo "asan extension instrumented ($ASAN_SYMS __asan symbols): $ASAN_EXT"

ASAN_RUNTIME=$("$ASAN_CC" -print-file-name=libasan.so)
CANARY_PYTHONPATH="$(dirname "$ASAN_EXT"):$REPO_ROOT/cinderx/PythonLib"
if [ -n "$PYTHON_EXTENSIONS_DIR" ]; then
  CANARY_PYTHONPATH="$CANARY_PYTHONPATH:$PYTHON_EXTENSIONS_DIR"
fi
(cd "$REPO_ROOT" && \
  env LD_PRELOAD="$ASAN_RUNTIME" \
    ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=0 \
    CINDERX_JIT_MODE=canary PYTHONJITAUTO=1 \
    PYTHONPATH="$CANARY_PYTHONPATH" \
    "$TEST_PYTHON" "$REPO_ROOT/ci_pipeline/scripts/asan_canary_smoke.py") \
  > "$EXEC_DIR-extension-canary.log" 2>&1 || {
    echo "asan extension canary FAILED"
    tail -30 "$EXEC_DIR-extension-canary.log"
    exit 1
  }
grep -q 'asan canary entries=' "$EXEC_DIR-extension-canary.log" || {
  echo "asan extension canary produced no machine-code entry proof"
  tail -20 "$EXEC_DIR-extension-canary.log"
  exit 1
}
grep 'asan canary ' "$EXEC_DIR-extension-canary.log"
echo "asan extension canary ok"
