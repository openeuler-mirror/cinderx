# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Startup policy, explicit deopts, and bounded generic retries on 3.11."""

import os
import subprocess
import sys
import textwrap
import unittest


PREAMBLE = """
import json
import time
import _cinderx
import cinderx
cinderx.init()
_cinderx.install_frame_evaluator()
import cinderjit
assert cinderx.is_initialized()
assert cinderjit.is_enabled()
assert _cinderx.is_frame_evaluator_installed()
"""


@unittest.skipUnless(sys.version_info[:2] == (3, 11), "CPython 3.11 scheduling")
class SchedulingPolicy311Tests(unittest.TestCase):
    def run_probe(self, source, *, options=(), env=None, success=True):
        child_env = {
            key: value for key, value in os.environ.items()
            if key != "PYTHONPATH"
            and not key.startswith(("CINDERX_", "PYTHONJIT", "SPIKE_", "PARALLEL_GC_"))
        }
        child_env.update(CINDERX_JIT_MODE="execute", PYTHONJITAUTO="2")
        child_env.update(env or {})
        proc = subprocess.run(
            [sys.executable, *options, "-c", PREAMBLE + textwrap.dedent(source)],
            env=child_env, text=True, capture_output=True, timeout=120,
        )
        if success:
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        else:
            self.assertGreater(proc.returncode, 0, proc.stderr)
        return proc

    def test_default_grace_defers_without_spending_an_attempt(self):
        self.run_probe("""
            def hot(x):
                return x + 1
            cinderjit.after_fork_child()
            assert hot(1) == 2
            assert hot(2) == 3
            assert not cinderjit.is_jit_compiled(hot)
            events = _cinderx._get_observe_stats()["events"]
            assert not any(e["qualname"] == "hot" for e in events), events
            time.sleep(0.2)
            before = _cinderx._get_trigger_stats()["machine_code_entries"]
            assert hot(3) == 4
            assert cinderjit.is_jit_compiled(hot)
            # The threshold-crossing frame publishes the artifact; its
            # next invocation must actually enter the machine code.
            assert hot(4) == 5
            assert _cinderx._get_trigger_stats()["machine_code_entries"] > before
            events = [e for e in _cinderx._get_observe_stats()["events"]
                      if e["qualname"] == "hot"]
            assert len(events) == 1 and events[0]["result"] == "installed", events
        """)

    def test_zero_grace_respects_exact_call_threshold(self):
        self.run_probe("""
            def hot(x):
                return x + 1
            assert hot(1) == 2
            assert not cinderjit.is_jit_compiled(hot)
            assert hot(2) == 3
            assert cinderjit.is_jit_compiled(hot)
        """, env={"CINDERX_JIT_STARTUP_GRACE_MS": "0"})

    def test_xoption_overrides_environment_grace(self):
        self.run_probe("""
            def hot(x):
                return x + 1
            hot(1); hot(2)
            assert cinderjit.is_jit_compiled(hot)
        """, options=("-X", "jit-startup-grace-ms=0"),
                       env={"CINDERX_JIT_STARTUP_GRACE_MS": "invalid"})

    def test_invalid_grace_fails_initialization(self):
        for value in ("-1", "bad", "4294967296", "1.5"):
            with self.subTest(value=value):
                proc = self.run_probe("", env={"CINDERX_JIT_STARTUP_GRACE_MS": value},
                                      success=False)
                self.assertIn("CINDERX_JIT_STARTUP_GRACE_MS", proc.stderr)

    def test_explicit_compile_remains_available_during_grace(self):
        self.run_probe("""
            def explicit(x):
                return x + 1
            cinderjit.after_fork_child()
            assert cinderjit.force_compile(explicit)
            before = _cinderx._get_trigger_stats()["machine_code_entries"]
            assert explicit(41) == 42
            assert cinderjit.is_jit_compiled(explicit)
            assert _cinderx._get_trigger_stats()["machine_code_entries"] > before
        """, env={"CINDERX_JIT_STARTUP_GRACE_MS": "60000"})

    def test_forced_deopt_opt_out_is_explicit(self):
        self.run_probe("""
            def target(x):
                return x + 1
            assert cinderjit.force_compile(target)
            before = _cinderx._get_trigger_stats()
            assert target(4) == 5
            try:
                cinderjit.force_deopt(target, 0)
            except RuntimeError as exc:
                assert "CINDERX_JIT_FORCE_DEOPT_CHECKS=0" in str(exc), exc
            else:
                raise AssertionError("a disabled trigger was accepted")
            after = _cinderx._get_trigger_stats()
            assert after["machine_code_entries"] > before["machine_code_entries"]
            assert after["forced_deopt_hits"] == before["forced_deopt_hits"]
        """, env={"CINDERX_JIT_FORCE_DEOPT_CHECKS": "0"})

    def test_generic_retry_stabilizes_guards_and_keeps_exception_retirement(self):
        self.run_probe("""
            def first(x):
                return 1
            def second(x):
                return 2
            selected = first
            def target(x):
                return selected(x=x)
            for _ in range(100):
                assert target(1) == 1
            cinderjit._clear_autojit_gate_stats()
            assert cinderjit.force_compile(target)
            before = _cinderx._get_trigger_stats()
            selected = second
            assert target(1.5) == 2
            assert target(2.5) == 2
            assert not cinderjit.is_jit_compiled(target)
            stats = cinderjit._autojit_gate_stats()
            assert stats["roi_uncompile"] == 1 and stats["roi_frozen"] == 0, stats
            assert _cinderx._get_trigger_stats()["organic_deopt_hits"] > before["organic_deopt_hits"]
            assert cinderjit.force_compile(target)
            before = _cinderx._get_trigger_stats()
            for _ in range(64):
                assert target(3.5) == 2
            after = _cinderx._get_trigger_stats()
            assert after["machine_code_entries"] > before["machine_code_entries"]
            assert after["organic_deopt_hits"] == before["organic_deopt_hits"], (before, after)
            selected = int
            cinderjit.get_and_clear_runtime_stats()
            try:
                target(None)
            except TypeError:
                pass
            else:
                raise AssertionError("expected TypeError")
            # Inspect the reason while its artifact is still resident;
            # final retirement intentionally releases per-artifact stats.
            assert cinderjit.is_jit_compiled(target)
            deopts = cinderjit.get_and_clear_runtime_stats()["deopt"]
            own = [e for e in deopts if e["normal"]["func_qualname"] == "target"]
            assert own and all(e["normal"]["reason"] == "UnhandledException" for e in own), own
            for _ in range(64):
                try:
                    target(None)
                except TypeError:
                    pass
                else:
                    raise AssertionError("expected TypeError")
            assert not cinderjit.is_jit_compiled(target)
            stats = cinderjit._autojit_gate_stats()
            # Retry transitions and final freezes have separate counters.
            assert stats["roi_uncompile"] == 1 and stats["roi_frozen"] == 1, stats
        """, env={"PYTHONJITAUTO": "1000000", "CINDERX_JIT_STARTUP_GRACE_MS": "0",
                   "CINDERX_AUTOJIT_ROI_BACKOFF_BUDGET": "2",
                   "CINDERX_AUTOJIT_ROI_BACKOFF_MAX_ROUNDS": "1",
                   "CINDERX_AUTOJIT_ROI_AGING_INTERVAL_MS": "0"})

    def test_automatic_retry_rewarms_once_and_then_stays_frozen(self):
        self.run_probe("""
            def first(x):
                return 1
            def second(x):
                return 2
            selected = first
            def target(x):
                return selected(x=x)
            for _ in range(4):
                assert target(1) == 1
            assert cinderjit.is_jit_compiled(target)
            cinderjit._clear_autojit_gate_stats()
            selected = second
            assert target(1.5) == 2
            assert target(2.5) == 2
            assert not cinderjit.is_jit_compiled(target)
            # Threshold 2 times rewarm factor 4: all seven preceding calls
            # must stay interpreted, without consuming another attempt.
            for i in range(7):
                assert target(3.5) == 2
                assert not cinderjit.is_jit_compiled(target), i
            assert target(3.5) == 2
            assert cinderjit.is_jit_compiled(target)
            before = _cinderx._get_trigger_stats()
            for _ in range(32):
                assert target(4.5) == 2
            after = _cinderx._get_trigger_stats()
            assert after["machine_code_entries"] > before["machine_code_entries"]
            assert after["organic_deopt_hits"] == before["organic_deopt_hits"]
            selected = int
            cinderjit.get_and_clear_runtime_stats()
            try:
                target(None)
            except TypeError:
                pass
            else:
                raise AssertionError("expected TypeError")
            # Inspect the reason while its artifact is still resident;
            # final retirement intentionally releases per-artifact stats.
            assert cinderjit.is_jit_compiled(target)
            deopts = cinderjit.get_and_clear_runtime_stats()["deopt"]
            own = [e for e in deopts if e["normal"]["func_qualname"] == "target"]
            assert own and all(e["normal"]["reason"] == "UnhandledException" for e in own), own
            for _ in range(16):
                try:
                    target(None)
                except TypeError:
                    pass
                else:
                    raise AssertionError("expected TypeError")
            assert not cinderjit.is_jit_compiled(target)
            selected = second
            for _ in range(32):
                assert target(4.5) == 2
                assert not cinderjit.is_jit_compiled(target)
            stats = cinderjit._autojit_gate_stats()
            assert stats["roi_uncompile"] == 1, stats
            assert stats["roi_recompile"] == 1, stats
            assert stats["roi_frozen"] == 1, stats
        """, env={"CINDERX_JIT_STARTUP_GRACE_MS": "0",
                   "CINDERX_AUTOJIT_ROI_BACKOFF_BUDGET": "2",
                   "CINDERX_AUTOJIT_ROI_BACKOFF_MAX_ROUNDS": "1",
                   "CINDERX_AUTOJIT_ROI_REWARM_FACTOR": "4",
                   "CINDERX_AUTOJIT_ROI_AGING_INTERVAL_MS": "0"})

    @unittest.skipUnless(hasattr(os, "fork"), "requires fork")
    def test_fork_rearms_grace_without_retiring_inherited_code(self):
        source = """
            import os
            def inherited(x):
                return x + 1
            assert cinderjit.force_compile(inherited)
            time.sleep(0.6)
            read_fd, write_fd = os.pipe()
            pid = os.fork()
            if pid == 0:
                os.close(read_fd)
                try:
                    assert cinderjit.is_jit_compiled(inherited)
                    assert inherited(4) == 5
                    def child_only(x):
                        return x * 2
                    assert child_only(1) == 2
                    assert child_only(2) == 4
                    assert cinderjit.is_jit_compiled(child_only) is (not @REARM@)
                    time.sleep(0.6)
                    assert child_only(3) == 6
                    assert cinderjit.is_jit_compiled(child_only)
                    before = _cinderx._get_trigger_stats()["machine_code_entries"]
                    assert child_only(4) == 8
                    assert _cinderx._get_trigger_stats()["machine_code_entries"] > before
                    answer = {"ok": True}
                except BaseException as exc:
                    answer = {"error": repr(exc)}
                os.write(write_fd, json.dumps(answer).encode())
                os.close(write_fd)
                os._exit(0 if answer.get("ok") else 1)
            os.close(write_fd)
            data = os.read(read_fd, 65536)
            os.close(read_fd)
            _, status = os.waitpid(pid, 0)
            assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, (status, data)
            assert json.loads(data) == {"ok": True}, data
        """
        for rearm in (True, False):
            with self.subTest(rearm=rearm):
                self.run_probe(source.replace("@REARM@", repr(rearm)), env={
                    "CINDERX_JIT_STARTUP_GRACE_MS": "500",
                    "CINDERX_JIT_FORK_GRACE": "1" if rearm else "0",
                })


if __name__ == "__main__":
    unittest.main()
