#!/bin/bash
# regrtest's reference-leak hunt (-R) for the CPython 3.11 execute mode.
#
# This is the acceptance item's own instrument, not a substitute for it.
# It needs sys.gettotalrefcount, which only a Py_DEBUG interpreter has, so
# the leg builds one -- vanilla CPython 3.11.6, the version this port is
# pinned to -- and builds CinderX against it.  Both are cached in the work
# directory; only the first run pays for them.
#
# The arm fails closed on its own premises, and the proof comes from the
# -R run itself.  A separate probe process can only show that the build is
# capable of running the JIT; it says nothing about the process that
# actually reported "no leaks".  So the -R run is started through the
# product startup path (the wheel's .pth, which needs
# CINDERX_PLUGIN_ENABLE=1 and CINDERX_EVAL_MODE=cinder -- without them the
# interpreter runs stock and a clean -R means nothing), and every process
# in that run attests at exit: whether the evaluator was installed, and
# what its machine-code counters read.  The verdict is only read after
# that attestation checks out.
#
# Usage: run_refleak_311.sh <work-dir> [module ...]
set -euo pipefail
export LC_ALL=C

WORK=${1:?usage: run_refleak_311.sh <work-dir> [module ...]}
shift || true
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD_REQUIREMENTS="$REPO_ROOT/ci_pipeline/requirements-cp311-build.txt"
PY_VERSION=3.11.6
THRESHOLD=${PYTHONJITAUTO:-20}
# -R <warmups>:<repetitions>.  regrtest reports a leak only when the
# totals move in every repetition, so the repetitions are the judge.  Ten
# warmups let threshold-triggered compilation settle before those measured
# repetitions. Disable startup grace in the -R workers so delayed compilation
# cannot first become eligible during the measured repetitions.
ROUNDS=${CINDERX_REFLEAK_ROUNDS:-10:3}
BUILD_JOBS=${CINDERX_TEST_JOBS:-$(nproc)}
case "$BUILD_JOBS" in
  ''|*[!0-9]*) echo "CINDERX_TEST_JOBS must be a positive integer"; exit 2 ;;
esac
[ "$BUILD_JOBS" -gt 0 ] \
  || { echo "CINDERX_TEST_JOBS must be greater than zero"; exit 2; }
PYTHON_SOURCE_ARCHIVE=${CINDERX_REFLEAK_PYTHON_SOURCE:-}
PIP_ARGS=(--disable-pip-version-check --no-cache-dir)
if [ -n "${CINDERX_PIP_WHEELHOUSE:-}" ]; then
  PIP_ARGS+=(--no-index --find-links="$CINDERX_PIP_WHEELHOUSE")
fi

MODULES=("$@")
if [ ${#MODULES[@]} -eq 0 ]; then
  mapfile -t MODULES < <(grep -vE '^\s*(#|$)' \
    "$REPO_ROOT/ci_pipeline/jit311/data/refleak_modules.txt")
fi

mkdir -p "$WORK"
PREFIX="$WORK/py-$PY_VERSION-debug"
PYD="$PREFIX/bin/python$(echo "$PY_VERSION" | cut -d. -f1,2)"

if [ ! -x "$PYD" ]; then
  echo "building a Py_DEBUG CPython $PY_VERSION (first run only)"
  SRC="$WORK/src"
  mkdir -p "$SRC"
  ( cd "$SRC"
    if [ ! -f "Python-$PY_VERSION.tgz" ]; then
      if [ -n "$PYTHON_SOURCE_ARCHIVE" ]; then
        test -f "$PYTHON_SOURCE_ARCHIVE" || {
          echo "refleak Python source archive missing: $PYTHON_SOURCE_ARCHIVE"
          exit 1
        }
        cp "$PYTHON_SOURCE_ARCHIVE" "Python-$PY_VERSION.tgz"
      else
        curl -sSLO "https://www.python.org/ftp/python/$PY_VERSION/Python-$PY_VERSION.tgz"
      fi
    fi
    rm -rf "Python-$PY_VERSION" && tar xf "Python-$PY_VERSION.tgz"
    cd "Python-$PY_VERSION"
    ./configure --with-pydebug --prefix="$PREFIX" --with-ensurepip=install
    make -j"$BUILD_JOBS"
    make install
  ) > "$WORK/cpython-build.log" 2>&1
fi

if ! "$PYD" -c 'import sys; raise SystemExit(0 if hasattr(sys, "gettotalrefcount") else 1)'; then
  echo "the interpreter at $PYD is not a Py_DEBUG build; -R cannot run"
  exit 1
fi

# CinderX against that interpreter.  The wheel carries the cp311d ABI tag,
# so it can never be confused with the release one.
"$PYD" -m pip install -q "${PIP_ARGS[@]}" \
  -r "$BUILD_REQUIREMENTS" > "$WORK/pip.log" 2>&1
rm -rf "$WORK/wheels"
( cd "$REPO_ROOT" && CMAKE_BUILD_PARALLEL_LEVEL="$BUILD_JOBS" \
    CMAKE_BUILD_TYPE=Release "$PYD" -m pip wheel . \
    -w "$WORK/wheels" --no-deps --no-build-isolation ) > "$WORK/cinderx-build.log" 2>&1
WHEEL=$(ls "$WORK"/wheels/cinderx-*.whl)
case "$WHEEL" in
  *cp311d*) ;;
  *) echo "the wheel $WHEEL is not a debug-ABI build"; exit 1 ;;
esac

rm -rf "$WORK/venv"
"$PYD" -m venv "$WORK/venv"
"$WORK/venv/bin/pip" install -q "$WHEEL"
VENV_PY="$WORK/venv/bin/python"

# Exercise finalization with Py_DEBUG assertions enabled as well as the
# release gate: the in-place JIT finalizer enters deallocation before it
# restores the stock generator type.
PYTHONPATH="$REPO_ROOT/cinderx/PythonLib" "$VENV_PY" -m unittest -v \
  test_cinderx.test_kunpeng.test_generator_close_semantics_311 \
  test_cinderx.test_kunpeng.test_subscript_cache_lifetime_311 \
  test_cinderx.test_kunpeng.test_quickened_count_311 \
  test_cinderx.test_kunpeng.test_exception_handler_semantics_311 \
  test_cinderx.test_kunpeng.test_execute_surface_coverage_311 \
  test_cinderx.test_kunpeng.test_scheduling_policy_311 \
  test_cinderx.test_kunpeng.test_speculation_feedback_311 \
  test_cinderx.test_kunpeng.test_range_lowering_311 \
  test_cinderx.test_kunpeng.test_loop_phi_identity_311 \
  > "$WORK/generator-close.log" 2>&1

CINDERX_PLUGIN_ENABLE=1 CINDERX_EVAL_MODE=cinder \
CINDERX_JIT_MODE=execute CINDERX_OSR_ENABLED=0 \
PYTHONJITAUTO=1000000 AUTO_JIT=1000000 \
PYTHONPATH="$REPO_ROOT/cinderx/PythonLib" "$VENV_PY" -m unittest -v \
  test_cinderx.test_kunpeng.test_admission_semantics_311 \
  > "$WORK/admission-semantics.log" 2>&1

CINDERX_REQUIRE_CORO_INLINE=1 CINDERX_PLUGIN_ENABLE=1 \
  CINDERX_EVAL_MODE=cinder CINDERX_JIT_MODE=execute PYTHONJITAUTO=4 \
  PYTHONJITGENERATOR=1 PYTHONJITLIGHTWEIGHTFRAME=0 CINDERX_OSR_ENABLED=0 \
  PYTHONPATH="$REPO_ROOT/cinderx/PythonLib" "$VENV_PY" -m unittest -v \
  test_cinderx.test_kunpeng.test_coroutine_call_inline_311 \
  test_cinderx.test_kunpeng.test_subprocess_setup_311 \
  > "$WORK/coroutine-call.log" 2>&1

# Build sanity only: this shows the debug build CAN run the JIT.  The
# trigger proof comes from the -R processes themselves, below.
CINDERX_JIT_MODE=execute PYTHONJITAUTO="$THRESHOLD" \
  "$VENV_PY" "$REPO_ROOT/ci_pipeline/scripts/refleak_probe_311.py" \
  > "$WORK/probe.log" 2>&1
cat "$WORK/probe.log"

# Attestation for the -R run.  It only records; the evaluator is installed
# by the product startup path, so what is attested is that path working,
# not something this file did on its behalf.
LEDGER="$WORK/refleak-trigger.log"
: > "$LEDGER"
START="$WORK/startup"
mkdir -p "$START"
cp "$REPO_ROOT/ci_pipeline/scripts/refleak_attest_sitecustomize.py" \
  "$START/sitecustomize.py"

echo "running: one python -m test -R $ROUNDS process per module: ${MODULES[*]}"
MODULE_LOG_DIR="$WORK/regrtest-modules"
rm -rf "$MODULE_LOG_DIR"
mkdir -p "$MODULE_LOG_DIR"
: > "$WORK/regrtest.log"
for MODULE in "${MODULES[@]}"; do
  MODULE_LOG="$MODULE_LOG_DIR/$MODULE.log"
  set +e
  env CINDERX_PLUGIN_ENABLE=1 CINDERX_EVAL_MODE=cinder \
      CINDERX_JIT_MODE=execute PYTHONJITAUTO="$THRESHOLD" \
      CINDERX_JIT_STARTUP_GRACE_MS=0 \
      CINDERX_REFLEAK_LEDGER="$LEDGER" \
      PYTHONPATH="$START" \
    "$VENV_PY" -m test -R "$ROUNDS" "$MODULE" > "$MODULE_LOG" 2>&1
  MODULE_RC=$?
  printf '%s\n' "$MODULE_RC" > "$MODULE_LOG_DIR/$MODULE.exitcode"
  set -e
  if [ "$MODULE_RC" != 0 ] && [ "$MODULE_RC" != 2 ]; then
    echo "refleak: $MODULE exited abnormally ($MODULE_RC)"
    tail -30 "$MODULE_LOG"
    exit 1
  fi
  grep -qE '^Result: |^== Tests result: ' "$MODULE_LOG" || {
    echo "refleak: $MODULE produced no completion epilogue"
    tail -30 "$MODULE_LOG"
    exit 1
  }
  {
    echo "===== $MODULE (exit $MODULE_RC) ====="
    cat "$MODULE_LOG"
  } >> "$WORK/regrtest.log"
  if [ "$MODULE_RC" = 0 ]; then
    grep -qE '^Result: SUCCESS|^== Tests result: SUCCESS' "$MODULE_LOG" || {
      echo "refleak: $MODULE exited zero without a success epilogue"
      tail -30 "$MODULE_LOG"
      exit 1
    }
  fi
done
tail -20 "$WORK/regrtest.log"

# Quickening and code destruction now update CPython's same native counter.
# Require each ORIGINAL -R process to pass. No memory-block finding, including
# the former [1, 1, 1] test_binop figure, is reclassified or excused.
if ! "$VENV_PY" -m ci_pipeline.jit311.refleak_verdict \
    --logs "$MODULE_LOG_DIR" "${MODULES[@]}"; then
  exit 1
fi

# The proof, from the -R run itself.
ROWS=$(grep -c . "$LEDGER" || true)
INSTALLED=$(awk '$2 == "True"' "$LEDGER" | wc -l)
ENTRIES=$(awk '$2 == "True" { total += $3 } END { print total + 0 }' "$LEDGER")
echo "refleak: $ROWS attesting process(es), $INSTALLED with the evaluator" \
     "installed, $ENTRIES machine-code entries among them"
if [ "$ROWS" -lt 1 ]; then
  echo "refleak: no process attested; the -R run cannot vouch for itself"
  exit 1
fi
if [ "$INSTALLED" -lt 1 ]; then
  echo "refleak: no -R process installed the evaluator -- the run was stock"
  echo "CPython, so a clean -R says nothing about the JIT"
  exit 1
fi
if [ "$ENTRIES" -le 0 ]; then
  echo "refleak: the -R processes installed the evaluator but never entered"
  echo "machine code; a leak in compiled-code lifetime could not have shown"
  exit 1
fi
printf '{"modules":%d,"rounds":"%s","attesting_processes":%d,' \
  "${#MODULES[@]}" "$ROUNDS" "$ROWS" > "$WORK/refleak-summary.json"
printf '"evaluator_installed_processes":%d,"machine_code_entries":%d,' \
  "$INSTALLED" "$ENTRIES" >> "$WORK/refleak-summary.json"
printf '"reference_leaks":0}\n' >> "$WORK/refleak-summary.json"
echo "refleak: -R $ROUNDS over ${#MODULES[@]} module(s), no leaks"
