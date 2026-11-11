import dis
import json
import os
from pathlib import Path
import subprocess
import sys
import textwrap
import unittest


@unittest.skipUnless(
    sys.version_info[:2] == (3, 11), "CPython 3.11 execution acceptance infrastructure"
)
class ExecutionInfra311Test(unittest.TestCase):
    def test_refcycle_contract_keeps_finalization_and_garbage_checks(self):
        # PR runs from cinderx/PythonLib; acceptance runs from a staged tree.
        # Locate the harness beside either layout rather than relying on cwd
        # or a developer's PYTHONPATH. Do not add source runtime packages.
        harness_root = next(
            parent
            for parent in Path(__file__).resolve().parents
            if (parent / "ci_pipeline/jit311/semantic_conformance_hook.py").is_file()
        )
        result = self.run_child(
            """
            import gc, json, os, tempfile, unittest
            from pathlib import Path
            from unittest.mock import patch
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit
            from test import support
            from test.test_generators import FinalizationTest

            fn = FinalizationTest.test_refcycle
            assert cinderjit._jit311_compile_diagnostic(fn)["compiled"]
            raw = unittest.TestResult()
            FinalizationTest('test_refcycle').run(raw)
            assert len(raw.failures) == 1 and not raw.errors
            assert '2 not greater than 2' in raw.failures[0][1]
            support.gc_collect()

            with tempfile.TemporaryDirectory() as directory:
                os.environ['EXECUTION_COMPILE_ALL_MODE'] = 'jit'
                os.environ['EXECUTION_COMPILE_ALL_LOG_DIR'] = directory
                from ci_pipeline.jit311 import semantic_conformance_hook
                collect = support.gc_collect
                was_enabled = gc.isenabled()
                gc.disable()
                outcomes = []
                deltas = []
                marker = object()
                def polluted_collection():
                    collect()
                    gc.garbage.append(marker)
                try:
                    for collector in (collect, lambda: None, polluted_collection):
                        case = FinalizationTest('test_refcycle')
                        before = _cinderx._get_trigger_stats()['machine_code_entries']
                        with patch.object(support, 'gc_collect', collector):
                            outcome = unittest.TestResult()
                            case.run(outcome)
                        assert not outcome.errors, outcome.errors
                        if collector is not collect:
                            assert len(outcome.failures) == 1, outcome.failures
                            expected = ('Lists differ' if collector is polluted_collection
                                        else 'False is not true')
                            assert expected in outcome.failures[0][1], outcome.failures
                        assert 'assertGreater' not in vars(case)
                        outcomes.append(outcome.wasSuccessful())
                        deltas.append(_cinderx._get_trigger_stats()['machine_code_entries'] - before)
                        if collector is polluted_collection:
                            assert gc.garbage[-1] is marker
                            gc.garbage.pop()
                        collect()
                finally:
                    if was_enabled:
                        gc.enable()
                events = [json.loads(line) for path in Path(directory).glob('*.jsonl')
                          for line in path.read_text().splitlines()]
                observations = [event for event in events
                                if event['type'] == 'assertion-contract']
                # atexit must still have a writable journal directory.
                import atexit
                atexit.unregister(semantic_conformance_hook._summary)
                print(json.dumps({'outcomes': outcomes, 'entries': deltas,
                                  'observed': [event['observed'] for event in observations]}))
            """,
            pythonpath=(str(harness_root),),
        )
        self.assertEqual(result["outcomes"], [True, False, False])
        self.assertEqual(result["entries"], [1, 1, 1])
        self.assertEqual(result["observed"], [2, 2, 2])

    def run_child(self, body: str, *, threshold="1000000", pythonpath=()):
        env = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith(("CINDERX_", "PYTHONJIT", "PARALLEL_GC_"))
        }
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = threshold
        if pythonpath:
            inherited_path = env.get("PYTHONPATH")
            env["PYTHONPATH"] = os.pathsep.join(
                [*pythonpath, *([inherited_path] if inherited_path else [])]
            )
        proc = subprocess.run(
            [sys.executable, "-c", textwrap.dedent(body)],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        return json.loads(proc.stdout.strip().splitlines()[-1])

    def test_compile_diagnostic_compiles_or_returns_typed_refusal(self):
        result = self.run_child(
            """
            import json
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def simple(value):
                return value + 1

            def subscr(value):
                return value[0]

            async def async_fn():
                return 1

            def sliced(value):
                return value[1:]

            before = _cinderx._get_trigger_stats()
            payload = {}
            deltas = []
            for name, fn in (("simple", simple), ("subscr", subscr),
                             ("async", async_fn), ("sliced", sliced)):
                prior = _cinderx._get_trigger_stats()
                payload[name] = cinderjit._jit311_compile_diagnostic(fn)
                current = _cinderx._get_trigger_stats()
                deltas.append(current["compiled_function_creations"] -
                              prior["compiled_function_creations"])
            payload["creation_deltas"] = deltas
            after = _cinderx._get_trigger_stats()
            payload["creations_delta"] = (
                after["compiled_function_creations"]
                - before["compiled_function_creations"])
            payload["alloc_delta"] = (
                after["executable_alloc_calls"]
                - before["executable_alloc_calls"])
            entry_before = _cinderx._get_trigger_stats()["machine_code_entries"]
            payload["values"] = [simple(4), subscr([42])]
            payload["entries_delta"] = (
                _cinderx._get_trigger_stats()["machine_code_entries"] - entry_before)
            payload["compiled"] = [cinderjit.is_jit_compiled(fn)
                                   for fn in (simple, subscr)]
            print(json.dumps(payload, sort_keys=True))
            """
        )
        self.assertEqual(result["creation_deltas"], [1, 1, 0, 1])
        self.assertEqual(result["creations_delta"], 3)
        self.assertEqual(result["values"], [5, 42])
        self.assertEqual(result["compiled"], [True, True])
        self.assertEqual(result["entries_delta"], 2)
        self.assertEqual(result["alloc_delta"], 1)
        self.assertEqual(
            result["simple"],
            {
                "eligible": True,
                "compiled": True,
                "phase": "compiler",
                "reason": None,
                "opcode": None,
                "offset": None,
            },
        )
        self.assertEqual(result["subscr"], result["simple"])
        self.assertEqual(result["async"]["reason"], "REFUSE_SHAPE_ASYNC_CODE")
        self.assertFalse(result["async"]["eligible"])
        self.assertEqual(result["sliced"], result["simple"])

    def test_initialization_keeps_setup_interpreted_and_resumes_autojit(self):
        result = self.run_child(
            """
            import json, _cinderx
            before = _cinderx._get_trigger_stats()
            import cinderx
            cinderx.init()
            after = _cinderx._get_trigger_stats()
            assert cinderx.is_initialized()
            assert cinderx._autojit_setup_depth() == 0
            import cinderjit

            def hot(value, step):
                return value + step

            # Exercise post-startup scheduling with MR8's 100 ms grace intact.
            import time
            time.sleep(0.2)
            entries = after["machine_code_entries"]
            for i in range(40):
                assert hot(i, 1) == i + 1
            assert cinderjit.is_jit_compiled(hot)
            assert _cinderx._get_trigger_stats()["machine_code_entries"] > entries
            print(json.dumps({
                "initialization_deopts": after["organic_deopt_hits"] -
                                         before["organic_deopt_hits"],
                "compiled": cinderjit.is_jit_compiled(hot),
            }))
            """,
            threshold="8",
        )
        self.assertEqual(result["initialization_deopts"], 0)
        self.assertTrue(result["compiled"])

    def test_initialization_failure_and_nested_setup_restore_scheduling(self):
        result = self.run_child(
            """
            import json, _cinderx, cinderx, cinderjit
            cinderx.init()
            original = cinderx._install_autojit_import_provider

            def fail_setup():
                assert cinderx._autojit_setup_depth() == 1
                raise RuntimeError("injected setup failure")

            cinderx._is_init = False
            cinderx._install_autojit_import_provider = fail_setup
            try:
                cinderx.init()
            except RuntimeError as exc:
                assert str(exc) == "injected setup failure"
            else:
                raise AssertionError("initialization error was swallowed")
            finally:
                cinderx._install_autojit_import_provider = original
            assert not cinderx.is_initialized()
            assert cinderx._autojit_setup_depth() == 0

            def after_failure(value, step):
                return value + step

            import time
            time.sleep(0.2)
            before = _cinderx._get_trigger_stats()["machine_code_entries"]
            for i in range(40):
                assert after_failure(i, 1) == i + 1
            assert cinderjit.is_jit_compiled(after_failure)
            assert _cinderx._get_trigger_stats()["machine_code_entries"] > before

            # An enclosing provider's scope must survive our nested setup.
            cinderx._autojit_setup_enter()
            try:
                cinderx.init()
                assert cinderx.is_initialized()
                assert cinderx._autojit_setup_depth() == 1
            finally:
                cinderx._autojit_setup_leave()
            assert cinderx._autojit_setup_depth() == 0
            print(json.dumps({"restored": True}))
            """,
            threshold="8",
        )
        self.assertTrue(result["restored"])

    def test_compile_diagnostic_reports_runtime_fallback(self):
        result = self.run_child(
            """
            import json, sys
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def target(value):
                return value + 1

            def tracer(frame, event, arg):
                return tracer

            sys.settrace(tracer)
            traced = cinderjit._jit311_compile_diagnostic(target)
            sys.settrace(None)
            untraced = cinderjit._jit311_compile_diagnostic(target)
            print(json.dumps({"traced": traced, "untraced": untraced}, sort_keys=True))
            """
        )
        self.assertEqual(result["traced"]["phase"], "runtime")
        self.assertEqual(result["traced"]["reason"], "TRACING_ACTIVE")
        self.assertIsNone(result["untraced"]["reason"])

    def test_compile_diagnostic_rejects_non_function(self):
        result = self.run_child(
            """
            import json
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit
            try:
                cinderjit._jit311_compile_diagnostic(42)
            except TypeError as exc:
                payload = {"type": type(exc).__name__, "message": str(exc)}
            else:
                raise SystemExit("non-function diagnostic succeeded")
            print(json.dumps(payload, sort_keys=True))
            """
        )
        self.assertEqual(result["type"], "TypeError")
        self.assertIn("expected a Python function", result["message"])

    def test_execute_surface_and_per_code_entry_ledger(self):
        result = self.run_child(
            """
            import dis, json
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            surface = [dis.opname[op] for op in cinderjit._jit311_execute_surface()]
            cinderjit._jit311_reset_entry_ledger()

            def target(value):
                return value + 1

            diagnostic = cinderjit._jit311_compile_diagnostic(target)
            assert diagnostic["compiled"]
            assert [target(value) for value in range(3)] == [1, 2, 3]
            ledger = cinderjit._jit311_entry_ledger()
            rows = [
                row for row in ledger["entries"]
                if row["filename"] == target.__code__.co_filename
                and row["firstlineno"] == target.__code__.co_firstlineno
                and row["qualname"] == target.__code__.co_qualname
            ]
            print(json.dumps({
                "surface": surface,
                "entries": rows[0]["entries"] if len(rows) == 1 else None,
                "dropped": ledger["dropped"],
            }, sort_keys=True))
            """
        )
        self.assertIn("LOAD_ATTR", result["surface"])
        # The surface reports backend capability; per-function admission still
        # rejects unprofitable shapes (covered by compile_diagnostic above).
        self.assertIn("BINARY_SUBSCR", result["surface"])
        self.assertIn("STORE_SUBSCR", result["surface"])
        self.assertEqual(result["entries"], 3)
        self.assertEqual(result["dropped"], 0)
