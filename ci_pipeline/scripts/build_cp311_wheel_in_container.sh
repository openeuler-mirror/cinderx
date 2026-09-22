#!/usr/bin/env bash
# Build the ordinary CPython 3.11 CinderX wheel inside the repository's
# CPython 3.11 development image (cinderx-dev:py311).
#
# Deliberately NOT a fat/manylinux wheel: the 3.11 product targets exactly
# the anchored openEuler 24.03-LTS-SP3 environment. The build image uses
# the anchored openEuler CPython 3.11.6 packages, and runnability is proven
# against the same distribution runtime in a stock openEuler image by
# scripts/smoke_cp311_wheel_in_runtime.sh. The normalize step enforces the
# deterministic-zip and dependency contracts.
#
# Mounts (provided by ci_pipeline/build_cp311_wheel.py) default to /src,
# /out and /work.  Daily runs the same builder in-place and overrides those
# directories with CINDERX_CP311_WHEEL_{SOURCE,OUTPUT,WORK}_DIR.
set -Eeuo pipefail
set -x

export PIP_DISABLE_PIP_VERSION_CHECK=1
export PYTHONUNBUFFERED=1
export CMAKE_BUILD_TYPE=Release
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"
export CINDERX_VERSION_PATCH="${CINDERX_VERSION_PATCH:-0}"

SOURCE_DIR=${CINDERX_CP311_WHEEL_SOURCE_DIR:-/src}
OUTPUT_DIR=${CINDERX_CP311_WHEEL_OUTPUT_DIR:-/out}
WORK_DIR=${CINDERX_CP311_WHEEL_WORK_DIR:-/work}
BUILD_REQUIREMENTS="$SOURCE_DIR/ci_pipeline/requirements-cp311-build.txt"

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

test -d "$SOURCE_DIR"
test -f "$BUILD_REQUIREMENTS"
mkdir -p "$OUTPUT_DIR" "$OUTPUT_DIR/logs" "$WORK_DIR"

BUILD_PIP_ARGS=(--disable-pip-version-check --no-cache-dir)
if [ -n "${CINDERX_PIP_WHEELHOUSE:-}" ]; then
  BUILD_PIP_ARGS+=(--no-index --find-links "$CINDERX_PIP_WHEELHOUSE")
fi
"$PYTHON" -m pip install "${BUILD_PIP_ARGS[@]}" -r "$BUILD_REQUIREMENTS"

# The checked-out tree's preflight is authoritative -- the copy baked into
# the image only guards image builds and goes stale as the tree evolves.
bash "$SOURCE_DIR/ci_pipeline/scripts/check_cpython_311_build.sh"

{
  printf 'python=%s\n' "$PYTHON"
  "$PYTHON" -VV
  printf 'cc=%s\n' "$CC"
  "$CC" --version | sed -n '1p'
  printf 'cxx=%s\n' "$CXX"
  "$CXX" --version | sed -n '1p'
} > "$OUTPUT_DIR/logs/toolchain-311.txt"

# Interpreter build-config snapshot, mirroring the cp314 flow's evidence.
"$PYTHON" - <<'PY' > "$OUTPUT_DIR/logs/cpython-311-build.jsonl"
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

# The source stays pristine: build from a copy, with any stray local build state
# from the shipped tree dropped before it can leak into the wheel.
BUILD_SOURCE="$WORK_DIR/src"
rm -rf "$BUILD_SOURCE"
mkdir -p "$BUILD_SOURCE"
if [ "${CINDERX_CP311_WHEEL_TRACKED_SOURCE:-0}" = "1" ]; then
  git -C "$SOURCE_DIR" archive --format=tar HEAD | tar -xf - -C "$BUILD_SOURCE"
else
  cp -a "$SOURCE_DIR"/. "$BUILD_SOURCE"/
fi
cd "$BUILD_SOURCE"
rm -rf scratch build dist wheelhouse ./*.egg-info

"$PYTHON" -m pip wheel --no-build-isolation --no-deps --no-cache-dir -w "$OUTPUT_DIR" .

# Scoped to the cp311 tag: /out is the shared release wheelhouse and may
# already hold the cp314 fat wheel.
wheel=$(find "$OUTPUT_DIR" -maxdepth 1 -type f -name 'cinderx-*-cp311-*.whl' | sort | tail -n 1)
test -n "$wheel"
echo "[cp311-wheel] BUILT ${wheel}"
sha256sum "$wheel" | tee "$OUTPUT_DIR/logs/ordinary.sha256"

"$PYTHON" "$SOURCE_DIR/ci_pipeline/scripts/normalize_cp311_wheel.py" \
  --wheel "$wheel" \
  --git-sha "${CINDERX_GIT_SHA:-unknown}" \
  --builder-image "${CINDERX_BUILDER_IMAGE:-unknown}" \
  --source-dir "$BUILD_SOURCE"

echo "[cp311-wheel] NORMALIZED ${wheel}"
sha256sum "$wheel" | tee "$OUTPUT_DIR/logs/normalized.sha256"
