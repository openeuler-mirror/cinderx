import os
import platform
import sys
import unittest
from pathlib import Path

import cinderx
import cinderx.jit
from cinderx.test_support import run_python_child

try:
    import cinderjit
except ImportError:
    cinderjit = None


HELPER = Path(__file__).with_name("child_cases") / "lightweight_frames.py"
IS_AARCH64 = platform.machine().lower() in {"aarch64", "arm64"}
IS_CPYTHON_311 = sys.version_info[:2] == (3, 11)


def _clean_env() -> dict[str, str]:
    env = os.environ.copy()
    for key in (
        "CINDERX_DISABLE",
        "CINDERX_EVAL_MODE",
        "CINDERX_JIT_DISABLE",
        "CINDERX_JIT_MODE",
        "CINDERX_OSR_ENABLED",
        "CINDERX_PLUGIN_ENABLE",
        "PYTHONJITALL",
        "PYTHONJITAUTO",
        "PYTHONJITDEBUG",
        "PYTHONJITDISABLE",
        "PYTHONJITDUMPASM",
        "PYTHONJITGENERATOR",
        "PYTHONJITLIGHTWEIGHTFRAME",
    ):
        env.pop(key, None)
    return env


def _run_tls_case(case: str) -> str:
    env = _clean_env()
    env.update(
        {
            "CINDERX_EVAL_MODE": "cinder",
            "CINDERX_JIT_MODE": "execute",
            "CINDERX_PLUGIN_ENABLE": "1",
            "PYTHONJITLIGHTWEIGHTFRAME": "1",
            "PYTHONUNBUFFERED": "1",
        }
    )
    completed = run_python_child(
        HELPER,
        case,
        env=env,
        timeout=120,
    )
    output = completed.stdout + completed.stderr
    assert completed.returncode == 0, (
        f"{case}: subprocess failed with {completed.returncode}\n"
        f"stdout:\n{completed.stdout}\n"
        f"stderr:\n{completed.stderr}"
    )
    assert "Traceback" not in output, output
    return output


def _run_lightweight_case(
    case: str, *, enable_generators: bool = False, jit_mode: str = "execute"
) -> str:
    env = _clean_env()
    env.update(
        {
            "CINDERX_EVAL_MODE": "cinder",
            "CINDERX_JIT_MODE": jit_mode,
            "CINDERX_PLUGIN_ENABLE": "1",
            "PYTHONJITLIGHTWEIGHTFRAME": "1",
            "PYTHONUNBUFFERED": "1",
        }
    )
    if enable_generators:
        env["PYTHONJITGENERATOR"] = "1"
    if jit_mode == "canary":
        env["PYTHONJITAUTO"] = "1000000"
    completed = run_python_child(HELPER, case, env=env, timeout=120)
    output = completed.stdout + completed.stderr
    assert completed.returncode == 0, (
        f"{case}: subprocess failed with {completed.returncode}\n"
        f"stdout:\n{completed.stdout}\n"
        f"stderr:\n{completed.stderr}"
    )
    assert "Traceback" not in output, output
    return output


def _run_default_mode_case() -> str:
    env = _clean_env()
    env.update(
        {
            "CINDERX_EVAL_MODE": "cinder",
            "CINDERX_JIT_MODE": "execute",
            "CINDERX_PLUGIN_ENABLE": "1",
            "PYTHONUNBUFFERED": "1",
        }
    )
    completed = run_python_child(HELPER, "mode", env=env, timeout=120)
    output = completed.stdout + completed.stderr
    assert completed.returncode == 0, (
        f"default mode: subprocess failed with {completed.returncode}\n"
        f"stdout:\n{completed.stdout}\n"
        f"stderr:\n{completed.stderr}"
    )
    assert "Traceback" not in output, output
    return output


def _run_normal_generator_rollback_case() -> str:
    env = _clean_env()
    env.update(
        {
            "CINDERX_EVAL_MODE": "cinder",
            "CINDERX_JIT_MODE": "execute",
            "CINDERX_PLUGIN_ENABLE": "1",
            "PYTHONJITAUTO": "2",
            "PYTHONJITGENERATOR": "1",
            "PYTHONUNBUFFERED": "1",
        }
    )
    env["PYTHONJITLIGHTWEIGHTFRAME"] = "0"
    completed = run_python_child(
        HELPER,
        "normal_generator",
        env=env,
        timeout=120,
    )
    output = completed.stdout + completed.stderr
    assert completed.returncode == 0, (
        f"normal generator rollback: subprocess failed with "
        f"{completed.returncode}\nstdout:\n{completed.stdout}\n"
        f"stderr:\n{completed.stderr}"
    )
    assert "Traceback" not in output, output
    return output


def _run_recursion_case(frame_mode: str) -> str:
    env = _clean_env()
    env.update(
        {
            "CINDERX_EVAL_MODE": "cinder",
            "CINDERX_JIT_MODE": "execute",
            "CINDERX_PLUGIN_ENABLE": "1",
            "PYTHONJITAUTO": "2",
            "PYTHONJITGENERATOR": "1",
            "PYTHONUNBUFFERED": "1",
        }
    )
    if frame_mode == "lightweight":
        env["PYTHONJITLIGHTWEIGHTFRAME"] = "1"
        expected_mode = 1
    elif frame_mode == "rollback":
        env["PYTHONJITLIGHTWEIGHTFRAME"] = "0"
        expected_mode = 0
    else:
        raise ValueError(f"unknown frame mode {frame_mode!r}")

    completed = run_python_child(HELPER, "recursion", env=env, timeout=120)
    output = completed.stdout + completed.stderr
    assert completed.returncode == 0, (
        f"recursion frame_mode={frame_mode}: subprocess failed with "
        f"{completed.returncode}\nstdout:\n{completed.stdout}\n"
        f"stderr:\n{completed.stderr}"
    )
    assert "Traceback" not in output, output
    assert f"CASE_RESULT recursion OK mode={expected_mode}" in output, output
    return output


class LightweightFramesTests(unittest.TestCase):
    def test_lightweight_frames_api(self) -> None:
        self.assertIsInstance(cinderx.is_lightweight_frames_enabled(), bool)
        self.assertIsInstance(cinderx.jit.is_lightweight_frames_enabled(), bool)

    @unittest.skipUnless(IS_CPYTHON_311, "CPython 3.11 delivery contract")
    @unittest.skipIf(cinderjit is None, "cinderjit unavailable")
    def test_python311_control_surface_exposes_frame_mode_queries(self) -> None:
        self.assertIn(cinderjit.jit_frame_mode(), (0, 1))
        self.assertEqual(
            cinderjit.is_lightweight_frames_enabled(),
            cinderx.is_lightweight_frames_enabled(),
        )

    @unittest.skipUnless(IS_CPYTHON_311, "CPython 3.11 delivery contract")
    def test_python311_defaults_to_normal_frames(self) -> None:
        output = _run_default_mode_case()
        self.assertIn("CASE_RESULT frame_mode OK 0", output)

    @unittest.skipUnless(IS_CPYTHON_311, "CPython 3.11 delivery contract")
    def test_python311_normal_generator_with_lwf_rollback(self) -> None:
        output = _run_normal_generator_rollback_case()
        self.assertIn(
            "CASE_RESULT normal_generator OK 0 2 4 6 8",
            output,
        )

    @unittest.skipUnless(IS_CPYTHON_311, "CPython 3.11 delivery contract")
    def test_python311_jit_recursion_balances_frame_modes(self) -> None:
        frame_modes = ["rollback"]
        if cinderx.is_lightweight_frames_enabled():
            frame_modes.insert(0, "lightweight")
        for frame_mode in frame_modes:
            with self.subTest(frame_mode=frame_mode):
                _run_recursion_case(frame_mode)

    @unittest.skipUnless(IS_AARCH64, "AArch64-specific TLS instruction parser")
    @unittest.skipUnless(
        cinderjit is not None
        and hasattr(cinderjit, "_test_parse_thread_state_prologue"),
        "TLS parser test hook unavailable",
    )
    def test_s4_standard_tls_access_shape_extracts_offset(self) -> None:
        code = [
            0xA9BF7BFD,  # stp x29, x30, [sp, #-16]!
            0x910003FD,  # mov x29, sp
            0xD53BD050,  # mrs x16, tpidr_el0
            0x91404210,  # add x16, x16, #0x10, lsl #12
            0x9107C210,  # add x16, x16, #0x1f0
            0xF9400200,  # ldr x0, [x16]
            0xD65F03C0,  # ret
        ]
        self.assertEqual(
            cinderjit._test_parse_thread_state_prologue(code),
            (0x10 << 12) + 0x1F0,
        )

    @unittest.skipUnless(IS_AARCH64, "AArch64-specific TLS instruction parser")
    @unittest.skipUnless(
        cinderjit is not None
        and hasattr(cinderjit, "_test_parse_thread_state_prologue"),
        "TLS parser test hook unavailable",
    )
    def test_s5_no_prologue_mrs_tls_access_shape_extracts_offset(self) -> None:
        code = [
            0xD53BD050,  # mrs x16, tpidr_el0
            0x91082210,  # add x16, x16, #0x208
            0xF9400200,  # ldr x0, [x16]
            0xD65F03C0,  # ret
        ]
        self.assertEqual(cinderjit._test_parse_thread_state_prologue(code), 0x208)

    @unittest.skipUnless(IS_AARCH64, "AArch64-specific JIT code shape")
    @unittest.skipUnless(
        cinderx.is_lightweight_frames_enabled(),
        "LWF not compiled in",
    )
    @unittest.skipUnless(
        cinderjit is not None and hasattr(cinderjit, "disassemble"),
        "JIT disassembler unavailable",
    )
    def test_s6_real_jit_dump_uses_inline_tls_access(self) -> None:
        output = _run_tls_case("inline")
        self.assertIn("tpidr_el0", output)
        self.assertNotIn("_PyThreadState_GetCurrent", output)

    @unittest.skipUnless(IS_AARCH64, "AArch64-specific JIT code shape")
    @unittest.skipUnless(
        cinderx.is_lightweight_frames_enabled(),
        "LWF not compiled in",
    )
    def test_s7_inline_tls_minimal_jit_function_executes(self) -> None:
        output = _run_tls_case("execute")
        self.assertIn("CASE_RESULT minimal_jit_target OK 42", output)

    @unittest.skipUnless(IS_CPYTHON_311, "CPython 3.11 delivery contract")
    @unittest.skipUnless(
        cinderx.is_lightweight_frames_enabled(),
        "LWF not compiled in",
    )
    def test_python311_lightweight_frame_clears_argument_local(self) -> None:
        output = _run_tls_case("localsplus_reuse")
        self.assertIn("CASE_RESULT localsplus_reuse OK 100", output)

    @unittest.skipUnless(IS_AARCH64, "AArch64-specific TLS instruction parser")
    @unittest.skipUnless(
        cinderjit is not None
        and hasattr(cinderjit, "_test_parse_thread_state_prologue"),
        "TLS parser test hook unavailable",
    )
    def test_s8_unrecognized_tls_shape_falls_back_conservatively(self) -> None:
        code = [
            0xD2800000,  # mov x0, #0
            0xD65F03C0,  # ret
        ]
        self.assertIsNone(cinderjit._test_parse_thread_state_prologue(code))
        self.assertIsNone(
            cinderjit._test_parse_thread_state_prologue(
                [
                    0xD53BD050,  # mrs x16, tpidr_el0
                    0x91082210,  # add x16, x16, #0x208
                    0x91004210,  # add x16, x16, #0x10
                ]
            )
        )

        if cinderx.is_lightweight_frames_enabled():
            output = _run_tls_case("fallback")
            self.assertIn("CASE_RESULT minimal_jit_target OK 42", output)
            self.assertNotIn("tpidr_el0", output)

    @unittest.skipUnless(
        cinderx.is_lightweight_frames_enabled(),
        "LWF not compiled in",
    )
    def test_getframe_inside_jit_function_materializes_frame(self) -> None:
        output = _run_lightweight_case("materialize_getframe")
        self.assertIn("CASE_RESULT materialize_getframe OK mode=1", output)

    @unittest.skipUnless(
        cinderx.is_lightweight_frames_enabled(),
        "LWF not compiled in",
    )
    def test_traceback_from_jit_function_materializes_frame(self) -> None:
        output = _run_lightweight_case("materialize_traceback")
        self.assertIn("CASE_RESULT materialize_traceback OK mode=1", output)

    @unittest.skipUnless(
        cinderx.is_lightweight_frames_enabled(),
        "LWF not compiled in",
    )
    def test_generator_return_cleanup_marks_generator_done(self) -> None:
        output = _run_lightweight_case(
            "generator_return_cleanup", enable_generators=True
        )
        self.assertIn("CASE_RESULT generator_return_cleanup OK mode=1", output)

    @unittest.skipUnless(
        cinderx.is_lightweight_frames_enabled(),
        "LWF not compiled in",
    )
    def test_generator_argument_survives_initial_yield(self) -> None:
        output = _run_lightweight_case(
            "generator_argument_lifetime", enable_generators=True
        )
        self.assertIn("CASE_RESULT generator_argument_lifetime OK mode=1", output)

    @unittest.skipUnless(
        cinderx.is_lightweight_frames_enabled(),
        "LWF not compiled in",
    )
    def test_generator_close_releases_owned_argument_once(self) -> None:
        output = _run_lightweight_case(
            "generator_close_gc", enable_generators=True
        )
        self.assertIn("CASE_RESULT generator_close_gc OK mode=1", output)

    @unittest.skipUnless(
        cinderx.is_lightweight_frames_enabled(),
        "LWF not compiled in",
    )
    @unittest.skipUnless(IS_CPYTHON_311, "CPython 3.11 canary control plane")
    def test_forced_deopt_rebuilds_locals_and_value_stack(self) -> None:
        output = _run_lightweight_case(
            "forced_deopt_restore", jit_mode="canary"
        )
        self.assertIn("CASE_RESULT forced_deopt_restore OK mode=1", output)

    @unittest.skipUnless(
        cinderx.is_lightweight_frames_enabled(),
        "LWF not compiled in",
    )
    def test_normal_and_exceptional_exits_release_owned_argument_once(self) -> None:
        output = _run_lightweight_case("exit_ownership", jit_mode="canary")
        self.assertIn("CASE_RESULT exit_ownership OK mode=1", output)

    @unittest.skipUnless(
        cinderx.is_lightweight_frames_enabled(),
        "LWF not compiled in",
    )
    def test_deopt_preserves_materialized_frame_identity(self) -> None:
        output = _run_lightweight_case("deopt_materialized_frame")
        self.assertIn("CASE_RESULT deopt_materialized_frame OK mode=1", output)

    @unittest.skipUnless(
        cinderx.is_lightweight_frames_enabled(),
        "LWF not compiled in",
    )
    @unittest.skipUnless(IS_CPYTHON_311, "CPython 3.11 delivery contract")
    def test_generator_frame_traversal_and_escape_lifecycle(self) -> None:
        output = _run_lightweight_case(
            "generator_frame_lifecycle", enable_generators=True
        )
        self.assertIn("CASE_RESULT generator_frame_lifecycle OK mode=1", output)

    @unittest.skipUnless(
        cinderx.is_lightweight_frames_enabled(),
        "LWF not compiled in",
    )
    def test_f_locals_dictionary_releases_owned_argument(self) -> None:
        output = _run_lightweight_case("f_locals_ownership")
        self.assertIn("CASE_RESULT f_locals_ownership OK mode=1", output)


if __name__ == "__main__":
    unittest.main()
