# Copyright (c) Meta Platforms, Inc. and affiliates.

"""CPython 3.11 custom interpreter loop and PEP 523 entry-point ownership."""

import sys
import unittest


def _specialization_target(value, offset=0):
    """Module-level callee with a default, reached via LOAD_GLOBAL.

    The global lookup can specialize to LOAD_GLOBAL_MODULE, while calls
    with either explicit or defaulted arguments must retain CALL_ADAPTIVE
    under the CinderX evaluator so the callee still enters PEP 523.
    """
    return value + offset


@unittest.skipUnless(
    sys.version_info[:3] == (3, 11, 6),
    "the CPython 3.11 evaluator is pinned to 3.11.6",
)
class Interpreter311Tests(unittest.TestCase):
    """Code executed while the CinderX evaluator is installed."""

    @classmethod
    def setUpClass(cls) -> None:
        import _cinderx
        import cinderx

        cinderx.init()
        cls._cinderx = _cinderx
        cls._installed_here = not _cinderx.is_frame_evaluator_installed()
        if cls._installed_here:
            _cinderx.install_frame_evaluator()

    @classmethod
    def tearDownClass(cls) -> None:
        if cls._installed_here:
            cls._cinderx.remove_frame_evaluator()

    def test_evaluator_is_installed(self) -> None:
        self.assertTrue(self._cinderx.is_frame_evaluator_installed())

    def test_call_and_arguments(self) -> None:
        def add(a, b=2, *rest, kw=8, **kwargs):
            return a + b + sum(rest) + kw + sum(kwargs.values())

        self.assertEqual(add(1), 11)
        self.assertEqual(add(1, 2, 3, 4, kw=5, extra=6), 21)

    def test_exception_type_message_and_traceback(self) -> None:
        def fail():
            raise ValueError("formal-311-boom")

        frames = []
        try:
            fail()
        except ValueError as exc:
            self.assertEqual(str(exc), "formal-311-boom")
            traceback = exc.__traceback__
            while traceback is not None:
                frames.append(traceback.tb_frame.f_code.co_name)
                traceback = traceback.tb_next
        else:
            self.fail("fail() did not raise")

        # The traceback runs from the catching frame down to the raising one.
        self.assertEqual(frames[0], "test_exception_type_message_and_traceback")
        self.assertEqual(frames[-1], "fail")

    def test_exception_is_catchable_in_a_loop(self) -> None:
        # Exercises the exception table on a path that raises and recovers
        # many times, which is where a mis-wired evaluator shows up first.
        caught = 0
        for index in range(100):
            try:
                if index % 3:
                    raise KeyError(index)
            except KeyError:
                caught += 1
        self.assertEqual(caught, 66)

    def test_generator_protocol(self) -> None:
        def gen():
            received = yield 10
            yield received + 1

        instance = gen()
        self.assertEqual(next(instance), 10)
        self.assertEqual(instance.send(41), 42)
        with self.assertRaises(StopIteration):
            next(instance)

    def test_generator_close_and_throw(self) -> None:
        def gen():
            try:
                yield 1
            except RuntimeError:
                yield 2

        instance = gen()
        next(instance)
        self.assertEqual(instance.throw(RuntimeError("x")), 2)

        other = gen()
        next(other)
        other.close()

    def test_global_specializes_while_ordinary_calls_keep_adaptive_backoff(
        self,
    ) -> None:
        # LOAD_GLOBAL may specialize, but the evaluator rejects ordinary
        # CALL specialization. Both argument shapes must keep that backoff.
        import dis

        for arguments, expected in (("i", 18), ("i, 3", 45)):
            with self.subTest(arguments=arguments):
                namespace = {"_specialization_target": _specialization_target}
                exec(
                    "def loop():\n"
                    "    total = 0\n"
                    "    for i in [1, 2, 3] * 3:\n"
                    f"        total += _specialization_target({arguments})\n"
                    "    return total\n",
                    namespace,
                )
                loop = namespace["loop"]

                for _ in range(30):
                    self.assertEqual(loop(), expected)

                instructions = list(
                    dis.get_instructions(loop, adaptive=True, show_caches=True)
                )
                self.assertIn("LOAD_GLOBAL_MODULE", [i.opname for i in instructions])
                calls = [
                    index
                    for index, instr in enumerate(instructions)
                    if instr.opname.startswith("CALL")
                ]
                self.assertEqual(len(calls), 1)
                call = calls[0]
                self.assertEqual(instructions[call].opname, "CALL_ADAPTIVE")
                self.assertEqual(instructions[call + 1].opname, "CACHE")
                self.assertRegex(instructions[call + 1].argrepr, r"^counter: [1-9]\d*$")

    def test_frame_introspection(self) -> None:
        def inner():
            frame = sys._getframe()
            return frame.f_code.co_name, frame.f_back.f_code.co_name

        self.assertEqual(inner(), ("inner", "test_frame_introspection"))


@unittest.skipUnless(
    sys.version_info[:3] == (3, 11, 6),
    "the CPython 3.11 evaluator is pinned to 3.11.6",
)
class EvalHookOwnership311Tests(unittest.TestCase):
    """Installing and removing the PEP 523 entry point."""

    def setUp(self) -> None:
        import _cinderx
        import cinderx

        cinderx.init()
        self._cinderx = _cinderx
        self._was_installed = _cinderx.is_frame_evaluator_installed()

    def tearDown(self) -> None:
        installed = self._cinderx.is_frame_evaluator_installed()
        if self._was_installed and not installed:
            self._cinderx.install_frame_evaluator()
        elif not self._was_installed and installed:
            self._cinderx.remove_frame_evaluator()

    def test_install_remove_round_trip(self) -> None:
        if self._was_installed:
            self._cinderx.remove_frame_evaluator()
        self.assertFalse(self._cinderx.is_frame_evaluator_installed())

        self._cinderx.install_frame_evaluator()
        self.assertTrue(self._cinderx.is_frame_evaluator_installed())

        # Code keeps running correctly on either side of the switch.
        self.assertEqual(sum(i * i for i in range(10)), 285)

        self._cinderx.remove_frame_evaluator()
        self.assertFalse(self._cinderx.is_frame_evaluator_installed())
        self.assertEqual(sum(i * i for i in range(10)), 285)

    def test_install_is_idempotent(self) -> None:
        self._cinderx.install_frame_evaluator()
        self._cinderx.install_frame_evaluator()
        self.assertTrue(self._cinderx.is_frame_evaluator_installed())

    def test_remove_is_idempotent(self) -> None:
        self._cinderx.install_frame_evaluator()
        self._cinderx.remove_frame_evaluator()
        self._cinderx.remove_frame_evaluator()
        self.assertFalse(self._cinderx.is_frame_evaluator_installed())


if __name__ == "__main__":
    unittest.main()
