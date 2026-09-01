import json
from pathlib import Path
import subprocess

import ci_pipeline.run_gate as run_gate


def test_pr_pipeline_selects_cp311_gate_for_python311():
    assert run_gate.pipeline_for_python("pr", (3, 11)) == (
        ("cp311_gate", False),
    )


def test_pr_pipeline_keeps_existing_suites_for_python314():
    assert run_gate.pipeline_for_python("pr", (3, 14)) == (
        ("runtime", True),
        ("cinderx_local", False),
    )


def test_pr_pipeline_rejects_unsupported_python():
    try:
        run_gate.pipeline_for_python("pr", (3, 13))
    except ValueError as exc:
        assert "does not support Python 3.13" in str(exc)
    else:
        raise AssertionError("unsupported Python version was accepted")


def test_daily_pipeline_selects_cp311_gate_and_daily_for_python311():
    assert run_gate.pipeline_for_python("daily", (3, 11)) == (
        ("cp311_gate", False),
        ("cp311_daily", False),
    )


def test_daily_pipeline_keeps_existing_suites_for_python314():
    assert run_gate.pipeline_for_python("daily", (3, 14)) == (
        ("runtime", True),
        ("cinderx_local", False, {"CINDERX_LOCAL_RUN_LIBTEST": "1"}),
    )


def test_daily_pipeline_rejects_unsupported_python():
    try:
        run_gate.pipeline_for_python("daily", (3, 13))
    except ValueError as exc:
        assert "pipeline daily does not support Python 3.13" in str(exc)
    else:
        raise AssertionError("unsupported Python version was accepted")


def test_legacy_daily311_pipeline_remains_available():
    assert run_gate.pipeline_for_python("daily311", (3, 14)) == (
        ("cp311_gate", False),
        ("cp311_daily", False),
    )


def test_daily_compatibility_fanout_is_python314_only():
    assert not run_gate.daily_compat_enabled("daily", (3, 11))
    assert run_gate.daily_compat_enabled("daily", (3, 14))
    assert not run_gate.daily_compat_enabled("daily311", (3, 14))
    assert not run_gate.daily_compat_enabled("pr", (3, 14))


def test_non_versioned_pipeline_is_not_version_dispatched():
    assert run_gate.pipeline_for_python("ref314", (3, 11)) == (
        ("cp314_reference", False),
    )


def test_cp311_pr_suite_wires_the_acceptance_surface_by_phase():
    jobs = run_gate.load_suite("cp311_gate")["jobs"]
    jobs_by_name = {job["name"]: job for job in jobs}

    assert len(jobs) == 3
    runtime_job = jobs_by_name["runtime_tests_311"]
    assert "run_rt311_green.sh" in runtime_job["command"]
    assert '"{run_dir}/rt311-build" --census' in runtime_job["command"]
    assert "RT311_BASELINE_BASE" in runtime_job["command"]
    assert set(jobs_by_name) == {
        "runtime_tests_311",
        "setup_release_311",
        "test_release_311",
    }
    assert "sha256sum --check" in runtime_job["command"]
    assert "setup_release" in jobs_by_name["setup_release_311"]["command"]
    assert "test_release" in jobs_by_name["test_release_311"]["command"]

    assert [
        phase["name"]
        for phase in run_gate.summarize_phases(
            [
                {
                    "name": job["name"],
                    "phase": job["phase"],
                    "returncode": 0,
                }
                for job in jobs
            ]
        )
    ] == ["runtime_tests", "setup_release", "test_release"]


def test_cp311_daily_has_two_incremental_jobs():
    pr_jobs = run_gate.load_suite("cp311_gate")["jobs"]
    daily_jobs = run_gate.load_suite("cp311_daily")["jobs"]
    daily_names = [job["name"] for job in daily_jobs]

    assert len(pr_jobs) + len(daily_jobs) == 5
    assert daily_names == ["test_release_daily_311", "libtest_daily_311"]
    assert all("phase" in job for job in daily_jobs)
    assert {job["phase"] for job in daily_jobs} == {
        "test_release",
        "libtest",
    }


def test_cp311_stage_wrapper_reuses_daily_wheel_and_avoids_duplicate_suites():
    script = (
        Path(run_gate.REPO_ROOT)
        / "ci_pipeline"
        / "scripts"
        / "run_cp311_stage.sh"
    ).read_text()

    assert "CINDERX_TEST_WHEEL" in script
    assert script.count("-m pip wheel") == 1
    build_requirements = (
        '"$TEST_PYTHON" -m pip install "${PIP_ARGS[@]}" --upgrade'
    )
    assert build_requirements in script
    assert "'setuptools>=77.0.3' wheel" in script
    assert script.index(build_requirements) < script.index("-m pip wheel")
    assert "pytest -q test_cinderx/test_kunpeng" not in script
    for module in (
        "test_interpreter_311",
        "test_jit_unsupported_311",
        "test_trigger_stats_311",
        "test_execution_infra_311",
        "test_observe_311",
        "test_canary_execute_311",
        "test_execute_311",
    ):
        assert script.count(module) == 1
    assert "--non-libtest" in script
    assert script.count("libtest_diff_311.py off-gate") == 1
    assert script.count('--stock-dir "$RUN_DIR/libtest-off/stock"') == 1
    assert "libtest-tri" not in script
    assert "evaluator-off-vs-shadow" not in script
    assert "cinderx-test-support.pth" not in script
    assert "import test.test_threading" in script
    assert script.count('--jobs "$BUILD_JOBS"') == 2
    assert "stock_to_evaluator_off" not in script
    assert "evaluator_off_to_shadow" not in script
    assert "--skip-test-cinderx" not in script
    assert "--skip-libtest" in script


def test_python_test_support_is_scoped_to_release_test_jobs():
    base_env = {
        "CINDERX_TEST_PYTHON_STDLIB_DIR": "/opt/python/lib/python3.11",
        "CINDERX_TEST_PYTHON_EXTENSIONS_DIR": (
            "/opt/python/lib/python3.11/lib-dynload"
        ),
        "PYTHONPATH": "/candidate/site-packages",
    }

    test_env = dict(base_env)
    run_gate.configure_python_test_support(
        test_env, {"phase": "test_release"}
    )
    assert test_env["PYTHONPATH"].split(run_gate.os.pathsep) == [
        "/opt/python/lib/python3.11",
        "/opt/python/lib/python3.11/lib-dynload",
        "/candidate/site-packages",
    ]

    setup_env = dict(base_env)
    run_gate.configure_python_test_support(setup_env, {"phase": "setup_release"})
    assert setup_env["PYTHONPATH"] == "/candidate/site-packages"


def test_rt311_runner_honors_gate_python_deps_and_parallelism():
    script = (
        Path(run_gate.REPO_ROOT)
        / "ci_pipeline"
        / "scripts"
        / "run_rt311_green.sh"
    ).read_text()

    assert "CINDERX_RUNTIME_TEST_PYTHON" in script
    assert "TEST_PYTHON=$CINDERX_RUNTIME_TEST_PYTHON" in script
    assert 'FLAGS=$("$TEST_PYTHON" -c' in script
    assert "PYTHON_ROOT=$(\"$TEST_PYTHON\" -c" in script
    assert '-DPython_ROOT_DIR="$PYTHON_ROOT"' in script
    assert '-DPython_EXECUTABLE="$TEST_PYTHON"' in script
    assert "CINDERX_RUNTIME_TEST_PYTHON_INCLUDE_DIR" in script
    assert '-DPython_INCLUDE_DIR="$PYTHON_INCLUDE_DIR"' in script
    assert "CINDERX_RUNTIME_TEST_PYTHON_LIBRARY" in script
    assert '-DPython_LIBRARY="$PYTHON_LIBRARY"' in script
    assert "CINDERX_RUNTIME_TEST_PYTHON_EXTENSIONS_DIR" in script
    assert 'env "${RUNTIME_TEST_ENV[@]}" "$BIN"' in script
    assert "CINDERX_LOCAL_DEPS_DIR=${CINDERX_LOCAL_DEPS_DIR:-$CINDERX_LOCAL_DEPS}" in script
    assert "BUILD_JOBS=${CINDERX_TEST_JOBS:-$(nproc)}" in script
    assert 'make -C "$BUILD_DIR" -j"$BUILD_JOBS" runtime_tests' in script
    assert '2>&1 | tee "$BUILD_DIR-configure.log"' in script
    assert '| tee "$BUILD_DIR-build.log"' in script
    assert "RT311_CENSUS_SHARD_SIZE" in script
    assert 'split -d -a 4 -l "$CENSUS_SHARD_SIZE"' in script
    assert 'if [ "$CENSUS_RAN" != "$CENSUS_EXPECTED" ]' in script


def test_cp311_daily_build_scripts_honor_runner_resources_and_offline_inputs():
    scripts_dir = Path(run_gate.REPO_ROOT) / "ci_pipeline" / "scripts"
    debug_script = (scripts_dir / "run_debug_build_311.sh").read_text()
    asan_script = (scripts_dir / "run_asan_build_311.sh").read_text()
    refleak_script = (scripts_dir / "run_refleak_311.sh").read_text()

    for script in (debug_script, asan_script):
        assert "CINDERX_TEST_PYTHON" in script
        assert "CINDERX_TEST_JOBS" in script
        assert "CINDERX_LOCAL_DEPS" in script
        assert "CINDERX_TEST_PYTHON_INCLUDE_DIR" in script
        assert "CINDERX_TEST_PYTHON_LIBRARY" in script

    assert "CINDERX_RUNTIME_TEST_PYTHON" not in debug_script
    assert "CINDERX_RUNTIME_TEST_PYTHON" in asan_script
    assert "TEST_PYTHON=$CINDERX_RUNTIME_TEST_PYTHON" in asan_script

    assert "RUNTIME_TEST_ENV" in asan_script
    assert 'env "${RUNTIME_TEST_ENV[@]}" "$BIN"' in asan_script
    assert 'EXEC_DIR="$BUILD_DIR"' in asan_script
    assert 'BUILD_DIR-exec' not in asan_script
    assert asan_script.count("cmake -S") == 1
    assert 'make -C "$EXEC_DIR"' in asan_script
    assert "asan_canary_smoke.py" not in asan_script

    assert "CINDERX_TEST_JOBS" in refleak_script
    assert "CINDERX_REFLEAK_PYTHON_SOURCE" in refleak_script
    assert "CINDERX_PIP_WHEELHOUSE" in refleak_script
    assert "--no-index" in refleak_script
    assert "THRESHOLD=${PYTHONJITAUTO:-20}" in refleak_script
    assert "ROUNDS=${CINDERX_REFLEAK_ROUNDS:-10:3}" in refleak_script
    assert 'for MODULE in "${MODULES[@]}"' in refleak_script
    assert '"$VENV_PY" -m test -R "$ROUNDS" "$MODULE"' in refleak_script
    assert 'FAILED_MODULES+=("$MODULE")' in refleak_script
    assert "comm -3" in refleak_script
    assert '--original-log-dir "$MODULE_LOG_DIR"' in refleak_script
    assert "any global SUCCESS epilogue" in refleak_script


def test_cp311_container_scripts_resolve_bare_executable_names():
    scripts_dir = Path(run_gate.REPO_ROOT) / "ci_pipeline" / "scripts"
    scripts = [
        (scripts_dir / "build_cp311_wheel_in_container.sh").read_text(),
        (scripts_dir / "check_cpython_311_build.sh").read_text(),
        (scripts_dir / "smoke_cp311_wheel_in_runtime.sh").read_text(),
    ]

    for script in scripts:
        assert "resolve_executable()" in script
        assert 'command -v "$candidate"' in script
        assert '[[ "$candidate" == */* ]]' in script

    builder = scripts[0]
    assert 'CC=$(resolve_executable "${CC:-gcc}")' in builder
    assert 'CXX=$(resolve_executable "${CXX:-g++}")' in builder


def test_autojit_zero_threshold_is_only_rejected_on_cp311():
    source = (Path(run_gate.REPO_ROOT) / "cinderx" / "Jit" / "pyjit.cpp").read_text()

    helper = source[source.index("bool validAutoJitThreshold") :]
    helper = helper[: helper.index("bool parseAutoJitOption")]
    assert "#if PY_VERSION_HEX < 0x030C0000" in helper
    assert "return threshold > 0;" in helper
    assert "#else\n  return true;" in helper


def test_configure_toolchain_prefers_target_python_compilers(monkeypatch):
    env = {"CINDERX_TEST_PYTHON": "/usr/local/cpython-3.14.3/bin/python3.14"}

    def fake_run(cmd, **kwargs):
        assert cmd[0] == env["CINDERX_TEST_PYTHON"]
        return subprocess.CompletedProcess(
            cmd,
            0,
            stdout=json.dumps({"CC": "gcc", "CXX": "g++"}),
            stderr="",
        )

    def fake_first_executable(candidates, extra_globs):
        if candidates[0] == "gcc-14":
            return "/usr/local/bin/gcc-14"
        if candidates[0] == "g++-14":
            return "/usr/local/bin/g++-14"
        return None

    monkeypatch.setattr(run_gate.subprocess, "run", fake_run)
    monkeypatch.setattr(run_gate, "first_executable", fake_first_executable)

    run_gate.configure_toolchain(env)

    assert env["CC"] == "gcc"
    assert env["CXX"] == "g++"


def test_configure_toolchain_keeps_explicit_compilers(monkeypatch):
    env = {
        "CINDERX_TEST_PYTHON": "/usr/local/cpython-3.14.3/bin/python3.14",
        "CC": "/custom/gcc",
        "CXX": "/custom/g++",
    }

    def fail_run(*args, **kwargs):
        raise AssertionError("target Python should not be queried")

    monkeypatch.setattr(run_gate.subprocess, "run", fail_run)

    run_gate.configure_toolchain(env)

    assert env["CC"] == "/custom/gcc"
    assert env["CXX"] == "/custom/g++"


def test_runtime_tests_enable_runtime_selectable_frames_on_cpython311(
    monkeypatch,
):
    monkeypatch.setattr(
        run_gate,
        "cinderx_test_python_info",
        lambda _env: {
            "executable": "/usr/bin/python3.11",
            "py_version": "3.11",
            "python_root": "/usr",
            "python_library": "/usr/lib64/libpython3.11.so",
            "meta_python": False,
            "linux": True,
            "mac": False,
        },
    )

    options = run_gate.runtime_tests_cmake_options(
        {"CINDERX_TEST_PYTHON": "/usr/bin/python3.11"}
    )

    assert "-DENABLE_LIGHTWEIGHT_FRAMES=1" in options
