#!/usr/bin/env bash
# Builder preflight for the cp311 release wheel.  This script is intentionally
# stricter than PR/dev builds: the deliverable is anchored to the distro
# CPython NVR and the GCC 14 toolset in the openEuler 24.03-LTS-SP3 image.
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

PYTHON3_NVR=3.11.6-34.oe2403sp3
test "$(rpm -q --queryformat '%{NAME}-%{VERSION}-%{RELEASE}' python3)" = \
  "python3-${PYTHON3_NVR}"
test "$(rpm -q --queryformat '%{NAME}-%{VERSION}-%{RELEASE}' python3-devel)" = \
  "python3-devel-${PYTHON3_NVR}"

PYTHON=$(resolve_executable python3.11)
CC=$(resolve_executable gcc)
CXX=$(resolve_executable g++)
export PYTHON CC CXX
"$PYTHON" -c "import sys; assert sys.version_info[:3] == (3, 11, 6), sys.version"

cc_version=$("$CC" -dumpfullversion)
cxx_version=$("$CXX" -dumpfullversion)
case "$cc_version" in
  14.*) ;;
  *) echo "expected GCC 14.x, got ${cc_version}" >&2; exit 1 ;;
esac
case "$cxx_version" in
  14.*) ;;
  *) echo "expected G++ 14.x, got ${cxx_version}" >&2; exit 1 ;;
esac
test "${cc_version%%.*}" = "${cxx_version%%.*}" || {
  echo "CC/CXX major mismatch: gcc ${cc_version}, g++ ${cxx_version}" >&2
  exit 1
}
echo "gcc ${cc_version}; g++ ${cxx_version}"

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
