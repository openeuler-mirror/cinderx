import sys
import unittest

from ci_pipeline.jit311.semantic_conformance_adapters import (
    REFCYCLE_TEST,
    refcycle_refcount_contract,
)


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
