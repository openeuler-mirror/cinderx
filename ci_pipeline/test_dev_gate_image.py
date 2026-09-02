import json
import os
from pathlib import Path
import shutil
import subprocess


REPO_ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = REPO_ROOT / "ci_pipeline/docker/dev-gate/python-launcher.sh"


def make_fake_python(path: Path) -> None:
    path.write_text(
        "#!/usr/bin/env python3\n"
        "import json, os, sys\n"
        "print(json.dumps({'argv': sys.argv[1:], 'env': dict(os.environ)}))\n"
    )
    path.chmod(0o755)


def invoke_launcher(tmp_path: Path, version: str, cwd: Path, *args: str):
    launcher = tmp_path / f"python{version}"
    shutil.copyfile(LAUNCHER, launcher)
    launcher.chmod(0o755)
    fake_python = tmp_path / f"fake-python-{version}"
    make_fake_python(fake_python)
    env = os.environ.copy()
    env.pop("RT311_BASELINE_BASE", None)
    env[f"CINDERX_DEV_GATE_PYTHON{version.replace('.', '')}"] = str(
        fake_python
    )
    completed = subprocess.run(
        [launcher, *args],
        cwd=cwd,
        env=env,
        check=True,
        capture_output=True,
        text=True,
    )
    return json.loads(completed.stdout)


def test_python311_launcher_selects_toolchain_and_merge_base(tmp_path):
    repo = tmp_path / "repo"
    repo.mkdir()
    subprocess.run(["git", "init", "-q", "-b", "dev"], cwd=repo, check=True)
    subprocess.run(["git", "config", "user.name", "Gate Test"], cwd=repo, check=True)
    subprocess.run(["git", "config", "user.email", "gate@example.invalid"], cwd=repo, check=True)
    (repo / "base").write_text("base\n")
    subprocess.run(["git", "add", "base"], cwd=repo, check=True)
    subprocess.run(["git", "commit", "-q", "-m", "base"], cwd=repo, check=True)
    base = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
    subprocess.run(["git", "update-ref", "refs/remotes/origin/dev", base], cwd=repo, check=True)
    subprocess.run(["git", "switch", "-q", "-c", "feature"], cwd=repo, check=True)
    (repo / "head").write_text("head\n")
    subprocess.run(["git", "add", "head"], cwd=repo, check=True)
    subprocess.run(["git", "commit", "-q", "-m", "head"], cwd=repo, check=True)

    result = invoke_launcher(
        tmp_path, "3.11", repo, "ci_pipeline/run_gate.py", "pr", "--list"
    )

    assert result["argv"] == ["ci_pipeline/run_gate.py", "pr", "--list"]
    assert result["env"]["CC"] == "/usr/local/bin/gcc-14"
    assert result["env"]["CXX"] == "/usr/local/bin/g++-14"
    assert result["env"]["RT311_BASELINE_BASE"] == base
    assert result["env"]["CINDERX_RUNTIME_TEST_PYTHON"] == "/usr/bin/python3.11"


def test_python314_launcher_clears_cp311_environment(tmp_path):
    env_names = (
        "CINDERX_TEST_PYTHON_STDLIB_DIR",
        "CINDERX_RUNTIME_TEST_PYTHON",
        "CINDERX_RUNTIME_TEST_PYTHON_LIBRARY",
        "CINDERX_ENABLE_LTO",
    )
    previous = {name: os.environ.get(name) for name in env_names}
    try:
        for name in env_names:
            os.environ[name] = "unexpected"
        result = invoke_launcher(tmp_path, "3.14", tmp_path, "-V")
    finally:
        for name, value in previous.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value

    assert result["env"]["CC"] == "/usr/local/bin/gcc-14"
    assert result["env"]["CXX"] == "/usr/local/bin/g++-14"
    for name in env_names:
        assert name not in result["env"]
