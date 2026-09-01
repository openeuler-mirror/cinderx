# CinderX Local Test Gate

Chinese documentation: [README_CN.md](README_CN.md)

This directory maintains the local gate flow for ARM64 Linux CPython 3.11 and
3.14 CinderX JIT work. The entry point is `ci_pipeline/run_gate.py`; suite
configuration lives in `ci_pipeline/suites/*.toml`.

## Software Dependencies

Before running `run_gate.py`, prepare the target Python, build toolchain, CMake,
and test tools. `pr --coverage` also needs native coverage tools; `daily` and
compat suites need an external wheel and the Python interpreters listed in the
compatibility matrix.

### Base Dependencies

| Dependency | Purpose |
|---|---|
| `pip`, `venv`, `setuptools >= 77.0.3` | Build and install the local wheel; `pyproject.toml` defines the setuptools lower bound |
| `pytest` | Used by `cinderx_local`, `wheel_compat`, and Lib/test runs |
| `bash`, `coreutils`, `findutils`, `git` | Needed by suite shell commands, source-state checks, and file handling |
| `cmake`, `ctest` | Configure, build, and run the `runtime` suite |
| `make` or another CMake generator backend | `cmake --build` needs an actual build backend; when no generator is set explicitly, this is usually `make` |

### Coverage Dependencies

Running `python3.14 ci_pipeline/run_gate.py pr --coverage` or any native suite
with `--coverage` also requires:

| Dependency | Purpose |
|---|---|
| GCC/G++ | CMake coverage mode only supports GNU compilers |
| `gcov` | Generate native coverage data |
| `lcov` | Collect and filter the coverage tracefile |
| `genhtml` | Generate the HTML coverage report |

Coverage mode cannot be combined with LTO/PGO. `run_gate.py` enables
`ENABLE_COVERAGE=ON` for the `runtime` suite, and CMake rejects non-GCC/gcov
coverage builds.

## Quick Start

### PR Gate

The `pr` entry point selects its suites from the interpreter running it:

```bash
CINDERX_LOCAL_DEPS=/path/to/cinderx-local-deps \
python3.11 ci_pipeline/run_gate.py pr

CINDERX_LOCAL_DEPS=/path/to/cinderx-local-deps \
python3.14 ci_pipeline/run_gate.py pr --coverage
```

Python 3.11 runs three `cp311_gate` jobs: `runtime_tests_311`,
`setup_release_311`, and `test_release_311`. PR builds exactly one Release
wheel, while the full Lib/test differential remains Daily-only. Python 3.14
keeps the existing pipeline and runs in this order:

1. `runtime`: build and run native `RuntimeTests` through CMake. `--coverage`
   applies only to this suite.
2. Coverage post-processing: run `gcov`, `lcov`, and `genhtml`, then check the
   coverage thresholds.
3. `cinderx_local`: build a local release wheel, install it into a temporary
   venv, and run the CinderX Python tests.

If `runtime` or coverage post-processing fails, the pipeline stops before
running later suites.

### Daily Compat Gate

`daily` reuses the main PR gate flow, then fans out the wheel compatibility
matrix:

```bash
CINDERX_LOCAL_DEPS=/path/to/cinderx-local-deps \
CINDERX_TEST_WHEEL=/path/to/cinderx.whl \
python3.14 ci_pipeline/run_gate.py daily
```

Python 3.11 Daily requires the fat wheel. `setup_release_311` installs it
instead of building a second wheel, then Daily appends
`test_release_daily_311` and `libtest_daily_311`:

```bash
CINDERX_LOCAL_DEPS=/path/to/cinderx-local-deps \
CINDERX_TEST_WHEEL=/path/to/cinderx-fat.whl \
python3.11 ci_pipeline/run_gate.py daily
```

`libtest_daily_311` runs stock 440, evaluator-off 440, and shadow 440 once
each, extracts the 72-module control baseline from that stock result, and then
runs only execute 72, the ten-module Py_DEBUG refleak check, and one unified
report. It repeats neither stock 72 nor evaluator-off 440.

The `daily` pipeline runs in this order:

1. `runtime`
2. `cinderx_local`, with `CINDERX_LOCAL_RUN_LIBTEST=1` set automatically so
   Lib/test runs against the locally built wheel
3. `wheel_compat_<name>` entries from `ci_pipeline/python_compat_matrix.toml`
4. `wheel_compat_negative_<name>` entries from the same matrix file

`daily` does not build the external compatibility wheel. Callers must provide
the wheel to test through `CINDERX_TEST_WHEEL`.

### Standalone Suites

Available suites correspond to `ci_pipeline/suites/*.toml`. Use `--list` to
print the jobs for a pipeline or suite without running them:

```bash
python3.11 ci_pipeline/run_gate.py pr --list
python3.14 ci_pipeline/run_gate.py pr --list
python3.14 ci_pipeline/run_gate.py --suite runtime --list
```

Run one suite with `--suite`:

```bash
python3.14 ci_pipeline/run_gate.py --suite runtime --coverage
python3.14 ci_pipeline/run_gate.py --suite cinderx_local
python3.14 ci_pipeline/run_gate.py --suite wheel_compat
python3.14 ci_pipeline/run_gate.py --suite wheel_compat_negative
```

The normal `cinderx_local` suite only builds a local release wheel and runs the
CinderX Python tests. Set this explicitly when Lib/test should run too:

```bash
CINDERX_LOCAL_DEPS=/path/to/cinderx-local-deps \
CINDERX_LOCAL_RUN_LIBTEST=1 \
python3.14 ci_pipeline/run_gate.py --suite cinderx_local
```

## Pipelines And Suites

### Pipelines

| Pipeline | Composition | Use case |
|---|---|---|
| `pr` | `runtime` -> coverage post-processing -> `cinderx_local` | Local pre-submit validation |
| `daily` | `runtime` -> coverage post-processing -> `cinderx_local` + Lib/test -> compat matrix | Full daily compatibility validation |

`--coverage` is only passed to native RuntimeTests suites that are marked for
coverage. Python wheel tests still use a normal release wheel.

### Suites

| Suite | Purpose | Main inputs |
|---|---|---|
| `runtime` | Build and run C++ `RuntimeTests`; can emit native coverage | Optional `--coverage`, `CINDERX_LOCAL_DEPS` |
| `cinderx_local` | Build a local release wheel and run CinderX Python tests | Optional `CINDERX_LOCAL_RUN_LIBTEST=1` |
| `wheel_compat` | Install and test an external wheel on a supported Python | `CINDERX_TEST_WHEEL`, `CINDERX_TEST_PYTHON` |
| `wheel_compat_negative` | Verify an unsupported Python rejects the external wheel | `CINDERX_TEST_WHEEL`, `CINDERX_UNSUPPORTED_TEST_PYTHON` |

The `cinderx_local` build job sets `CINDERX_INCLUDE_TEST_PACKAGE_DATA=1` so
gate-only package data is included only in the local test wheel, not in normal
release wheels.

## Compatibility Matrix

The compatibility matrix is defined in `ci_pipeline/python_compat_matrix.toml`:

- `[[supported]]` entries are used by `wheel_compat`
- `[[unsupported]]` entries are used by `wheel_compat_negative`

Each entry must define:

- `name`
- `python`
- `version`

`daily` creates a separate run directory, venv, logs, and `summary.json` for
each matrix entry. The top-level summary reports each Python version as its own
job.

## Common Environment Variables

| Environment variable | Purpose |
|---|---|
| `CINDERX_TEST_PYTHON` | Python interpreter used by the gate; defaults to the interpreter running `run_gate.py` |
| `CINDERX_TEST_WHEEL` | External wheel tested by `daily` compat fan-out and the `wheel_compat` / `wheel_compat_negative` suites |
| `CINDERX_UNSUPPORTED_TEST_PYTHON` | Unsupported Python interpreter used by `wheel_compat_negative` |
| `CINDERX_LOCAL_RUN_LIBTEST=1` | Makes `cinderx_local` run Lib/test against the local wheel |
| `CINDERX_LOCAL_DEPS` | Local cache directory for CMake FetchContent dependencies |
| `CINDERX_PIP_WHEELHOUSE` | Local Python wheelhouse used to bootstrap suite venvs |
| `CINDERX_PIP_OFFLINE=1` | Requires pip installs to use `CINDERX_PIP_WHEELHOUSE` only |
| `CINDERX_TEST_JOBS` | RuntimeTests CMake/ctest parallelism; defaults to CPU count |
| `CINDERX_TESTGATE_PRELUDE` | Shell snippet run before each job; can also be overridden with `--prelude` |
| `CINDERX_TESTGATE_ALLOW_TARGET_MISMATCH=1` | Allows current machine and suite target mismatches; equivalent to `--allow-target-mismatch` |

For native `RuntimeTests`, the default `CC` and `CXX` come from the target
`CINDERX_TEST_PYTHON`'s `sysconfig` values. This keeps the linker compatible
with the target Python's static `libpython`, including Python builds that embed
GCC LTO bytecode. If you override `CC` or `CXX`, make sure that compiler can
link the target Python library.

## Dependency Cache And Pip Offline Mode

`run_gate` does not hard-code a dependency cache path. Set `CINDERX_LOCAL_DEPS`
explicitly when a host should build offline or reuse pre-populated dependencies:

```bash
export CINDERX_LOCAL_DEPS=/opt/cinderx-deps
```

The cache covers these CMake FetchContent dependencies:

- `fmt`
- `parallel-hashmap`
- `usdt`
- `capstone`
- `googletest`

If a cached dependency is missing or does not match the expected remote and
tag/commit, CMake refreshes that dependency directory. Without
`CINDERX_LOCAL_DEPS`, CMake uses the normal FetchContent behavior for the
current environment.

For Python package bootstrap in suite venvs, use a local wheelhouse:

```bash
export CINDERX_PIP_WHEELHOUSE=/opt/cinderx-pydeps
export CINDERX_PIP_OFFLINE=1
```

`CINDERX_PIP_WHEELHOUSE` must contain at least `pip`, `pytest`, and pytest's
transitive dependencies.

Complete example for an offline ARM64 host:

```bash
export CINDERX_LOCAL_DEPS=/opt/cinderx-deps
export CINDERX_PIP_WHEELHOUSE=/opt/cinderx-pydeps
export CINDERX_PIP_OFFLINE=1

python3.14 ci_pipeline/run_gate.py pr --coverage
```

## Fat Wheel Build Options

The CPython 3.14 manylinux fat wheel builder defaults to release-oriented
settings:

```bash
python3.14 ci_pipeline/build_cp314_manylinux_fat_wheel.py
```

Default build behavior:

- `CMAKE_BUILD_TYPE=Release`
- `CINDERX_ENABLE_PGO=0`
- `CINDERX_ENABLE_LTO=0`

Use explicit flags when a CI job needs different build characteristics:

```bash
python3.14 ci_pipeline/build_cp314_manylinux_fat_wheel.py \
  --cmake-build-type RelWithDebInfo \
  --pgo \
  --lto
```

The host build manifest records the selected CMake build type and whether PGO
or LTO was enabled. The in-container build script follows the same defaults and
only enables PGO/LTO when `CINDERX_ENABLE_PGO` or `CINDERX_ENABLE_LTO` is set to
a non-zero value.

## Coverage, Artifacts, And Known Limits

Coverage thresholds are configured in `COVERAGE_MIN_PERCENT` near the top of
`ci_pipeline/run_gate.py`. They are calibrated for the current runtime-only
coverage scope:

- line: 70%
- function: 60%
- branch: 40%

Each run prints an artifact directory and writes logs, per-job JSON results,
and the top-level `summary.json` there. Coverage runs also generate
`coverage/coverage.info` and `coverage/html/index.html`.

LCOV compatibility is handled at runtime:

- LCOV 1.x uses `lcov_branch_coverage=1`
- LCOV 2.x uses `branch_coverage=1` and downgrades known third-party/template
  consistency issues during capture, filter, and HTML generation

Known exclusions:

- `test_jit_support_instrumentation.py` is filtered to ARM64-supported cases.
- `test_compiler_sbs_stdlib_0.py` through
  `test_compiler_sbs_stdlib_9.py` are tracked as Kunpeng `test_cinderx` debt
  outside the main gate.

Keep HIR runtime test fixture files checked out with LF line endings. CRLF can
make delimiter lines fail parser validation during `runtime_tests`.
