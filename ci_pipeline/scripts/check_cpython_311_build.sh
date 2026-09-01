#!/usr/bin/env bash
# Builder preflight for the cp311 release wheel. Refuses to build unless the
# selected CPython 3.11.6 and compiler toolchain are actually present.
set -Eeuo pipefail

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

PYTHON=$(resolve_executable "${CINDERX_CP311_PYTHON:-python3.11}")
CC=$(resolve_executable "${CC:-gcc}")
CXX=$(resolve_executable "${CXX:-g++}")
export PYTHON CC CXX
"$PYTHON" -c "import sys; assert sys.version_info[:3] == (3, 11, 6), sys.version"

gcc_version=$("$CXX" -dumpfullversion)
case "$gcc_version" in
  12.*|14.*) echo "g++ ${gcc_version}" ;;
  *) echo "expected GCC 12.x or 14.x, got ${gcc_version}" >&2; exit 1 ;;
esac

cmake --version | head -n 1

# Actually link once, with the release link mode: version strings alone
# missed a toolset packaging gap where the compiler installed without its
# own libgcc_s (the gcc rpm does not require gcc-toolset-14-libgcc).
probe=$(mktemp -d)
echo 'int main() { return 0; }' > "${probe}/probe.cc"
"$CXX" -static-libstdc++ "${probe}/probe.cc" -o "${probe}/probe"
"${probe}/probe"
rm -rf "$probe"

"$PYTHON" - <<'PY'
import importlib.metadata as metadata

version = metadata.version("setuptools")
major = int(version.split(".", 1)[0])
assert major >= 77, f"setuptools too old for pyproject license metadata: {version}"
print(f"setuptools {version}")
PY

# -static-libstdc++ silently degrades to dynamic linking when the archive
# is missing; assert it exists instead of finding out in the smoke.
test -f "$("$CXX" -print-file-name=libstdc++.a)"

echo "[check-cpython-311-build] OK"
