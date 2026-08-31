import json
import subprocess

import ci_pipeline.run_gate as run_gate


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


def test_runtime_gate_forces_golden_sample_compare_mode(monkeypatch):
    monkeypatch.setenv("UPDATE_HIR_PIPELINE_GOLDEN", "1")
    monkeypatch.setattr(run_gate, "configure_toolchain", lambda env: None)

    suite = run_gate.load_suite("runtime")
    job = suite["jobs"][0]
    env = run_gate.merged_env(job)

    assert env["UPDATE_HIR_PIPELINE_GOLDEN"] == "0"
