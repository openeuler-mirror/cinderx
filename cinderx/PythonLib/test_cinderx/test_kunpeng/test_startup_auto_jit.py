import json
import os
from pathlib import Path

from cinderx.test_support import run_python_child

HELPER = Path(__file__).with_name("startup_auto_jit_helper.py")


def _startup_auto_jit_env():
    env = os.environ.copy()
    env.pop("PYTHONPATH", None)
    for name in ("CINDERX_DISABLE", "CINDERX_JIT_DISABLE", "PYTHONJITDISABLE"):
        env.pop(name, None)

    env["PYTHONJITALL"] = "1"
    return env


def _startup_provider_env(provider):
    env = _startup_auto_jit_env()
    env.pop("PYTHONJITALL", None)
    env["CINDERX_PLUGIN_ENABLE"] = "1"
    env["PYTHONJITAUTO"] = "auto:2"
    env["CINDERX_AUTOJIT_IMPORT_PROVIDER"] = provider
    return env


def _run_startup_auto_jit_helper(tmp_path, env, failure_message):
    completed = run_python_child(
        HELPER,
        # Keep the child away from the source tree so startup must use the
        # installed cinderx package rather than local imports.
        cwd=tmp_path,
        env=env,
        timeout=60,
    )
    assert completed.returncode == 0, (
        f"{failure_message}\n"
        f"stdout:\n{completed.stdout}\n"
        f"stderr:\n{completed.stderr}"
    )
    assert "Traceback" not in completed.stderr


def test_installed_cinderx_auto_imports_and_jits(tmp_path):
    """Requires cinderx to be installed into the tested interpreter."""
    env = _startup_auto_jit_env()
    env["CINDERX_PLUGIN_ENABLE"] = "1"

    _run_startup_auto_jit_helper(
        tmp_path, env, "installed cinderx auto-import JIT subprocess failed"
    )


def test_installed_cinderx_auto_import_builtins_provider_tracks_depth(tmp_path):
    """Requires cinderx to be installed into the tested interpreter."""
    env = _startup_provider_env("builtins")

    _run_startup_auto_jit_helper(
        tmp_path,
        env,
        "installed cinderx builtins import provider subprocess failed",
    )


def test_installed_cinderx_auto_import_find_and_load_provider_tracks_depth(tmp_path):
    """Requires cinderx to be installed into the tested interpreter."""
    env = _startup_provider_env("find_and_load")

    _run_startup_auto_jit_helper(
        tmp_path,
        env,
        "installed cinderx find_and_load import provider subprocess failed",
    )


def test_installed_cinderx_auto_import_uses_lazy_bootstrap(tmp_path):
    """Requires cinderx to be installed into the tested interpreter."""
    env = _startup_provider_env("find_and_load")

    _run_startup_auto_jit_helper(
        tmp_path,
        env,
        "installed cinderx lazy startup bootstrap subprocess failed",
    )


def test_installed_cinderx_auto_setup_provider_tracks_depth(tmp_path):
    """Requires cinderx to be installed into the tested interpreter."""
    env = _startup_provider_env("find_and_load")
    env["CINDERX_AUTOJIT_SETUP_PROVIDER"] = "lib2to3_main"

    _run_startup_auto_jit_helper(
        tmp_path,
        env,
        "installed cinderx setup provider subprocess failed",
    )


def test_installed_plugin_candidate_keeps_autojit_held(tmp_path):
    """Requires cinderx to be installed into the tested interpreter."""
    dist_info = tmp_path / "candidate-1.0.dist-info"
    dist_info.mkdir()
    (dist_info / "METADATA").write_text(
        "Metadata-Version: 2.1\nName: candidate\nVersion: 1.0\n",
        encoding="utf-8",
    )
    (dist_info / "cinderx_plugin.json").write_text(
        json.dumps(
            {
                "id": "candidate",
                "spi_version": "1",
                "runtime_abi": {
                    "python_version": "3.14",
                    "soabi": "test",
                    "core_build_id": "test",
                    "cpu_caps": [],
                },
                "target_capabilities": [],
                "provides": {},
                "adapter": {
                    "entry": "never_imported.adapter",
                    "target": "never_imported_target",
                },
            }
        ),
        encoding="utf-8",
    )

    env = _startup_provider_env("find_and_load")
    env["EXPECT_CINDERX_EAGER_BOOTSTRAP"] = "1"
    env["PYTHONPATH"] = str(tmp_path)

    _run_startup_auto_jit_helper(
        tmp_path,
        env,
        "installed plugin candidate released the AutoJIT hold",
    )


def test_installed_cinderx_auto_import_disabled_when_plugin_env_unset(tmp_path):
    """Requires cinderx to be installed into the tested interpreter."""
    env = _startup_auto_jit_env()
    env.pop("CINDERX_PLUGIN_ENABLE", None)

    _run_startup_auto_jit_helper(
        tmp_path,
        env,
        (
            "installed cinderx auto-import subprocess should be disabled when "
            "CINDERX_PLUGIN_ENABLE is unset"
        ),
    )


def test_installed_cinderx_auto_import_disabled_when_plugin_env_zero(tmp_path):
    """Requires cinderx to be installed into the tested interpreter."""
    env = _startup_auto_jit_env()
    env["CINDERX_PLUGIN_ENABLE"] = "0"

    _run_startup_auto_jit_helper(
        tmp_path,
        env,
        (
            "installed cinderx auto-import subprocess should be disabled when "
            "CINDERX_PLUGIN_ENABLE is 0"
        ),
    )
