import os
import subprocess
from pathlib import Path
import types
import unittest

import cinderx.jit
from cinderx.test_support import assert_python_child_ok, run_python_child


CHILD = Path(__file__).with_name("child_cases") / "jit_cached_compiled_entry.py"


def _clean_jit_env() -> dict[str, str]:
    env = os.environ.copy()
    for name in (
        "CINDERX_DISABLE",
        "CINDERX_JIT_DISABLE",
        "PYTHONJITDISABLE",
        "PYTHONJITALL",
        "PYTHONJITALLSTATICFUNCTIONS",
        "PYTHONJITAUTO",
        "PYTHONJITLISTFILE",
    ):
        env.pop(name, None)
    env["CINDERX_PLUGIN_ENABLE"] = "1"
    return env


def _run_clean_jit_subprocess(case: str) -> subprocess.CompletedProcess[str]:
    return run_python_child(
        CHILD,
        case,
        env=_clean_jit_env(),
        timeout=60,
    )


@unittest.skipUnless(cinderx.jit.is_enabled(), "requires CinderX JIT")
class CachedCompiledEntryTests(unittest.TestCase):
    def assert_clean_jit_subprocess_succeeds(self, case: str) -> None:
        completed = _run_clean_jit_subprocess(case)
        assert_python_child_ok(
            completed,
            context=f"cached compiled entry case {case}",
        )

    def test_recreated_function_attaches_cached_compiled_entry(self) -> None:
        # Other test_cinderx cases can leave a process-local JIT list installed.
        # The cached-entry fast path intentionally stays disabled in that state.
        self.assert_clean_jit_subprocess_succeeds("fast-attach")

    def test_jit_list_disables_cached_compiled_entry_fast_path(self) -> None:
        self.assert_clean_jit_subprocess_succeeds("jit-list-bail")

    def test_instrumentation_disables_cached_compiled_entry_fast_path(self) -> None:
        self.assert_clean_jit_subprocess_succeeds("instrumentation-bail")

    def test_many_recreated_functions_preserve_cached_entry_semantics(self) -> None:
        self.assert_clean_jit_subprocess_succeeds("many-recreated")

    def test_cached_entry_does_not_cross_globals(self) -> None:
        globs = {
            "value": "original",
            "__builtins__": __builtins__,
        }
        exec("def read_global():\n    return value\n", globs)
        read_global = globs["read_global"]

        self.assertTrue(cinderx.jit.force_compile(read_global))
        self.assertTrue(cinderx.jit.is_jit_compiled(read_global))
        self.assertEqual(read_global(), "original")

        other_globals = {
            "value": "other",
            "__builtins__": read_global.__globals__["__builtins__"],
        }
        other = types.FunctionType(
            read_global.__code__, other_globals, "read_global"
        )

        self.assertEqual(other(), "other")

    def test_cached_entry_does_not_cross_builtins(self) -> None:
        def make_function_with_builtin_marker(result):
            globs = {"__builtins__": {"marker": lambda: result}}
            exec("def call_marker():\n    return marker()\n", globs)
            return globs["call_marker"]

        first = make_function_with_builtin_marker("original")
        self.assertTrue(cinderx.jit.force_compile(first))
        self.assertTrue(cinderx.jit.is_jit_compiled(first))
        self.assertEqual(first(), "original")

        other = types.FunctionType(
            first.__code__,
            {"__builtins__": {"marker": lambda: "other"}},
            "call_marker",
        )

        self.assertEqual(other(), "other")


if __name__ == "__main__":
    unittest.main()
