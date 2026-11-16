import sys
import unittest

from ci_pipeline.jit311.semantic_conformance_adapters import (
    CALL_DISASSEMBLY_TESTS,
    REFCYCLE_TEST,
    call_disassembly_contract,
    install_call_disassembly_contract,
    install_dis_worker_contract,
    refcycle_refcount_contract,
)


@unittest.skipUnless(sys.version_info[:3] == (3, 11, 6), "CP311.6 disassembly contract")
class CallDisassemblyContractTests(unittest.TestCase):
    stock = (
        "  604 RESUME_QUICK\n"
        "              38 CALL_PY_WITH_DEFAULTS     1\n"
        "              48 POP_TOP\n"
    )
    adaptive = stock.replace("CALL_PY_WITH_DEFAULTS     ", "CALL_ADAPTIVE            ")

    def case(self, name=None):
        case = unittest.TestCase()
        case.id = lambda: name or sorted(CALL_DISASSEMBLY_TESTS)[0]
        return case

    def test_exact_cases_compare_the_whole_snapshot_and_restore(self):
        for name in CALL_DISASSEMBLY_TESTS:
            with self.subTest(name=name):
                case = self.case(name)
                with call_disassembly_contract(case, evaluator_installed=True) as seen:
                    case.assertEqual(self.adaptive, self.stock)
                    self.assertEqual(seen["observed"], "CALL_ADAPTIVE")
                    with self.assertRaises(AssertionError):
                        case.assertEqual(self.adaptive, self.stock)
                self.assertNotIn("assertEqual", vars(case))

    def test_ineligible_cases_and_non_builtin_strings_still_fail(self):
        class String(str):
            pass

        for name, installed, actual, expected in [
            ("another.test", True, self.adaptive, self.stock),
            (None, False, self.adaptive, self.stock),
            (None, True, String(self.adaptive), self.stock),
            (None, True, self.adaptive, String(self.stock)),
            (None, True, [self.adaptive], [self.stock]),
            (None, True, self.stock, self.adaptive),
        ]:
            with self.subTest(name=name, installed=installed, actual=type(actual)):
                case = self.case(name)
                with call_disassembly_contract(case, evaluator_installed=installed):
                    with self.assertRaises(AssertionError):
                        case.assertEqual(actual, expected)

    def test_other_opcode_offset_argument_or_text_changes_still_fail(self):
        for actual, expected in [
            (self.adaptive.replace("38 ", "40 "), self.stock),
            (self.adaptive.replace("1\n", "2\n"), self.stock),
            (self.adaptive.replace("RESUME_QUICK", "RESUME"), self.stock),
            (self.adaptive + "extra\n", self.stock),
            (
                self.adaptive,
                self.stock.replace("CALL_PY_WITH_DEFAULTS", "CALL_PY_EXACT_ARGS"),
            ),
            (self.adaptive * 2, self.stock * 2),
        ]:
            with self.subTest(actual=actual, expected=expected):
                case = self.case()
                with call_disassembly_contract(case, evaluator_installed=True) as seen:
                    with self.assertRaises(AssertionError):
                        case.assertEqual(actual, expected)
                    self.assertEqual(seen, {})

    def test_passing_first_comparison_does_not_adapt_a_later_difference(self):
        case = self.case()
        with call_disassembly_contract(case, evaluator_installed=True) as seen:
            case.assertEqual(self.stock, self.stock)
            with self.assertRaises(AssertionError):
                case.assertEqual(self.adaptive, self.stock)
            self.assertEqual(seen, {})

    def test_original_assertion_message_and_exception_are_preserved(self):
        case = self.case()
        original = case.assertEqual
        calls = []

        def record(actual, expected, msg=None):
            calls.append((actual, expected, msg))
            return original(actual, expected, msg)

        case.assertEqual = record
        with self.assertRaisesRegex(RuntimeError, "downstream"):
            with call_disassembly_contract(case, evaluator_installed=True):
                case.assertEqual(self.adaptive, self.stock, "snapshot")
                raise RuntimeError("downstream")
        self.assertIs(case.assertEqual, record)
        self.assertEqual(calls, [(self.adaptive, self.adaptive, "snapshot")])

    def test_other_python_version_keeps_the_original_comparison(self):
        from unittest.mock import patch

        case = self.case()
        with patch.object(sys, "version_info", (3, 14, 3)):
            with call_disassembly_contract(case, evaluator_installed=True):
                with self.assertRaises(AssertionError):
                    case.assertEqual(self.adaptive, self.stock)

    def test_installation_is_idempotent(self):
        from unittest.mock import patch

        with patch.object(
            unittest.TestCase, "_callTestMethod", unittest.TestCase._callTestMethod
        ):
            install_call_disassembly_contract()
            installed = unittest.TestCase._callTestMethod
            install_call_disassembly_contract()
            self.assertIs(unittest.TestCase._callTestMethod, installed)

    def test_installed_hook_does_not_query_other_tests_id_attributes(self):
        from unittest.mock import patch

        events = []

        class NonDisCase(unittest.TestCase):
            def setUp(self):
                self.id = 1

            def runTest(self):
                events.append("ran")

        with patch.object(
            unittest.TestCase, "_callTestMethod", unittest.TestCase._callTestMethod
        ):
            install_call_disassembly_contract()
            result = unittest.TestResult()
            NonDisCase().run(result)
        self.assertEqual(result.testsRun, 1)
        self.assertEqual(result.errors, [])
        self.assertEqual(result.failures, [])
        self.assertEqual(events, ["ran"])

    def test_only_test_dis_workers_install_the_hook(self):
        import json
        from unittest.mock import patch

        module = "ci_pipeline.jit311.semantic_conformance_adapters"
        cases = [
            (["-m", "test"], False),
            (["--worker-args", "invalid json"], False),
        ]
        for target, expected in [
            ("test_dis", True),
            ("test.test_dis", True),
            ("test_unittest", False),
            ("test__xxsubinterpreters", False),
        ]:
            payload = json.dumps([{}, target])
            cases.extend(
                [
                    (["--worker-args", payload], expected),
                    (["--worker-args=" + payload], expected),
                ]
            )
        for arguments, expected in cases:
            with self.subTest(arguments=arguments):
                with (
                    patch.object(sys, "argv", ["python", *arguments]),
                    patch(module + ".install_call_disassembly_contract") as install,
                ):
                    self.assertEqual(install_dis_worker_contract(), expected)
                    self.assertEqual(install.call_count, int(expected))


@unittest.skipUnless(sys.version_info[:2] == (3, 11), "CP311 assertion contract")
class RefcycleContractTests(unittest.TestCase):
    def case(self, name=REFCYCLE_TEST):
        case = unittest.TestCase()
        case.id = lambda: name
        return case

    def test_only_first_exact_pair_is_adapted_and_recorded(self):
        case = self.case()
        with refcycle_refcount_contract(case, compiled=True) as observation:
            case.assertGreater(2, 2)
            self.assertEqual(observation, {"observed": 2, "interpreter_equivalent": 3})
            with self.assertRaises(AssertionError):
                case.assertGreater(2, 2)
        self.assertNotIn("assertGreater", vars(case))

    def test_interpreter_other_test_and_unexpected_counts_still_fail(self):
        for name, compiled, first, second in [
            (REFCYCLE_TEST, False, 2, 2),
            ("another.test", True, 2, 2),
            (REFCYCLE_TEST, True, 1, 2),
            (REFCYCLE_TEST, True, 2, 3),
            (REFCYCLE_TEST, True, 2.0, 2),
        ]:
            with self.subTest(name=name, compiled=compiled, values=(first, second)):
                case = self.case(name)
                with self.assertRaises(AssertionError):
                    with refcycle_refcount_contract(case, compiled=compiled):
                        case.assertGreater(first, second)
                self.assertNotIn("assertGreater", vars(case))

    def test_passing_original_check_does_not_adapt_later_assertions(self):
        case = self.case()
        with refcycle_refcount_contract(case, compiled=True) as observation:
            case.assertGreater(3, 2)
            with self.assertRaises(AssertionError):
                case.assertGreater(2, 2)
            self.assertEqual(observation, {})

    def test_downstream_failures_propagate_and_restore_instance_attribute(self):
        for assertion, args in [
            ("assertFalse", (True,)),
            ("assertTrue", (False,)),
            ("assertEqual", ([object()], [])),
        ]:
            with self.subTest(assertion=assertion):
                case = self.case()
                original = case.assertGreater
                case.assertGreater = original
                with self.assertRaises(AssertionError):
                    with refcycle_refcount_contract(case, compiled=True):
                        case.assertGreater(2, 2)
                        getattr(case, assertion)(*args)
                self.assertIs(case.assertGreater, original)


if __name__ == "__main__":
    unittest.main()
