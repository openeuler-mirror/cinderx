#!/usr/bin/env bash
# Prove the cp311 wheel runs on a STOCK openEuler 24.03-LTS-SP3 image.
#
# The runtime container carries the distro python3 rpm plus pip and nothing
# else while the wheel's load proof runs -- no compiler, no dev headers,
# none of the dev image's toolchain paths.  python3-devel is installed only
# afterwards, strictly as a test-harness dependency for the canary evidence
# suite.  The repo is not mounted either: only the test_cinderx test package
# is, so every cinderx import in the kunpeng unittests resolves from the
# installed wheel rather than a source checkout.
#
# Mounts (provided by ci_pipeline/build_cp311_wheel.py):
#   /wheels             wheel directory, read-only
#   /smoke/run.sh       this script
#   /smoke/test_cinderx the unittest package, read-only
set -Eeuo pipefail

export PIP_DISABLE_PIP_VERSION_CHECK=1
export PYTHONUNBUFFERED=1

# Narrowest-version phase: the smoke runs on exactly the anchored
# interpreter build, not whatever python3 rebuild the stock image happens
# to ship.  The stock image also ships no pip.
PYTHON3_NVR="${CINDERX_PYTHON3_NVR:-3.11.6-34.oe2403sp3}"
# Base runtime only: the wheel's load proof (Layer 1 below) must run before
# any development package touches the image, or the proof stops meaning
# "needs only the base runtime".  python3-devel arrives later, after Layer 1,
# and only for the test harness.
dnf install -y -q "python3-${PYTHON3_NVR}" python3-pip
# Whole-string compare: a substring match would accept an NVR suffix rebuild.
test "$(rpm -q --queryformat '%{NAME}-%{VERSION}-%{RELEASE}' python3)" = "python3-${PYTHON3_NVR}"
python3 -c "import sys; assert sys.version_info[:3] == (3, 11, 6), sys.version"

# Globbing, not find(1): the stock image is minimal and has no findutils.
# Scoped to the cp311 tag: the release wheelhouse also carries the cp314
# fat wheel, and a bare glob would sort onto it.
wheels=(/wheels/cinderx-*-cp311-*.whl)
wheel="${wheels[-1]}"
test -f "$wheel"
echo "[cp311-smoke] wheel: ${wheel}"

# Hash-checking mode against the driver-written requirements file, so the
# smoke also proves the sidecar hash matches what actually installs.
reqs=(/wheels/cinderx-*-cp311-*.requirements.txt)
python3 -m pip install --no-index --no-deps --require-hashes -r "${reqs[-1]}"

# Layer 1: the native module loads on the BASE runtime -- this is where a
# missing GLIBCXX version or a dev-only dependency would surface -- and
# reports the expected provenance.  The layer proves its own premise first:
# python3-devel must be absent, and importing a devel-provided module must
# fail, so a quietly polluted image cannot fake the base-runtime claim.
if rpm -q python3-devel > /dev/null 2>&1; then
    echo "[cp311-smoke] FAIL: python3-devel present before the load proof" >&2
    exit 1
fi
python3 - <<'PY'
import sys
import _cinderx
import cinderx

try:
    import _testcapi  # noqa: F401
except ModuleNotFoundError:
    pass
else:
    raise AssertionError(
        "_testcapi importable before python3-devel: the base-runtime "
        "premise of this layer does not hold")

print("[cp311-smoke] python", sys.version.split()[0])
print("[cp311-smoke] _cinderx at", _cinderx.__file__)
assert "site-packages" in _cinderx.__file__, _cinderx.__file__
print("[cp311-smoke] base-runtime load proof ok")
PY

# The harness dependency arrives only now: python3-devel (same NVR) ships
# the release-ABI _testcapi/_testinternalcapi that the canary evidence
# suite drives pending calls and PEP 523 with.  The wheel itself was
# already proven above without it.
dnf install -y -q "python3-devel-${PYTHON3_NVR}"
python3 -c "import _testcapi, _testinternalcapi"

# Layer 2: the full 3.11 unit suite (interpreter take-over, JIT-disabled
# gate, observe mode) against the installed wheel.
cd /smoke
python3 -m unittest -v \
    test_cinderx.test_kunpeng.test_interpreter_311 \
    test_cinderx.test_kunpeng.test_jit_unsupported_311 \
    test_cinderx.test_kunpeng.test_observe_311 \
    test_cinderx.test_kunpeng.test_canary_execute_311

echo "[cp311-smoke] PASS"
