#!/bin/bash
# AddressSanitizer leg for the CPython 3.11 build (dev plan MR-02
# acceptance item 9, extended by MR-04): builds the full source set with
# ASAN instrumentation and then RUNS the minimal execution set -- the
# green-family RuntimeTests population -- under the instrumented binary,
# so heap misuse in the compile pipeline and the execution scaffolding
# fails loudly instead of compiling quietly.
set -euo pipefail
# Same execution hygiene as the green gate: C collation for every manifest
# comparison, and no inherited GTEST_*/TESTBRIDGE_* that could shard or
# filter away the population this leg is supposed to instrument.
export LC_ALL=C
while IFS='=' read -r name _; do
  case "$name" in GTEST_*|TESTBRIDGE_*) unset "$name" ;; esac
done < <(env)
BUILD_DIR=${1:?usage: run_asan_build_311.sh <build_dir>}
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)

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

FLAGS=$(python3.11 -c '
import sys
sys.path.insert(0, sys.argv[1])
from cmake_options import cmake_feature_options
opts = cmake_feature_options(py_version="3.11")
print(" ".join(f"-D{k}={v}" for k, v in sorted(opts.items())))
' "$REPO_ROOT/ci_pipeline")
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER="$ASAN_CC" -DCMAKE_CXX_COMPILER="$ASAN_CXX" \
  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address $ASAN_RPATH" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address $ASAN_RPATH" \
  $FLAGS > "$BUILD_DIR-configure.log" 2>&1
make -C "$BUILD_DIR" -j"$(nproc)" > "$BUILD_DIR-build.log" 2>&1
echo "asan build ok: $BUILD_DIR"

# Minimal execution set: the green-family population must run clean under
# the sanitizer.  Leak detection stays off -- CPython's own finalization
# leaks are upstream noise; addressability errors are the contract here.
#
# The executed arm configures separately, at RelWithDebInfo rather than the
# Debug used above.  With assertions live, the vendored 3.11 interpreter
# compiles in CPython's own assert helpers (_PyThreadState_CheckConsistency,
# _Py_CheckSlotResult), which a release libpython does not export, so no
# executable can link.  The compile-only arm above keeps that assertion
# coverage; this arm keeps the sanitizer coverage of code that actually runs.
EXEC_DIR="$BUILD_DIR-exec"
cmake -S "$REPO_ROOT" -B "$EXEC_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER="$ASAN_CC" -DCMAKE_CXX_COMPILER="$ASAN_CXX" \
  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address $ASAN_RPATH" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address $ASAN_RPATH" \
  -DENABLE_RUNTIME_TESTS=ON \
  $FLAGS > "$EXEC_DIR-configure.log" 2>&1
make -C "$EXEC_DIR" -j"$(nproc)" runtime_tests > "$EXEC_DIR-build.log" 2>&1
BIN=$(find "$EXEC_DIR" -name runtime_tests -type f | head -1)
[ -n "$BIN" ] || { echo "asan runtime_tests binary missing"; exit 1; }
MANIFEST="$REPO_ROOT/ci_pipeline/jit311/data/rt311_green_families.txt"
"$BIN" --gtest_list_tests 2>/dev/null \
  | awk '/^[A-Za-z_][A-Za-z0-9_]*\./ { suite = $1 }
         /^  [A-Za-z_]/ { print suite $1 }' \
  | sort -u > "$EXEC_DIR-registered.txt"
EXPECTED=$(awk -F. 'NR == FNR { fam[$1] = 1; next } ($1 in fam)' \
  "$MANIFEST" "$EXEC_DIR-registered.txt" | wc -l | tr -d ' ')
FILTER=$(awk '{printf "%s.*:", $1}' "$MANIFEST")
set +e
(cd "$REPO_ROOT/cinderx" && \
  ASAN_OPTIONS=detect_leaks=0 "$BIN" --gtest_filter="${FILTER%:}") \
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
  ASAN_OPTIONS=detect_leaks=0 CINDERX_JIT_MODE=canary "$BIN" \
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

# The RuntimeTests population above instruments the compiler; it never
# enters machine code.  The prologue, the materialized frame, the epilogue,
# the deopt return and the shutdown teardown only run when compiled code
# actually executes, so the leg also runs a canary child against the
# instrumented extension.
#
# The extension comes from this CMake build, not from `pip wheel`: setup.py
# never forwards CFLAGS/CXXFLAGS into its CMake invocation and reuses a
# cached build tree under scratch/, so an environment-flag wheel build can
# silently produce an uninstrumented extension and pair it with a
# preloaded runtime -- a green leg that sanitized nothing.  It is built
# here at RelWithDebInfo for the same reason the executed RuntimeTests are:
# an assertion-enabled extension references CPython debug helpers a release
# libpython does not export, so it cannot even be imported.
make -C "$EXEC_DIR" -j"$(nproc)" _cinderx > "$EXEC_DIR-ext.log" 2>&1 \
  || { echo "asan extension build FAILED"; tail -20 "$EXEC_DIR-ext.log"; exit 1; }
ASAN_EXT=$(find "$EXEC_DIR" -name "_cinderx*.so" -type f | head -1)
[ -n "$ASAN_EXT" ] || { echo "asan extension missing after build"; exit 1; }
# Prove the thing under test is instrumented before trusting what it says.
ASAN_SYMS=$(nm -D "$ASAN_EXT" 2>/dev/null | grep -c __asan || true)
ASAN_NEEDED=$(readelf -d "$ASAN_EXT" 2>/dev/null | grep -c libasan || true)
if [ "$ASAN_SYMS" -lt 1 ] || [ "$ASAN_NEEDED" -lt 1 ]; then
  echo "asan extension is not instrumented (__asan symbols: $ASAN_SYMS,"
  echo "libasan NEEDED: $ASAN_NEEDED); the canary run would sanitize nothing"
  exit 1
fi
echo "asan extension instrumented ($ASAN_SYMS __asan symbols): $ASAN_EXT"

ASAN_RUNTIME=$("$ASAN_CC" -print-file-name=libasan.so)
# detect_leaks stays off (CPython finalization leaks are upstream noise);
# addressability and use-after-free are the contract.  alloc_dealloc
# mismatch is disabled because CPython legitimately pairs its own
# allocators across the boundary.  The interpreter itself is not
# instrumented, so the runtime is preloaded.
(cd "$REPO_ROOT" && \
  LD_PRELOAD="$ASAN_RUNTIME" \
  ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=0 \
  CINDERX_JIT_MODE=canary PYTHONJITAUTO=1 \
  PYTHONPATH="$(dirname "$ASAN_EXT"):$REPO_ROOT/cinderx/PythonLib" \
  python3.11 "$REPO_ROOT/ci_pipeline/scripts/asan_canary_smoke.py") \
  > "$BUILD_DIR-canary.log" 2>&1 \
  || { echo "asan canary execution FAILED"; tail -30 "$BUILD_DIR-canary.log"; exit 1; }
grep -q "asan canary entries=" "$BUILD_DIR-canary.log" \
  || { echo "asan canary produced no entry report"; tail -20 "$BUILD_DIR-canary.log"; exit 1; }
grep "asan canary " "$BUILD_DIR-canary.log"
echo "asan canary execution ok"
