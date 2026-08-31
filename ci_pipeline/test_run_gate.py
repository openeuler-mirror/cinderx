import json
import subprocess

import ci_pipeline.run_gate as run_gate


def test_pr_pipeline_routes_python311_to_pr311_gate():
    assert run_gate.resolve_pipeline_name("pr", (3, 11)) == "pr311"


def test_pr_pipeline_keeps_python314_gate():
    assert run_gate.resolve_pipeline_name("pr", (3, 14)) == "pr"


def test_explicit_pipeline_name_is_not_rewritten():
    assert run_gate.resolve_pipeline_name("pr311", (3, 11)) == "pr311"


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
