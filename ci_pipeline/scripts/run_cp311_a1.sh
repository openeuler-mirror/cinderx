#!/usr/bin/env bash
set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../.." && pwd)"
python_bin="${A1_PYTHON:-python3.11}"

exec "${python_bin}" "${repo_root}/ci_pipeline/jit311/a1_runner.py" "$@"
