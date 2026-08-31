import json
from pathlib import Path
import subprocess
import tomllib

import ci_pipeline.run_gate as run_gate


def test_pr_pipeline_routes_python311_to_pr311_gate():
    assert run_gate.resolve_pipeline_name("pr", (3, 11)) == "pr311"


def test_pr_pipeline_keeps_python314_gate():
    assert run_gate.resolve_pipeline_name("pr", (3, 14)) == "pr"


def test_explicit_pipeline_name_is_not_rewritten():
    assert run_gate.resolve_pipeline_name("pr311", (3, 11)) == "pr311"


def test_cp311_wheel_jobs_honor_declared_build_backend():
    suite_path = Path(__file__).parent / "suites" / "cp311_gate.toml"
    with suite_path.open("rb") as suite_file:
        suite = tomllib.load(suite_file)

    wheel_jobs = {
        job["name"]: job
        for job in suite["jobs"]
        if job["name"] in {"wheel_build_import", "release_canary_execute"}
    }

    assert set(wheel_jobs) == {"wheel_build_import", "release_canary_execute"}
    assert all(
        "--no-build-isolation" not in job["command"]
        for job in wheel_jobs.values()
    )
    assert all(
        job.get("env", {}).get("CINDERX_TEST_PREFER_MODERN_COMPILER") == "1"
        for job in wheel_jobs.values()
    )
    assert all(
        job.get("failure_log_tail_lines") == 80 for job in wheel_jobs.values()
    )
    assert all(
        "python3.11 -m pip" not in job["command"]
        and "python3.11 -m venv" not in job["command"]
        for job in wheel_jobs.values()
    )
    assert all(
        job["command"].count("$CINDERX_TEST_PYTHON") >= 2
        for job in wheel_jobs.values()
    )


def test_failure_log_tail_is_line_and_size_bounded(tmp_path):
    log_path = tmp_path / "failed.log"
    log_path.write_text(
        "first\nsecond\n" + ("x" * 12_500) + "\nlast\n",
        encoding="utf-8",
    )

    tail = run_gate.failure_log_tail(log_path, line_limit=2)

    assert tail.endswith("\nlast")
    assert "first" not in tail
    assert len(tail) <= run_gate.FAILURE_LOG_TAIL_MAX_CHARS


def test_configure_toolchain_uses_requested_modern_compilers(monkeypatch):
    env = {
        "CINDERX_TEST_PYTHON": "/usr/local/cpython-3.11.6/bin/python3.11",
        "CINDERX_TEST_PREFER_MODERN_COMPILER": "1",
    }

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
            return "/opt/gcc-14.2/bin/gcc"
        if candidates[0] == "g++-14":
            return "/opt/gcc-14.2/bin/g++"
        return None

    monkeypatch.setattr(run_gate.subprocess, "run", fake_run)
    monkeypatch.setattr(run_gate, "first_executable", fake_first_executable)

    run_gate.configure_toolchain(env)

    assert env["CC"] == "/opt/gcc-14.2/bin/gcc"
    assert env["CXX"] == "/opt/gcc-14.2/bin/g++"


def test_merged_env_applies_job_overrides_before_toolchain(monkeypatch):
    def fake_configure_toolchain(env):
        assert env["CINDERX_TEST_PREFER_MODERN_COMPILER"] == "1"
        env["CC"] = "/opt/gcc-14.2/bin/gcc"

    monkeypatch.setattr(run_gate, "configure_toolchain", fake_configure_toolchain)

    env = run_gate.merged_env(
        {"env": {"CINDERX_TEST_PREFER_MODERN_COMPILER": "1"}}
    )

    assert env["CC"] == "/opt/gcc-14.2/bin/gcc"


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
