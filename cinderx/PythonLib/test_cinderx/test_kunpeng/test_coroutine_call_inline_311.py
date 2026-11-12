# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Native coroutine creation across warm CALL caches and observation hooks."""

import gc
import inspect
import os
import subprocess
import sys
import textwrap
import unittest
import warnings
import weakref


def finish(coro):
    try:
        coro.send(None)
    except StopIteration as exc:
        return exc.value
    finally:
        coro.close()
    raise AssertionError("coroutine unexpectedly suspended")


class Pause:
    def __await__(self):
        yield "pause"
        return 7


async def suspending(value, events):
    try:
        return value + await Pause()
    finally:
        events.append("finally")


async def nested_suspending(value, events):
    return await suspending(value, events)


@unittest.skipUnless(sys.version_info[:2] == (3, 11), "CPython 3.11 protocol")
class CoroutineCallInlineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.environ.get("CINDERX_REQUIRE_CORO_INLINE") == "1":
            import _cinderx
            import cinderjit
            import cinderx

            assert cinderx.is_initialized()
            assert _cinderx.is_frame_evaluator_installed()
            assert cinderjit.is_enabled()
            assert cinderjit.jit_frame_mode() == 0

    def warm(self, caller, expected):
        for _ in range(80):
            self.assertEqual(finish(caller()), expected)

    def test_exact_and_positional_only(self):
        async def leaf(a, /, b):
            return a + b

        async def caller():
            return await leaf(2, 3)

        self.warm(caller, 5)

    def test_bound_method(self):
        class Receiver:
            async def leaf(self, value):
                return value + 1

        receiver = Receiver()

        async def caller():
            return await receiver.leaf(4)

        self.warm(caller, 5)

    def test_defaults_rebinding_and_keywords(self):
        async def leaf(a, b=3):
            return a + b

        async def caller():
            return await leaf(2)

        self.warm(caller, 5)
        leaf.__defaults__ = (8,)
        self.warm(caller, 10)

        async def keyword_caller():
            return await leaf(b=6, a=2)

        self.warm(keyword_caller, 8)

    def test_keyword_only_and_variadic(self):
        async def leaf(a, *args, b=3, **kw):
            return a, args, b, kw

        async def caller():
            return await leaf(2, 4, 5, b=6, x=7)

        self.warm(caller, (2, (4, 5), 6, {"x": 7}))

    def test_target_and_code_rebinding(self):
        async def first(x):
            return x + 1

        async def second(x):
            return x + 10

        target = first

        async def caller():
            return await target(2)

        self.warm(caller, 3)
        target = second
        self.warm(caller, 12)
        second.__code__ = first.__code__
        self.warm(caller, 3)

    def test_wrong_argument_count(self):
        async def leaf(a, b):
            return a + b

        async def caller():
            return await leaf(2)

        for _ in range(80):
            with self.assertRaises(TypeError):
                finish(caller())

    def test_created_frame_and_argument_lifetime(self):
        class Token:
            pass

        async def leaf(value):
            return None

        for _ in range(80):
            value = Token()
            ref = weakref.ref(value)
            coro = leaf(value)
            self.assertEqual(inspect.getcoroutinestate(coro), "CORO_CREATED")
            self.assertIs(coro.cr_code, leaf.__code__)
            self.assertIs(coro.cr_frame.f_locals["value"], value)
            del value
            self.assertIsNotNone(ref())
            coro.close()
            self.assertIsNone(coro.cr_frame)
            self.assertIsNone(ref())

    def test_close_and_throw(self):
        for _ in range(40):
            events = []
            coro = nested_suspending(3, events)
            self.assertEqual(coro.send(None), "pause")
            coro.close()
            self.assertEqual(events, ["finally"])
            events = []
            coro = nested_suspending(3, events)
            self.assertEqual(coro.send(None), "pause")
            with self.assertRaisesRegex(ValueError, "boom"):
                coro.throw(ValueError("boom"))
            self.assertEqual(events, ["finally"])

    def test_async_generator_fallback(self):
        async def gen(value):
            yield value

        for _ in range(40):
            obj = gen(7)
            self.assertEqual(finish(obj.__anext__()), 7)
            self.assertIsNone(finish(obj.aclose()))

    def test_origin_tracking(self):
        async def leaf(value):
            return value

        old = sys.get_coroutine_origin_tracking_depth()
        try:
            sys.set_coroutine_origin_tracking_depth(3)
            for _ in range(40):
                coro = leaf(3)
                self.assertTrue(coro.cr_origin)
                self.assertEqual(coro.cr_origin[0][2], "test_origin_tracking")
                self.assertEqual(finish(coro), 3)
        finally:
            sys.set_coroutine_origin_tracking_depth(old)

    def test_unawaited_warning(self):
        async def leaf(value):
            return value

        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always", RuntimeWarning)
            coro = leaf(3)
            del coro
            gc.collect()
        self.assertTrue(any("was never awaited" in str(w.message) for w in caught))

    def test_trace_and_profile_after_warmup(self):
        async def leaf(value):
            return value + 1

        async def caller():
            return await leaf(3)

        self.warm(caller, 4)
        events = []

        def hook(frame, event, arg):
            if frame.f_code == leaf.__code__ and event in ("call", "return"):
                events.append(event)
            return hook

        try:
            sys.settrace(hook)
            self.assertEqual(finish(caller()), 4)
        finally:
            sys.settrace(None)
        self.assertEqual(events, ["call", "return"])
        events.clear()
        try:
            sys.setprofile(hook)
            self.assertEqual(finish(caller()), 4)
        finally:
            sys.setprofile(None)
        self.assertEqual(events, ["call", "return"])

    def test_foreign_evaluator_after_warmup(self):
        code = textwrap.dedent("""
            import _testinternalcapi
            async def leaf(x): return x + 1
            async def caller(): return await leaf(3)
            def run():
                c = caller()
                try: c.send(None)
                except StopIteration as e: assert e.value == 4
            for _ in range(80): run()
            record = []
            _testinternalcapi.set_eval_frame_record(record)
            try:
                for _ in range(20): run()
            finally:
                _testinternalcapi.set_eval_frame_default()
            assert record.count('leaf') >= 20, record
            assert record.count('caller') >= 20, record
        """)
        proc = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)


if __name__ == "__main__":
    unittest.main()
