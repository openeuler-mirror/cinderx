#!/usr/bin/env bash
# Builder preflight for the cp311 release wheel. Refuses to build unless the
# image contains source-built CPython 3.11.6 compiled with system GCC 12,
# GCC 14 for CinderX, cmake, a sufficiently new setuptools, and the static
# libstdc++ archive required by the self-contained wheel link.
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

PYTHON=$(resolve_executable python3.11)
CC=$(resolve_executable "${CC:-gcc}")
CXX=$(resolve_executable "${CXX:-g++}")
export PYTHON CC CXX

"$PYTHON" - <<'PY'
import pathlib
import subprocess
import sys
import sysconfig

assert sys.version_info[:3] == (3, 11, 6), sys.version
assert pathlib.Path(sys.executable).resolve() == pathlib.Path(
    "/usr/local/cpython-3.11.6/bin/python3.11"
).resolve(), sys.executable
config_args = sysconfig.get_config_var("CONFIG_ARGS") or ""
assert "--prefix=/usr/local/cpython-3.11.6" in config_args, config_args
assert "CC=/usr/bin/gcc" in config_args, config_args
assert "LDFLAGS=-Wl,-Bsymbolic-functions" in config_args, config_args
assert sysconfig.get_config_var("Py_ENABLE_SHARED") == 1
assert subprocess.check_output(
    ["/usr/bin/gcc", "-dumpfullversion"], text=True
).split(".")[0] == "12"
assert list(pathlib.Path("/usr/local/cpython-3.11.6/lib").glob("libpython3.11*.so*"))
print(sys.version)
print(f"CONFIG_ARGS={config_args}")
PY

system_gcc_version=$(/usr/bin/gcc -dumpfullversion)
case "$system_gcc_version" in
  12.*) echo "CPython gcc ${system_gcc_version}" ;;
  *) echo "expected system GCC 12.x, got ${system_gcc_version}" >&2; exit 1 ;;
esac

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

cmake --version | sed -n '1p'

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
