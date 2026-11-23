# Copyright (c) Meta Platforms, Inc. and affiliates.

import os
import platform
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from cinderx.test_support import (
    ENCODING,
    SUBPROCESS_TIMEOUT_SEC,
    assert_python_child_ok,
    run_python_child,
    subprocess_env,
)


SKIP_PREFIX = "PYTEST_SKIP "
CHILD = Path(__file__).with_name("child_cases") / "elf_machine.py"


def _expected_elf_machine() -> int:
    machine = platform.machine().lower()
    if machine in {"x86_64", "amd64"}:
        return 0x3E
    if machine in {"aarch64", "arm64"}:
        return 0xB7
    raise unittest.SkipTest(f"unsupported ELF machine test architecture: {machine}")


def _read_elf_machine(path: Path) -> int:
    data = path.read_bytes()
    assert data[:4] == b"\x7fELF"
    assert data[4] == 2
    assert data[5] == 1
    return struct.unpack_from("<H", data, 18)[0]


def _expected_tool_arch_marker() -> str:
    machine = platform.machine().lower()
    if machine in {"x86_64", "amd64"}:
        return "x86-64"
    if machine in {"aarch64", "arm64"}:
        return "aarch64"
    raise unittest.SkipTest(f"unsupported ELF machine test architecture: {machine}")


def _run_external_tool(args: list[str]) -> str | None:
    executable = shutil.which(args[0])
    if executable is None:
        return None

    env = os.environ.copy()
    env["LC_ALL"] = "C"
    completed = subprocess.run(
        [executable, *args[1:]],
        env=env,
        capture_output=True,
        encoding=ENCODING,
        errors="replace",
        timeout=max(SUBPROCESS_TIMEOUT_SEC, 60),
    )
    output = (completed.stdout or "") + (completed.stderr or "")
    assert completed.returncode == 0, (
        f"{args[0]} failed with {completed.returncode}\n"
        f"stdout:\n{completed.stdout}\n"
        f"stderr:\n{completed.stderr}"
    )
    return output


def _assert_external_tools_report_machine(
    case: unittest.TestCase, path: Path
) -> None:
    expected_marker = _expected_tool_arch_marker()
    for args in (["readelf", "-h", str(path)], ["objdump", "-f", str(path)]):
        if shutil.which(args[0]) is None:
            case.skipTest(f"{args[0]} is not available")

        output = _run_external_tool(args)
        if output is None:
            case.fail(f"{args[0]} is not available")
        normalized_output = output.lower()
        case.assertIn(
            expected_marker,
            normalized_output,
            f"{args[0]} did not report {expected_marker!r} for {path}\n"
            f"output:\n{output}",
        )


def _subprocess_env(**overrides: str) -> dict[str, str]:
    env = os.environ.copy()
    env.update(subprocess_env())
    for name in ("CINDERX_DISABLE", "CINDERX_JIT_DISABLE", "PYTHONJITDISABLE"):
        env.pop(name, None)
    env.update(
        {
            "CINDERX_PLUGIN_ENABLE": "1",
            "PYTHONJITALL": "1",
            "PYTHONUNBUFFERED": "1",
        }
    )
    env.update(overrides)
    return env


def _run_child(
    case: str,
    output_path: Path,
    tmp_path: Path,
    env: dict[str, str],
) -> str:
    completed = run_python_child(
        CHILD,
        case,
        str(output_path),
        cwd=tmp_path,
        env=env,
        timeout=max(SUBPROCESS_TIMEOUT_SEC, 60),
    )
    output = (completed.stdout or "") + (completed.stderr or "")
    for line in output.splitlines():
        if line.startswith(SKIP_PREFIX):
            raise unittest.SkipTest(line[len(SKIP_PREFIX) :])

    assert_python_child_ok(
        completed,
        context=f"ELF machine child case {case}",
    )
    assert "Traceback" not in output
    return output


class ElfMachineTests(unittest.TestCase):
    def test_dump_elf_uses_build_machine(self) -> None:
        expected_machine = _expected_elf_machine()
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            elf_path = tmp_path / "jit_dump.elf"
            _run_child(
                "dump-elf",
                elf_path,
                tmp_path,
                _subprocess_env(),
            )

            self.assertEqual(_read_elf_machine(elf_path), expected_machine)
            _assert_external_tools_report_machine(self, elf_path)

    def test_gdb_jit_elf_uses_build_machine(self) -> None:
        expected_machine = _expected_elf_machine()
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            elf_path = tmp_path / "gdb_jit.elf"
            # Keep automatic compilation out of this short subprocess.
            env = _subprocess_env(
                TMPDIR=str(tmp_path),
                PYTHONJITGDBWRITEELF="1",
                CINDERX_JIT_MODE="execute",
                PYTHONJITAUTO="1000000",
            )
            env.pop("PYTHONJITALL", None)
            _run_child("gdb-jit-elf", elf_path, tmp_path, env)

            self.assertEqual(_read_elf_machine(elf_path), expected_machine)
            _assert_external_tools_report_machine(self, elf_path)


if __name__ == "__main__":
    unittest.main()
