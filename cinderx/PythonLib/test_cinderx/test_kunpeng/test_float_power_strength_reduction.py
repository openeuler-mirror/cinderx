# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import math
import random
import struct
import unittest
from typing import Any, Callable

import cinderx
import cinderx.jit
from cinderx.test_support import fail_if_deopt, passIf
from test_cinderx.test_jit_specialization import specialize


PowerFunc = Callable[[Any], Any]


def _oracle(x: Any, exponent: float) -> Any:
    return (x * 1.0) ** exponent


cinderx.jit.jit_suppress(_oracle)


def _pow_p05(x: Any) -> Any:
    return (x * 1.0) ** 0.5


def _pow_p10(x: Any) -> Any:
    return (x * 1.0) ** 1.0


def _pow_p15(x: Any) -> Any:
    return (x * 1.0) ** 1.5


def _pow_p20(x: Any) -> Any:
    return (x * 1.0) ** 2.0


def _pow_p30(x: Any) -> Any:
    return (x * 1.0) ** 3.0


def _pow_n05(x: Any) -> Any:
    return (x * 1.0) ** -0.5


def _pow_n10(x: Any) -> Any:
    return (x * 1.0) ** -1.0


def _pow_n15(x: Any) -> Any:
    return (x * 1.0) ** -1.5


def _pow_n20(x: Any) -> Any:
    return (x * 1.0) ** -2.0


POWER_CASES: tuple[tuple[PowerFunc, float, tuple[float, ...]], ...] = (
    (_pow_p05, 0.5, (4.0, 16.0)),
    (_pow_p10, 1.0, (-4.0, 2.0)),
    (_pow_p15, 1.5, (4.0, 16.0)),
    (_pow_p20, 2.0, (-4.0, 2.0)),
    (_pow_p30, 3.0, (-4.0, 2.0)),
    (_pow_n05, -0.5, (4.0, 16.0)),
    (_pow_n10, -1.0, (-4.0, 2.0)),
    (_pow_n15, -1.5, (4.0, 16.0)),
    (_pow_n20, -2.0, (-4.0, 2.0)),
)

FRACTIONAL_CASES: tuple[tuple[PowerFunc, float], ...] = (
    (_pow_p05, 0.5),
    (_pow_p15, 1.5),
    (_pow_n05, -0.5),
    (_pow_n15, -1.5),
)

NEGATIVE_CASES: tuple[tuple[PowerFunc, float], ...] = (
    (_pow_n05, -0.5),
    (_pow_n10, -1.0),
    (_pow_n15, -1.5),
    (_pow_n20, -2.0),
)

POSITIVE_CASES: tuple[tuple[PowerFunc, float], ...] = (
    (_pow_p05, 0.5),
    (_pow_p10, 1.0),
    (_pow_p15, 1.5),
    (_pow_p20, 2.0),
    (_pow_p30, 3.0),
)


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
class FloatPowerStrengthReductionTests(unittest.TestCase):
    def setUp(self) -> None:
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self) -> None:
        cinderx.jit.disable_specialized_opcodes()

    def _specialize(self, func: PowerFunc) -> None:
        specialize(func, lambda: func(4.0))
        self.assertTrue(cinderx.jit.is_jit_compiled(func))

    def test_runtime_power_preserves_exact_results_and_hir(
        self,
    ) -> None:
        # Counts include the leading x * 1.0 that makes the base FloatExact.
        for func, exponent, values in POWER_CASES:
            with self.subTest(exponent=exponent):
                self._specialize(func)

                counts = cinderx.jit.get_function_hir_opcode_counts(func)
                self.assertIsNotNone(counts)
                assert counts is not None
                self.assertEqual(
                    counts.get("DoubleBinaryOp", 0),
                    2,
                    dict(counts),
                )
                self.assertEqual(counts.get("FloatBinaryOp", 0), 0, dict(counts))
                self.assertEqual(counts.get("BinaryOp", 0), 0, dict(counts))

                checked = fail_if_deopt(func)
                # These finite inputs expose sqrt/multiply/reciprocal rounding
                # differences that the old exact-square-only cases missed.
                for value in (*(v for v in values if v > 0.0),
                              2.0, 3.0, 0.1, 1.0000000000000002,
                              float.fromhex("0x1.8aa959fd0b754p+8")):
                    self.assertEqual(
                        struct.pack("!d", checked(value)),
                        struct.pack("!d", _oracle(value, exponent)),
                    )
                # Negative integral bases use the Python path. Prove that
                # the input guard actually exits, and still compare bits.
                for value in (v for v in values if v < 0.0):
                    self._specialize(func)
                    cinderx.jit.get_and_clear_runtime_stats()
                    actual = func(value)
                    deopts = cinderx.jit.get_and_clear_runtime_stats()["deopt"]
                    self.assertTrue(any(d["normal"]["reason"] == "GuardFailure"
                                        for d in deopts), deopts)
                    self.assertEqual(struct.pack("!d", actual),
                                     struct.pack("!d", _oracle(value, exponent)))

    def test_random_positive_inputs_match_bits_without_deopt(self) -> None:
        rng = random.Random(231)
        values = [math.ldexp(rng.uniform(1.0, 2.0), rng.randrange(-100, 100))
                  for _ in range(256)]
        for func, exponent, _ in POWER_CASES:
            self._specialize(func)
            checked = fail_if_deopt(func)
            for value in values:
                self.assertEqual(struct.pack("!d", checked(value)),
                                 struct.pack("!d", _oracle(value, exponent)),
                                 (value, exponent))

    def test_guard_exit_preserves_same_frame_handlers(self) -> None:
        def inverse(x: Any) -> Any:
            try:
                return (x * 1.0) ** -0.5
            except ZeroDivisionError:
                return "zero"

        def overflow(x: Any) -> Any:
            try:
                return (x * 1.0) ** 1.5
            except OverflowError:
                return "overflow"

        for func, value, expected in (
            (inverse, 0.0, "zero"),
            (inverse, -0.0, "zero"),
            (overflow, 1e250, "overflow"),
        ):
            self._specialize(func)
            counts = cinderx.jit.get_function_hir_opcode_counts(func)
            self.assertGreaterEqual(counts.get("DoubleBinaryOp", 0), 2)
            cinderx.jit.get_and_clear_runtime_stats()
            self.assertEqual(func(value), expected)
            deopts = cinderx.jit.get_and_clear_runtime_stats()["deopt"]
            self.assertTrue(any(d["normal"]["reason"] == "GuardFailure"
                                for d in deopts), deopts)

    def test_fractional_power_result_can_feed_arithmetic(self) -> None:
        def consumer(x: Any) -> Any:
            return ((x * 1.0) ** 0.5) + 1.0

        for value in (-4.0, 4.0, -2.0, 2.0):
            self._specialize(consumer)
            expected = _oracle(value, 0.5) + 1.0
            actual = consumer(value)
            self.assertIs(type(actual), type(expected))
            if isinstance(expected, complex):
                self.assertEqual(struct.pack("!d", actual.real),
                                 struct.pack("!d", expected.real))
                self.assertEqual(struct.pack("!d", actual.imag),
                                 struct.pack("!d", expected.imag))
            else:
                self.assertEqual(struct.pack("!d", actual),
                                 struct.pack("!d", expected))

    def test_negative_fractional_base_falls_back_to_complex_power(self) -> None:
        for func, exponent in FRACTIONAL_CASES:
            with self.subTest(exponent=exponent):
                self._specialize(func)
                result = func(-4.0)
                self.assertIs(type(result), complex)
                self.assertEqual(result, (-4.0) ** exponent)

    def test_signed_zero_negative_exponents_raise(self) -> None:
        for func, exponent in NEGATIVE_CASES:
            for value in (0.0, -0.0):
                with self.subTest(exponent=exponent, value=value):
                    self._specialize(func)
                    with self.assertRaises(ZeroDivisionError):
                        func(value)

    def test_negative_zero_positive_exponents_preserve_python_sign(self) -> None:
        for func, exponent in POSITIVE_CASES:
            with self.subTest(exponent=exponent):
                self._specialize(func)
                result = func(-0.0)
                expected = (-0.0) ** exponent
                self.assertEqual(result, expected)
                self.assertEqual(
                    math.copysign(1.0, result), math.copysign(1.0, expected)
                )

    def test_result_overflow_raises(self) -> None:
        cases: tuple[tuple[PowerFunc, float], ...] = (
            (_pow_p15, 1e250),
            (_pow_p20, 1e200),
            (_pow_p30, -1e200),
            (_pow_n10, 1e-320),
            (_pow_n10, -1e-320),
            (_pow_n15, 1e-250),
            (_pow_n20, 1e-200),
        )
        for func, value in cases:
            with self.subTest(func=func.__name__, value=value):
                self._specialize(func)
                with self.assertRaises(OverflowError):
                    func(value)

    def test_intermediate_overflow_falls_back_to_subnormal_result(self) -> None:
        cases: tuple[tuple[PowerFunc, float, float], ...] = (
            (_pow_n15, 1e210, -1.5),
            (_pow_n20, 1e160, -2.0),
        )
        for func, value, exponent in cases:
            with self.subTest(exponent=exponent):
                self._specialize(func)
                expected = value**exponent
                self.assertNotEqual(expected, 0.0)
                self.assertEqual(func(value), expected)

    def test_nan_and_infinities_match_python(self) -> None:
        nan = float("nan")
        infinities = (float("inf"), float("-inf"))

        for func, exponent, _ in POWER_CASES:
            with self.subTest(exponent=exponent, value="nan"):
                self._specialize(func)
                self.assertTrue(math.isnan(func(nan)))

            for value in infinities:
                with self.subTest(exponent=exponent, value=value):
                    self._specialize(func)
                    result = func(value)
                    expected = value**exponent
                    self.assertEqual(result, expected)
                    if expected == 0.0 or math.isinf(expected):
                        self.assertEqual(
                            math.copysign(1.0, result),
                            math.copysign(1.0, expected),
                        )


if __name__ == "__main__":
    unittest.main()
