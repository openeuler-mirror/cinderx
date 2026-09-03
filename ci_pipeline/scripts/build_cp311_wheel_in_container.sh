#!/usr/bin/env bash
# Build the ordinary CPython 3.11 CinderX wheel inside the release builder
# image (cinderx-cp311-builder, gcc-toolset-14 based).
#
# Deliberately NOT a fat/manylinux wheel: the 3.11 product targets exactly
# the anchored openEuler 24.03-LTS-SP3 environment (distro python3-3.11.6
# rpm), so one plain cp311-cp311-linux_aarch64 wheel built against the
# distro python is the whole contract.  Runnability on a stock openEuler
# image is proven by scripts/smoke_cp311_wheel_in_runtime.sh, and the
# normalize step enforces the deterministic-zip and dependency contracts.
#
# Mounts (provided by ci_pipeline/build_cp311_wheel.py):
#   /src  read-only source checkout
#   /out  wheel output directory (logs land in /out/logs)
set -Eeuo pipefail
set -x

export PIP_DISABLE_PIP_VERSION_CHECK=1
export PYTHONUNBUFFERED=1
export CMAKE_BUILD_TYPE=Release
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"
export CINDERX_VERSION_PATCH="${CINDERX_VERSION_PATCH:-0}"

resolve_executable() {
  local candidate=$1
  if [[ "$candidate" == */* ]]; then
    test -x "$candidate" || {
      echo "executable not found in builder container: $candidate" >&2
      return 1
    }
    printf '%s\n' "$candidate"
  else
    command -v "$candidate"
  fi
}

PYTHON=$(resolve_executable python3.11)
CC=$(resolve_executable gcc)
CXX=$(resolve_executable g++)
export PYTHON CC CXX

# Static C++ runtime.  The stock openEuler image carries the GCC 12 system
# libstdc++ (GLIBCXX up to 3.4.30) while this GCC 14 build references
# GLIBCXX_3.4.31, so a dynamic link would strand the wheel; linking
# statically makes it self-contained, and the linker version script keeps
# every such symbol local, so the dynamic-symbol allowlist is unaffected.
# normalize_cp311_wheel.py enforces the resulting NEEDED contract.
export LDFLAGS="-static-libstdc++"

# TLS verification for dependency fetches stays ON by default; hosts behind
# TLS-intercepting intranet proxies opt down explicitly, and the downgrade
# is scoped to this process rather than baked into the image.  The explicit
# re-enable also overrides the global downgrade that older builder images
# baked in.  Dependency identity is carried by commit-SHA pins either way.
if [ "${CINDERX_GIT_INSECURE:-0}" = "1" ]; then
  export GIT_SSL_NO_VERIFY=1
else
  git config --system http.sslVerify true
fi

test -d /src
mkdir -p /out /out/logs /work

# The checked-out tree's preflight is authoritative -- the copy baked into
# the image only guards image builds and goes stale as the tree evolves.
bash /src/ci_pipeline/scripts/check_cpython_311_build.sh

{
  printf 'python=%s\n' "$PYTHON"
  "$PYTHON" -VV
  printf 'cc=%s\n' "$CC"
  "$CC" --version | sed -n '1p'
  printf 'cxx=%s\n' "$CXX"
  "$CXX" --version | sed -n '1p'
} > /out/logs/toolchain-311.txt

# Interpreter build-config snapshot, mirroring the cp314 flow's evidence.
"$PYTHON" - <<'PY' > /out/logs/cpython-311-build.jsonl
import json
import sys
import sysconfig

keys = ["CONFIG_ARGS", "EXT_SUFFIX", "Py_DEBUG", "SOABI"]
print(json.dumps({
    "executable": sys.executable,
    "sys_version": sys.version,
    "config": {key: sysconfig.get_config_var(key) for key in keys},
}, sort_keys=True))
PY

# /src stays pristine: build from a copy, with any stray local build state
# from the shipped tree dropped before it can leak into the wheel.
rm -rf /work/src
mkdir -p /work/src
cp -a /src/. /work/src/
cd /work/src
rm -rf scratch build dist wheelhouse ./*.egg-info

"$PYTHON" -m pip wheel --no-build-isolation --no-deps --no-cache-dir -w /out .

# Scoped to the cp311 tag: /out is the shared release wheelhouse and may
# already hold the cp314 fat wheel.
wheel=$(find /out -maxdepth 1 -type f -name 'cinderx-*-cp311-*.whl' | sort | tail -n 1)
test -n "$wheel"
echo "[cp311-wheel] BUILT ${wheel}"
sha256sum "$wheel" | tee /out/logs/ordinary.sha256

"$PYTHON" /src/ci_pipeline/scripts/normalize_cp311_wheel.py \
  --wheel "$wheel" \
  --git-sha "${CINDERX_GIT_SHA:-unknown}" \
  --builder-image "${CINDERX_BUILDER_IMAGE:-unknown}"

echo "[cp311-wheel] NORMALIZED ${wheel}"
sha256sum "$wheel" | tee /out/logs/normalized.sha256
