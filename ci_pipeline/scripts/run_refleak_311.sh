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
# totals move in every repetition, so the repetitions are the judge.
ROUNDS=${CINDERX_REFLEAK_ROUNDS:-3:3}

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
      curl -sSLO "https://www.python.org/ftp/python/$PY_VERSION/Python-$PY_VERSION.tgz"
    fi
    rm -rf "Python-$PY_VERSION" && tar xf "Python-$PY_VERSION.tgz"
    cd "Python-$PY_VERSION"
    ./configure --with-pydebug --prefix="$PREFIX" --with-ensurepip=install
    make -j"$(nproc)"
    make install
  ) > "$WORK/cpython-build.log" 2>&1
fi

if ! "$PYD" -c 'import sys; raise SystemExit(0 if hasattr(sys, "gettotalrefcount") else 1)'; then
  echo "the interpreter at $PYD is not a Py_DEBUG build; -R cannot run"
  exit 1
fi

# CinderX against that interpreter.  The wheel carries the cp311d ABI tag,
# so it can never be confused with the release one.
"$PYD" -m pip install -q --upgrade "setuptools>=77" wheel > "$WORK/pip.log" 2>&1
rm -rf "$WORK/wheels"
( cd "$REPO_ROOT" && CMAKE_BUILD_TYPE=Release "$PYD" -m pip wheel . \
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

# Build sanity only: this shows the debug build CAN run the JIT.  It is
# NOT the trigger proof -- that has to come from the -R processes
# themselves, and does, below.
CINDERX_JIT_MODE=execute PYTHONJITAUTO="$THRESHOLD" "$VENV_PY" - <<'PROBE' > "$WORK/probe.log" 2>&1
import sys
import _cinderx, cinderx
assert hasattr(sys, "gettotalrefcount"), "not a Py_DEBUG interpreter"
cinderx.init()
_cinderx.install_frame_evaluator()
assert _cinderx.is_frame_evaluator_installed(), "evaluator did not install"
import cinderjit

def hot(a, b):
    total = a - a
    i = total
    while i < b:
        total = total + a
        i = i + 1
    return total

for i in range(200):
    hot(i, 2)
assert cinderjit.is_jit_compiled(hot), "nothing compiled in the refleak arm"
stats = _cinderx._get_trigger_stats()
assert stats["machine_code_entries"] > 0, "nothing entered machine code"
print("refleak arm executes: entries=%d creations=%d"
      % (stats["machine_code_entries"], stats["compiled_function_creations"]))
PROBE
cat "$WORK/probe.log"

# Attestation for the -R run.  It only records; the evaluator is installed
# by the product startup path, so what is attested is that path working,
# not something this file did on its behalf.
LEDGER="$WORK/refleak-trigger.log"
: > "$LEDGER"
START="$WORK/startup"
mkdir -p "$START"
cat > "$START/sitecustomize.py" <<'PY'
import atexit
import os


def _attest():
    ledger = os.environ.get("CINDERX_REFLEAK_LEDGER")
    if not ledger:
        return
    try:
        import _cinderx

        stats = _cinderx._get_trigger_stats()
        line = "%d %s %d %d\n" % (
            os.getpid(),
            bool(_cinderx.is_frame_evaluator_installed()),
            stats["machine_code_entries"],
            stats["compiled_function_creations"],
        )
    except Exception:
        line = "%d unavailable 0 0\n" % os.getpid()
    try:
        with open(ledger, "a", encoding="utf-8") as fh:
            fh.write(line)
    except OSError:
        pass


atexit.register(_attest)
PY

echo "running: python -m test -R $ROUNDS ${MODULES[*]}"
set +e
env CINDERX_PLUGIN_ENABLE=1 CINDERX_EVAL_MODE=cinder \
    CINDERX_JIT_MODE=execute PYTHONJITAUTO="$THRESHOLD" \
    CINDERX_REFLEAK_LEDGER="$LEDGER" \
    PYTHONPATH="$START${PYTHONPATH:+:$PYTHONPATH}" \
  "$VENV_PY" -m test -R "$ROUNDS" "${MODULES[@]}" > "$WORK/regrtest.log" 2>&1
RC=$?
set -e
tail -20 "$WORK/regrtest.log"
if [ "$RC" != 0 ]; then
  echo "refleak: regrtest -R reported failures (exit $RC)"
  exit 1
fi
if grep -qE "leaked \[|references leaked|memory blocks leaked" "$WORK/regrtest.log"; then
  echo "refleak: regrtest -R reported leaks"
  grep -E "leaked \[|references leaked|memory blocks leaked" "$WORK/regrtest.log" | head
  exit 1
fi
# A run that executed nothing would also print no leaks.
if ! grep -qE "^Result: SUCCESS|== Tests result: SUCCESS" "$WORK/regrtest.log"; then
  echo "refleak: regrtest did not report success; refusing to read that as clean"
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
echo "refleak: -R $ROUNDS over ${#MODULES[@]} module(s), no leaks"
