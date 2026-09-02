#!/usr/bin/env bash
set -euo pipefail

launcher=$(basename -- "$0")
jobs=${CINDERX_TEST_JOBS:-4}
export CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-$jobs}
export MAKEFLAGS=${MAKEFLAGS:--j$jobs}
export CINDERX_LOCAL_DEPS=${CINDERX_LOCAL_DEPS:-/opt/cinderx-deps}
export CINDERX_PIP_WHEELHOUSE=${CINDERX_PIP_WHEELHOUSE:-/opt/cinderx-pydeps}
export CINDERX_PIP_OFFLINE=${CINDERX_PIP_OFFLINE:-1}

is_pr_gate=0
args=("$@")
for ((i = 0; i + 1 < ${#args[@]}; i++)); do
  case "${args[i]}" in
    *ci_pipeline/run_gate.py)
      [[ "${args[i + 1]}" == pr ]] && is_pr_gate=1
      ;;
  esac
done

case "$launcher" in
  python3.11)
    python=${CINDERX_DEV_GATE_PYTHON311:-/usr/local/cpython-3.11.6/bin/python3.11}
    export PATH="$(dirname -- "$python"):/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
    export CC=/usr/local/bin/gcc-14
    export CXX=/usr/local/bin/g++-14
    export CINDERX_TEST_PYTHON="$python"
    export CINDERX_TEST_PYTHON_INCLUDE_DIR=/usr/local/cpython-3.11.6/include/python3.11
    export CINDERX_TEST_PYTHON_STDLIB_DIR=/usr/local/cpython-3.11.6/lib/python3.11
    export CINDERX_TEST_PYTHON_EXTENSIONS_DIR=/usr/local/cpython-3.11.6/lib/python3.11/lib-dynload
    export CINDERX_RUNTIME_TEST_PYTHON=/usr/bin/python3.11
    export CINDERX_RUNTIME_TEST_PYTHON_INCLUDE_DIR=/usr/local/cpython-3.11.6/include/python3.11
    export CINDERX_RUNTIME_TEST_PYTHON_LIBRARY=/usr/lib64/libpython3.11.so.1.0
    export CINDERX_RUNTIME_TEST_PYTHON_EXTENSIONS_DIR=/usr/local/cpython-3.11.6/lib/python3.11/lib-dynload
    export RT311_CENSUS_SHARD_SIZE=${RT311_CENSUS_SHARD_SIZE:-100}
    unset CINDERX_ENABLE_LTO

    if ((is_pr_gate)) && [[ -z "${RT311_BASELINE_BASE:-}" ]]; then
      repo=$(git rev-parse --show-toplevel 2>/dev/null) || {
        echo "error: python3.11 PR gate must run inside a Git checkout" >&2
        exit 2
      }
      for ref in upstream/dev origin/dev; do
        if git -C "$repo" rev-parse --verify "$ref^{commit}" >/dev/null 2>&1; then
          RT311_BASELINE_BASE=$(git -C "$repo" merge-base HEAD "$ref")
          export RT311_BASELINE_BASE
          break
        fi
      done
      if [[ -z "${RT311_BASELINE_BASE:-}" ]]; then
        echo "error: cannot determine the CPython 3.11 baseline; fetch upstream/dev or origin/dev" >&2
        exit 2
      fi
      echo "[cinderx-dev-gate] RT311_BASELINE_BASE=$RT311_BASELINE_BASE" >&2
    fi
    ;;
  python3.14)
    python=${CINDERX_DEV_GATE_PYTHON314:-/usr/local/cpython-3.14.3/bin/python3.14}
    export PATH="$(dirname -- "$python"):/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
    export CC=/usr/local/bin/gcc-14
    export CXX=/usr/local/bin/g++-14
    export CINDERX_TEST_PYTHON="$python"
    unset CINDERX_TEST_PYTHON_INCLUDE_DIR
    unset CINDERX_TEST_PYTHON_STDLIB_DIR
    unset CINDERX_TEST_PYTHON_EXTENSIONS_DIR
    unset CINDERX_RUNTIME_TEST_PYTHON
    unset CINDERX_RUNTIME_TEST_PYTHON_INCLUDE_DIR
    unset CINDERX_RUNTIME_TEST_PYTHON_LIBRARY
    unset CINDERX_RUNTIME_TEST_PYTHON_EXTENSIONS_DIR
    unset CINDERX_ENABLE_LTO
    ;;
  *)
    echo "error: unsupported launcher name: $launcher" >&2
    exit 2
    ;;
esac

exec "$python" "$@"
