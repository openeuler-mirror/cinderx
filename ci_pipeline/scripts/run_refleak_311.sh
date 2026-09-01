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
PY_VERSION=3.11.6
THRESHOLD=${PYTHONJITAUTO:-20}
# -R <warmups>:<repetitions>.  regrtest reports a leak only when the
# totals move in every repetition, so the repetitions are the judge.  Ten
# warmups let threshold-triggered compilation settle before those measured
# repetitions; otherwise compilation itself lands inside the measurement.
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
  setuptools==82.0.1 wheel==0.47.0 > "$WORK/pip.log" 2>&1
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
RC=0
FAILED_MODULES=()
for MODULE in "${MODULES[@]}"; do
  MODULE_LOG="$MODULE_LOG_DIR/$MODULE.log"
  set +e
  env CINDERX_PLUGIN_ENABLE=1 CINDERX_EVAL_MODE=cinder \
      CINDERX_JIT_MODE=execute PYTHONJITAUTO="$THRESHOLD" \
      CINDERX_REFLEAK_LEDGER="$LEDGER" \
      PYTHONPATH="$START${PYTHONPATH:+:$PYTHONPATH}" \
    "$VENV_PY" -m test -R "$ROUNDS" "$MODULE" > "$MODULE_LOG" 2>&1
  MODULE_RC=$?
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
  else
    RC=2
    FAILED_MODULES+=("$MODULE")
  fi
done
tail -20 "$WORK/regrtest.log"

# Reference leaks are the acceptance item's subject and are never excused.
REF_LINES=$(grep -E "leaked \[[-0-9, ]+\] references" "$WORK/regrtest.log" || true)
if [ -n "$REF_LINES" ]; then
  echo "refleak: regrtest -R reported reference leaks"
  echo "$REF_LINES"
  exit 1
fi
if grep -qE "file descriptors leaked" "$WORK/regrtest.log"; then
  echo "refleak: regrtest -R reported file-descriptor leaks"
  grep -E "file descriptors leaked" "$WORK/regrtest.log"
  exit 1
fi

# Block lines need verifying rather than believing.  regrtest computes
# its block figure as getallocatedblocks() - _getquickenedcount(), and
# that subtraction is unsound here: _Py_QuickenedCount is a file-local
# symbol in the interpreter, so the vendored evaluator must define its
# own copy.  Quickening then increments CinderX's counter while
# code_dealloc decrements the interpreter's -- the one sys reports -- so
# it only ever falls, and subtracting a negative inflates the figure.
# quickened_artifact.py re-runs each flagged module recording both terms
# and clears the line only when the raw block count is flat over a
# settled tail AND the counter drift accounts for the reported figure.
BLK_MODULES=$(grep -E "leaked \[[-0-9, ]+\] memory blocks" "$WORK/regrtest.log" \
  | awk '{print $1}' | sort -u || true)
if [ -n "$BLK_MODULES" ]; then
  echo "refleak: verifying block lines for: $(echo "$BLK_MODULES" | tr '\n' ' ')"
  # shellcheck disable=SC2086
  if ! env CINDERX_PLUGIN_ENABLE=1 CINDERX_EVAL_MODE=cinder \
       CINDERX_JIT_MODE=execute PYTHONJITAUTO="$THRESHOLD" \
       "$VENV_PY" "$REPO_ROOT/ci_pipeline/jit311/quickened_artifact.py" \
       "$VENV_PY" $BLK_MODULES --warmups 30 --reps 8; then
    echo "refleak: a reported block figure is not the quickened-counter"
    echo "artifact -- treating it as a real leak"
    exit 1
  fi
elif [ "$RC" != 0 ]; then
  echo "refleak: regrtest -R reported failures (exit $RC) with no leak lines"
  exit 1
fi
# A non-zero module is acceptable only when that same module's block figure
# was verified above.  Looking for any global SUCCESS epilogue would let a
# later successful module hide an earlier real failure.
if ((${#FAILED_MODULES[@]})); then
  FAILED=$(printf '%s\n' "${FAILED_MODULES[@]}" | sort -u)
  UNEXPLAINED=$(comm -23 <(printf '%s\n' "$FAILED") \
    <(printf '%s\n' "$BLK_MODULES" | sort -u) || true)
  if [ -n "$UNEXPLAINED" ]; then
    echo "refleak: regrtest failed on tests beyond the verified block"
    echo "artifact: $(echo "$UNEXPLAINED" | tr '\n' ' ')"
    exit 1
  fi
  echo "refleak: the only failures were block figures verified as the"
  echo "quickened-counter artifact"
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
