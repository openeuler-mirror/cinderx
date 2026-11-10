# Copyright (c) Meta Platforms, Inc. and affiliates.
import gc
import os
import sys
import unittest
import weakref

cinderjit = None


@unittest.skipUnless(sys.version_info[:2] == (3, 11), "CPython 3.11 admission")
@unittest.skipUnless(
    os.environ.get("CINDERX_JIT_MODE") in ("execute", "canary"),
    "requires the dedicated execute-mode gate",
)
class AdmissionSemantics311Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        global cinderjit
        import cinderx

        cinderx.init()
        import _cinderx

        _cinderx.install_frame_evaluator()
        import cinderjit as native_jit

        cinderjit = native_jit
        if not cinderjit.is_enabled():
            raise AssertionError("admission tests must execute the JIT")

    def compile(self, fn):
        cinderjit.force_compile(fn)
        self.assertTrue(cinderjit.is_jit_compiled(fn), fn.__name__)
        return fn

    def test_format_conversions_and_dynamic_spec(self):
        @self.compile
        def fmt(value, width):
            return f"[{value!s}] [{value!r}] [{value!a}] {17:0{width}d}"

        self.assertEqual(fmt("é", 4), "[é] ['é'] ['\\xe9'] 0017")

    def test_format_callbacks_once_and_in_order(self):
        events = []

        class Value:
            def __format__(self, spec):
                events.append(("format", spec))
                return "formatted"

            def __str__(self):
                events.append("str")
                return "string"

            def __repr__(self):
                events.append("repr")
                return "representation"

        @self.compile
        def fmt(value):
            return f"{value:hello}:{value!s}:{value!r}"

        self.assertEqual(fmt(Value()), "formatted:string:representation")
        self.assertEqual(events, [("format", "hello"), "str", "repr"])

    def test_format_exception_and_invalid_return(self):
        events = []
        error = ValueError("format failed")

        class Raises:
            def __format__(self, spec):
                events.append(spec)
                raise error

        class Invalid:
            def __format__(self, spec):
                return 42

        @self.compile
        def fmt(value):
            return f"prefix:{value:spec}:suffix"

        with self.assertRaises(ValueError) as caught:
            fmt(Raises())
        self.assertIs(caught.exception, error)
        self.assertEqual(events, ["spec"])
        with self.assertRaises(TypeError):
            fmt(Invalid())

    def test_slices_boundaries_and_types(self):
        @self.compile
        def sliced(value, start, stop, step):
            return value[start:stop:step]

        for value in (list(range(8)), tuple(range(8)), "abcdefgh", b"abcdefgh"):
            for start, stop, step in (
                (None, None, None),
                (None, None, -1),
                (-100, 100, 2),
                (10**100, -(10**100), -2),
                (3, 3, 1),
            ):
                with self.subTest(value=type(value), bounds=(start, stop, step)):
                    self.assertEqual(
                        sliced(value, start, stop, step), value[slice(start, stop, step)]
                    )
            with self.assertRaises(ValueError):
                sliced(value, None, None, 0)
        with self.assertRaises(TypeError):
            sliced([1, 2], 1.5, None, None)

    def test_slice_index_callbacks_and_error(self):
        events = []

        class Index:
            def __init__(self, name, value):
                self.name, self.value = name, value

            def __index__(self):
                events.append(self.name)
                return self.value

        @self.compile
        def sliced(value, start, stop, step):
            return value[start:stop:step]

        self.assertEqual(
            sliced(
                list(range(10)), Index("start", 1), Index("stop", 8), Index("step", 2)
            ),
            [1, 3, 5, 7],
        )
        self.assertEqual(events, ["step", "start", "stop"])
        events.clear()
        with self.assertRaises(TypeError):
            sliced([1, 2], Index("bad", "not an int"), None, None)
        self.assertEqual(events, ["bad"])

    def test_user_container_receives_unconverted_slice(self):
        events = []
        start, stop, step = object(), object(), object()

        class Container:
            def __getitem__(self, key):
                events.append(key)
                return key.start, key.stop, key.step

        @self.compile
        def sliced(value, a, b, c):
            return value[a:b:c]

        self.assertEqual(
            sliced(Container(), start, stop, step), (start, stop, step)
        )
        self.assertEqual(len(events), 1)
        self.assertIsInstance(events[0], slice)

    def test_slice_assignment(self):
        @self.compile
        def assign(value, start, stop, replacement):
            value[start:stop] = replacement
            return value

        value = [0, 1, 2, 3]
        self.assertIs(assign(value, 1, 3, [8]), value)
        self.assertEqual(value, [0, 8, 3])
        with self.assertRaises(TypeError):
            assign((0, 1, 2), 0, 1, [8])

    def test_accumulator_overflow_and_type_changes(self):
        def multiply(value):
            value *= 2
            return value

        for _ in range(64):
            self.assertEqual(multiply(3), 6)
        self.compile(multiply)
        values = (
            0, -1, 2**62, 2**63, -(2**63), 10**100,
            True, False, 1.25, "ab", [1, 2],
        )
        for value in values:
            with self.subTest(value=value):
                # A fresh list avoids comparing the same mutated input twice.
                actual = multiply(value.copy() if isinstance(value, list) else value)
                self.assertEqual(actual, value * 2)

    def test_large_local_loop_overflow(self):
        def loop(value, n, a, b, c, d, e, f, g):
            for i in range(n):
                value += 2
            return value, a, b, c, d, e, f, g

        for _ in range(64):
            loop(0, 4, 1, 2, 3, 4, 5, 6, 7)
        self.compile(loop)
        for value in (2**63 - 3, -(2**63), 10**100, 0.5):
            self.assertEqual(
                loop(value, 4, 1, 2, 3, 4, 5, 6, 7),
                (value + 8, 1, 2, 3, 4, 5, 6, 7),
            )

    def test_inplace_identity_and_callback_exception(self):
        events = []

        class Value:
            def __imul__(self, other):
                events.append(other)
                return self

        @self.compile
        def multiply(value):
            value *= 2
            return value

        value = Value()
        self.assertIs(multiply(value), value)
        self.assertEqual(events, [2])

        class Raises:
            def __imul__(self, other):
                raise LookupError("inplace")

        with self.assertRaisesRegex(LookupError, "inplace"):
            multiply(Raises())
        self.assertEqual(multiply(5), 10)

    def test_slice_does_not_retain_temporary_objects(self):
        class Value:
            pass

        @self.compile
        def sliced(value):
            return value[:]

        obj = Value()
        ref = weakref.ref(obj)
        result = sliced([obj])
        del obj, result
        gc.collect()
        self.assertIsNone(ref())


if __name__ == "__main__":
    unittest.main()
