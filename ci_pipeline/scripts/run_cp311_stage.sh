#!/usr/bin/env bash
# Compose the public CPython 3.11 jobs while retaining one log and duration
# for every sub-step.
set -euo pipefail

STAGE=${1:?usage: run_cp311_stage.sh <setup_release|test_release|libtest_execute_72|test_release_daily|libtest_daily> <run_dir>}
RUN_DIR=${2:?usage: run_cp311_stage.sh <stage> <run_dir>}
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TEST_PYTHON=${CINDERX_TEST_PYTHON:-python3.11}
BUILD_JOBS=${CINDERX_TEST_JOBS:-$(nproc)}
PIPELINE_MODE=${CINDERX_CP311_PIPELINE_MODE:-}
VENV="$RUN_DIR/venv"
PYTHON="$VENV/bin/python"
PIP="$VENV/bin/pip"
LOG_DIR="$RUN_DIR/logs/cp311-$STAGE"

case "$BUILD_JOBS" in
  ''|*[!0-9]*) echo "CINDERX_TEST_JOBS must be a positive integer"; exit 2 ;;
esac
[ "$BUILD_JOBS" -gt 0 ] || {
  echo "CINDERX_TEST_JOBS must be greater than zero"
  exit 2
}
mkdir -p "$LOG_DIR"

"$TEST_PYTHON" - <<'PY'
import sys
import sysconfig

assert sys.version_info[:3] == (3, 11, 6), sys.version
assert sysconfig.get_config_var("SOABI") == "cpython-311-aarch64-linux-gnu"
print("CPython 3.11 gate interpreter", sys.executable)
print("CPython 3.11 gate prefix", sys.base_prefix)
PY

run_step() {
  local name=$1
  shift
  local started ended rc had_errexit=0
  started=$(date +%s)
  echo "[ CP311 STEP ] $name"
  [[ $- == *e* ]] && had_errexit=1
  set +e
  (
    set -e
    "$@"
  ) > >(tee "$LOG_DIR/$name.log") 2>&1
  rc=$?
  if [ "$had_errexit" -eq 1 ]; then
    set -e
  else
    set +e
  fi
  ended=$(date +%s)
  printf '%s\n' "$((ended - started))" > "$LOG_DIR/$name.seconds"
  if [ "$rc" -ne 0 ]; then
    echo "[ CP311 FAIL ] $name (exit $rc, $((ended - started))s)"
    return "$rc"
  fi
  echo "[ CP311 PASS ] $name ($((ended - started))s)"
}

DAILY_FAILURES=()

run_step_continue() {
  local name=$1
  local rc
  set +e
  run_step "$@"
  rc=$?
  set -e
  if [ "$rc" -ne 0 ]; then
    DAILY_FAILURES+=("$name:$rc")
  fi
  return 0
}

finish_daily_stage() {
  if [ "${#DAILY_FAILURES[@]}" -eq 0 ]; then
    return 0
  fi
  printf '[ CP311 DAILY FAILURES ] %s\n' "${DAILY_FAILURES[*]}"
  return 1
}

require_candidate() {
  [ -x "$PYTHON" ] || {
    echo "candidate venv is missing: $PYTHON; setup_release_311 must run first"
    exit 2
  }
}

pip_args() {
  PIP_ARGS=(--disable-pip-version-check)
  if [ -n "${CINDERX_PIP_WHEELHOUSE:-}" ]; then
    PIP_ARGS+=(--no-index --find-links "$CINDERX_PIP_WHEELHOUSE")
  elif [ "${CINDERX_PIP_OFFLINE:-0}" = "1" ]; then
    echo "CINDERX_PIP_OFFLINE=1 requires CINDERX_PIP_WHEELHOUSE" >&2
    return 2
  fi
}

verify_daily_wheel_source() {
  local wheel=$1
  "$TEST_PYTHON" - "$wheel" "$REPO_ROOT" <<'PY'
import json
import subprocess
import sys
import zipfile

from ci_pipeline.jit311.execution_acceptance import require_matching_source_sha

wheel, source = sys.argv[1:]
try:
    with zipfile.ZipFile(wheel) as archive:
        provenance = json.loads(
            archive.read("cinderx/_native/build_info_311.json").decode("utf-8")
        )
except (KeyError, OSError, ValueError, zipfile.BadZipFile) as exc:
    raise SystemExit(f"daily wheel provenance is missing or invalid: {exc}")

if provenance.get("format") != "cinderx-cp311-wheel-v1":
    raise SystemExit(
        f"unexpected daily wheel provenance format: {provenance.get('format')!r}"
    )
source_sha = subprocess.run(
    ["git", "rev-parse", "HEAD"],
    cwd=source,
    check=True,
    capture_output=True,
    text=True,
).stdout.strip()
try:
    require_matching_source_sha(provenance.get("git_sha"), source_sha)
except RuntimeError as exc:
    raise SystemExit(str(exc))
print(f"setup_release_311: wheel provenance matches HEAD {source_sha}")
PY
}

setup_release() {
  local wheel=${CINDERX_TEST_WHEEL:-}
  local wheel_dir="$RUN_DIR/release-wheels"
  case "$PIPELINE_MODE" in
    pr)
      if [ -n "$wheel" ]; then
        echo "PR mode rejects CINDERX_TEST_WHEEL; the candidate must be built from HEAD"
        return 2
      fi
      ;;
    daily)
      if [ -z "$wheel" ]; then
        echo "Daily mode requires CINDERX_TEST_WHEEL"
        return 2
      fi
      [ -f "$wheel" ] || {
        echo "CINDERX_TEST_WHEEL does not exist: $wheel"
        return 2
      }
      verify_daily_wheel_source "$wheel"
      ;;
    *)
      echo "CINDERX_CP311_PIPELINE_MODE must be pr or daily, got: ${PIPELINE_MODE:-<unset>}"
      return 2
      ;;
  esac
  pip_args
  if [ "$PIPELINE_MODE" = "daily" ]; then
    echo "setup_release_311: using CINDERX_TEST_WHEEL=$wheel"
  else
    rm -rf "$wheel_dir"
    mkdir -p "$wheel_dir"
    "$TEST_PYTHON" -m pip install "${PIP_ARGS[@]}" --upgrade \
      'setuptools>=77.0.3' wheel
    CMAKE_BUILD_TYPE=Release CMAKE_BUILD_PARALLEL_LEVEL="$BUILD_JOBS" \
      "$TEST_PYTHON" -m pip wheel . -w "$wheel_dir" \
        --no-cache-dir --no-deps --no-build-isolation
    wheel=$(find "$wheel_dir" -maxdepth 1 -name 'cinderx-*.whl' \
      -type f -print -quit)
    [ -n "$wheel" ] || { echo "Release wheel was not produced"; return 1; }
    grep -qxs 'CMAKE_BUILD_TYPE:STRING=Release' scratch/*/CMakeCache.txt || {
      echo "the PR wheel was not built with CMAKE_BUILD_TYPE=Release"
      grep -h '^CMAKE_BUILD_TYPE' scratch/*/CMakeCache.txt || true
      return 1
    }
    echo "setup_release_311: built Release wheel $wheel"
  fi

  rm -rf "$VENV"
  "$TEST_PYTHON" -m venv "$VENV"
  "$PIP" install "${PIP_ARGS[@]}" --force-reinstall "$wheel"
  "$PIP" install "${PIP_ARGS[@]}" pytest==9.0.3
  printf '%s\n' "$wheel" > "$RUN_DIR/cp311-tested-wheel.txt"
}

import_dynsym_smoke() {
  "$PYTHON" - <<'PY'
import _cinderx
import cinderx

print("cinderx", cinderx.__file__)
print("_cinderx", _cinderx.__file__)
PY
  local extension
  extension=$("$PYTHON" -c 'import _cinderx; print(_cinderx.__file__)')
  nm -D --defined-only "$extension" | sed 's/.* //' \
    | grep -vx 'PyInit__cinderx' > "$RUN_DIR/dynsym-extra.txt" || true
  if [ -s "$RUN_DIR/dynsym-extra.txt" ]; then
    echo "unexpected dynamic exports:"
    cat "$RUN_DIR/dynsym-extra.txt"
    return 1
  fi
}

test_kunpeng() {
  # PR owns the focused CPython 3.11 wheel contract.  The directory also
  # contains OSR, plugin-startup and cross-version setup-policy suites that
  # require their own runner environments; Daily covers those through the
  # official full test_cinderx off-to-shadow orchestration below.
  (cd "$REPO_ROOT/cinderx/PythonLib" && "$PYTHON" -m unittest \
    test_cinderx.test_kunpeng.test_interpreter_311 \
    test_cinderx.test_kunpeng.test_jit_unsupported_311 \
    test_cinderx.test_kunpeng.test_trigger_stats_311 \
    test_cinderx.test_kunpeng.test_execution_infra_311 \
    test_cinderx.test_kunpeng.test_observe_311 \
    test_cinderx.test_kunpeng.test_canary_execute_311 \
    test_cinderx.test_kunpeng.test_execute_311 \
    test_cinderx.test_kunpeng.test_attr_cache_method_peek_311 \
    test_cinderx.test_kunpeng.test_attr_cache_new_shape_load_311 \
    test_cinderx.test_kunpeng.test_attr_cache_new_shape_store_311 \
    test_cinderx.test_kunpeng.test_early_quicken_311)
}

generator_refcount_matrix() {
  mkdir -p "$RUN_DIR/generator-corpus" "$RUN_DIR/rcm-execute"
  "$PYTHON" ci_pipeline/jit311/refcount_matrix.py ci_pipeline/jit311 \
    corpus_generators interp "$RUN_DIR/generator-corpus/interp.json"
  CINDERX_JIT_MODE=canary PYTHONJITGENERATOR=1 PYTHONMALLOC=debug \
    "$PYTHON" ci_pipeline/jit311/refcount_matrix.py ci_pipeline/jit311 \
      corpus_generators jit "$RUN_DIR/generator-corpus/jit.json"
  "$PYTHON" ci_pipeline/jit311/refcount_matrix.py diff \
    "$RUN_DIR/generator-corpus/interp.json" \
    "$RUN_DIR/generator-corpus/jit.json"
  "$PYTHON" ci_pipeline/jit311/refcount_matrix.py ci_pipeline/jit311 \
    corpus_execute_min interp "$RUN_DIR/rcm-execute/interp.json"
  CINDERX_JIT_MODE=execute PYTHONJITAUTO=20 PYTHONMALLOC=debug \
    "$PYTHON" ci_pipeline/jit311/refcount_matrix.py ci_pipeline/jit311 \
      corpus_execute_min auto "$RUN_DIR/rcm-execute/auto.json"
  "$PYTHON" ci_pipeline/jit311/refcount_matrix.py diff \
    "$RUN_DIR/rcm-execute/interp.json" "$RUN_DIR/rcm-execute/auto.json" 11
}

bytecode_support() {
  "$PYTHON" -m pytest -q ci_pipeline/test_check_bytecode_support_311.py \
    ci_pipeline/test_bytecode_support_311_registry.py
  "$PYTHON" ci_pipeline/check_bytecode_support_311.py
}

lifecycle_census() {
  CINDERX_JIT_MODE=execute PYTHONJITAUTO=20 "$PYTHON" \
    ci_pipeline/jit311/lifecycle_census.py --rounds 6 \
      --out "$RUN_DIR/lifecycle-census.json"
}

non_libtest_drivers() {
  "$PYTHON" -m ci_pipeline.jit311.runners --non-libtest
}

driver_selftests() {
  "$PYTHON" -m pytest -q \
    ci_pipeline/test_jit311_runners.py \
    ci_pipeline/test_jit311_corpus.py \
    ci_pipeline/test_jit311_shadow_surface.py \
    ci_pipeline/test_jit311_lifecycle_census.py \
    ci_pipeline/test_execution_report.py \
    ci_pipeline/test_jit311_report_reasons.py \
    ci_pipeline/test_lifecycle_acceptance.py \
    ci_pipeline/test_quickened_artifact.py \
    ci_pipeline/test_runtime_transition_report.py \
    ci_pipeline/test_semantic_naming.py \
    ci_pipeline/test_shutdown_stability.py \
    ci_pipeline/test_libtest_diff_311.py \
    ci_pipeline/test_run_gate.py
}

test_release_daily() {
  run_step_continue asan_runtime_tests ci_pipeline/scripts/run_asan_build_311.sh \
    "$RUN_DIR/asan-build"
  run_step_continue debug_build ci_pipeline/scripts/run_debug_build_311.sh \
    "$RUN_DIR/debug-build"
  run_step_continue stdlib_execute_canary_72 "$PYTHON" \
    -m ci_pipeline.jit311.runners --stdlib-canary
  run_step_continue test_cinderx_off_to_shadow "$PYTHON" \
    -m ci_pipeline.jit311.runners --shadow-surface --skip-libtest \
    --out "$RUN_DIR/test-cinderx-shadow"
  finish_daily_stage
}

require_libtest() {
  "$PYTHON" -c 'import test, test.test_threading; print(test.__file__)' || {
    echo "candidate Python cannot import the configured Lib/test tree: $PYTHON"
    return 2
  }
}

libtest_execute_72() {
  require_libtest
  run_step execute_72 "$PYTHON" ci_pipeline/libtest_diff_311.py \
    execute-gate --jobs "$BUILD_JOBS" \
    --out "$RUN_DIR/libtest-execute-local"
}

libtest_daily() {
  require_libtest
  run_step_continue stock_evaluator_off_440 "$PYTHON" \
    ci_pipeline/libtest_diff_311.py off-gate --jobs "$BUILD_JOBS" \
    --out "$RUN_DIR/libtest-off"
  run_step_continue execute_72 "$PYTHON" ci_pipeline/libtest_diff_311.py \
    execute-gate --jobs "$BUILD_JOBS" \
    --stock-dir "$RUN_DIR/libtest-off/stock" \
    --out "$RUN_DIR/libtest-execute"
  run_step_continue pydebug_refleak_10 ci_pipeline/scripts/run_refleak_311.sh \
    "$RUN_DIR/refleak"
  run_step_continue unified_report unified_libtest_report
  finish_daily_stage
}

unified_libtest_report() {
  local base="$RUN_DIR/libtest-daily-trigger-summary.json"
  "$PYTHON" -m ci_pipeline.jit311.runners \
    --unify "$RUN_DIR/libtest-off/trigger_report_fields.json" -o "$base"
  "$PYTHON" - "$RUN_DIR" <<'PY'
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])

def load(relative):
    path = root / relative
    if not path.is_file():
        raise SystemExit(f"unified report input missing: {path}")
    return json.loads(path.read_text())

summary = load("libtest-daily-trigger-summary.json")
summary.update(
    {
        "libtest_440": load("libtest-off/off-gate-report.json"),
        "execute_72": load("libtest-execute/diff.json"),
        "pydebug_refleak_10": load("refleak/refleak-summary.json"),
        "artifacts": {
            "stock_vs_evaluator_off": (
                "libtest-off/stock-vs-evaluator-off.json"
            ),
            "execute_diff": "libtest-execute/diff.json",
            "refleak_log": "refleak/regrtest.log",
        },
    }
)
(root / "libtest-daily-unified-report.json").write_text(
    json.dumps(summary, indent=1, sort_keys=True) + "\n"
)
PY
}

cd "$REPO_ROOT"
case "$STAGE" in
  setup_release)
    run_step provision_release_candidate setup_release
    require_candidate
    run_step import_dynsym_smoke import_dynsym_smoke
    ;;
  test_release)
    require_candidate
    run_step test_kunpeng test_kunpeng
    run_step generator_refcount_matrix generator_refcount_matrix
    run_step bytecode_support bytecode_support
    run_step lifecycle_census lifecycle_census
    run_step driver_selftests driver_selftests
    run_step non_libtest_drivers non_libtest_drivers
    ;;
  libtest_execute_72)
    require_candidate
    libtest_execute_72
    ;;
  test_release_daily)
    require_candidate
    test_release_daily
    ;;
  libtest_daily)
    require_candidate
    libtest_daily
    ;;
  *)
    echo "unknown CPython 3.11 stage: $STAGE" >&2
    exit 2
    ;;
esac
