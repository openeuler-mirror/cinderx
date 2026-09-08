# Copyright (c) Meta Platforms, Inc. and affiliates.

import os
import subprocess
import sys
import textwrap
import unittest


@unittest.skipUnless(sys.version_info[:2] == (3, 11), "CPython 3.11 dict keys")
class CombinedKeys311Test(unittest.TestCase):
    def test_mutation_resize_replacement_and_reentry(self):
        probe = textwrap.dedent(
            """
            import cinderx
            import _cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            class Box:
                def method(self):
                    return "class"

            obj = Box()
            obj.__dict__ = {"padding": 1}

            def call(obj):
                return obj.method()

            def store(obj, value):
                obj.method = value

            assert cinderjit.force_compile(call) is True
            assert cinderjit.force_compile(store) is True
            assert cinderjit.is_jit_compiled(call)
            for _ in range(4):
                assert call(obj) == "class"
            # The first insertion used to abort on Py_DEBUG: the negative
            # method peek had increfed an exclusively owned combined table.
            obj.extra = 2
            store(obj, lambda: "shadow")
            assert call(obj) == "shadow"
            for value in (None, 42):
                store(obj, value)
                try:
                    call(obj)
                except TypeError:
                    pass
                else:
                    raise AssertionError('non-callable shadow was ignored')
            del obj.method
            assert call(obj) == "class"
            for index in range(100):
                obj.__dict__[str(index)] = index
            assert call(obj) == "class"
            obj.__dict__.clear()
            obj.method = lambda: "after-clear"
            assert call(obj) == "after-clear"

            def call_same_size(obj):
                return obj.method()

            assert cinderjit.force_compile(call_same_size) is True
            obj.__dict__ = {"padding": 1}
            for _ in range(4):
                assert call_same_size(obj) == "class"
            obj.__dict__ = {"method": lambda: "same-size-shadow"}
            assert call_same_size(obj) == "same-size-shadow"

            events = []
            def call_finalizer(obj):
                return obj.method()

            assert cinderjit.force_compile(call_finalizer) is True
            class Finalizer:
                def __del__(self):
                    events.append(call_finalizer(obj))
                    obj.method = lambda: "finalizer-shadow"
                    events.append(call_finalizer(obj))

            obj.__dict__ = {"padding": Finalizer()}
            for _ in range(4):
                assert call_finalizer(obj) == "class"
            obj.padding = 2
            assert events == ["class", "finalizer-shadow"], events
            assert call_finalizer(obj) == "finalizer-shadow"

            # A general-key comparison may replace the receiver's dict.
            # The current call keeps its selected class method; only the
            # next call observes the new dict's shadow.
            replacement = {"method": lambda: "replacement-shadow"}
            class ReplacingKey:
                armed = False
                def __hash__(self):
                    return hash("method")
                def __eq__(self, other):
                    if self.armed:
                        obj.__dict__ = replacement
                    return False

            key = ReplacingKey()
            obj.__dict__ = {key: 1}
            def call_replacing(obj):
                return obj.method()

            assert cinderjit.force_compile(call_replacing) is True
            for _ in range(4):
                assert call_replacing(obj) == "class"
            key.armed = True
            assert call_replacing(obj) == "class"
            assert call_replacing(obj) == "replacement-shadow"
            replacement = {"padding": 1}
            key.armed = False
            obj.__dict__ = {key: 1}
            for _ in range(4):
                assert call_replacing(obj) == "class"
            key.armed = True
            assert call_replacing(obj) == "class"
            assert call_replacing(obj) == "class"

            # A general-key error must reach the handler in the same
            # compiled frame as LOAD_METHOD, not just an outer Python caller.
            class UniqueError(Exception):
                pass

            class RaisingKey:
                comparisons = 0
                def __hash__(self):
                    return hash("method")
                def __eq__(self, other):
                    RaisingKey.comparisons += 1
                    raise UniqueError("combined-key-error")

            def call_and_catch(obj):
                try:
                    return obj.method()
                except UniqueError:
                    return "caught"

            def exercise_caught_error(require_cache_hits):
                receiver = Box()
                receiver.__dict__ = {"padding": 1}
                for _ in range(4):
                    assert call_and_catch(receiver) == "class"
                before = cinderjit.get_attr_cache_stats()["load_method"]["hits"]
                for _ in range(8):
                    assert call_and_catch(receiver) == "class"
                after = cinderjit.get_attr_cache_stats()["load_method"]["hits"]
                if require_cache_hits:
                    assert after == before + 8, (before, after)
                raising_key = RaisingKey()
                receiver.__dict__[raising_key] = 1
                RaisingKey.comparisons = 0
                caught = call_and_catch(receiver)
                comparisons = RaisingKey.comparisons
                del receiver.__dict__[raising_key]
                recovered = call_and_catch(receiver)
                assert RaisingKey.comparisons == comparisons
                return caught, comparisons, recovered

            interpreted = exercise_caught_error(False)
            assert cinderjit.force_compile(call_and_catch) is True
            assert cinderjit.is_jit_compiled(call_and_catch)
            compiled = exercise_caught_error(True)
            assert interpreted == compiled == ("caught", 1, "class"), (
                interpreted, compiled
            )

            class Switching:
                def method(self):
                    return "inline-class"

            first = Switching()
            second = Switching()
            def call_switching(obj):
                return obj.method()

            assert cinderjit.force_compile(call_switching) is True
            assert call_switching(first) == "inline-class"
            first.padding = 1
            for _ in range(4):
                assert call_switching(first) == "inline-class"
            first.__dict__ = {"padding": 1}
            for _ in range(4):
                assert call_switching(first) == "inline-class"
            second.method = lambda: "inline-shadow"
            assert call_switching(second) == "inline-shadow"
            del second.method
            assert call_switching(second) == "inline-class"
            assert call_switching(first) == "inline-class"
            first.__dict__ = first.__dict__.copy()
            first.method = lambda: "copy-shadow"
            assert call_switching(first) == "copy-shadow"
            assert call_switching(second) == "inline-class"

            def write_padding(obj, value):
                obj.padding = value

            def call_transition(obj):
                return obj.method()

            def call_transition_second(obj):
                return obj.method()

            for function in (write_padding, call_transition,
                             call_transition_second):
                assert cinderjit.force_compile(function) is True
            target = Box()
            target.__dict__ = {"padding": 0}
            other = Box()
            other.__dict__ = {"padding": 0,
                              "method": lambda: "other-shadow"}
            for i in range(10):
                write_padding(target, i)
                assert call_transition(target) == "class"
                assert call_transition_second(target) == "class"

            # Both compiled call sites retain normal hits across value writes.
            # Native tests separately distinguish transition reuse from a
            # behaviorally equivalent fallback using lazy Unicode hashing.
            total_before = cinderjit.get_attr_cache_stats()["load_method"]["hits"]
            misses_before = cinderjit.get_attr_cache_stats()["load_method"]["misses"]
            for i in range(10, 110):
                write_padding(target, i)
                assert call_transition(target) == "class"
                assert call_transition_second(target) == "class"
            assert (cinderjit.get_attr_cache_stats()["load_method"]["hits"]
                    == total_before + 200)
            assert (cinderjit.get_attr_cache_stats()["load_method"]["misses"]
                    == misses_before)
            total_before = cinderjit.get_attr_cache_stats()["load_method"]["hits"]
            assert call_transition(target) == "class"
            assert call_transition_second(target) == "class"
            assert (cinderjit.get_attr_cache_stats()["load_method"]["hits"]
                    == total_before + 2)

            write_padding(target, 20)
            assert call_transition(other) == "other-shadow"
            assert call_transition(target) == "class"
            assert call_transition_second(target) == "class"
            write_padding(target, 40)
            write_padding(target, 41)
            assert call_transition(target) == "class"
            # A structural change between two value writes is not a chain of
            # absence-preserving transitions.
            write_padding(target, 21)
            target.method = lambda: "between-writes"
            write_padding(target, 22)
            assert call_transition(target) == "between-writes"
            del target.method

            for mode in ("add", "clear", "replace", "other-write"):
                events = []
                class TransitionFinalizer:
                    def __del__(self):
                        if mode == "other-write":
                            write_padding(other, 30)
                            events.append(call_transition(target))
                            return
                        events.append(call_transition(target))
                        if mode == "clear":
                            target.__dict__.clear()
                        if mode == "replace":
                            target.__dict__ = {"method": lambda: mode}
                        else:
                            target.method = lambda: mode
                        events.append(call_transition(target))

                target.__dict__ = {"padding": TransitionFinalizer()}
                for _ in range(4):
                    assert call_transition(target) == "class"
                write_padding(target, 0)
                expected = ["class"] if mode == "other-write" else ["class", mode]
                assert events == expected, (mode, events)
                assert call_transition(target) == expected[-1]
            print("combined keys ownership passed")
            """
        )
        env = {
            key: value
            for key, value in os.environ.items()
            if key != "PYTHONPATH"
            and not key.startswith(("PYTHONJIT", "CINDERX_", "PARALLEL_GC_"))
        }
        env.update(
            CINDERX_JIT_MODE="execute",
            PYTHONJITAUTO="1000000",
            PYTHONJITLIGHTWEIGHTFRAME="0",
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("combined keys ownership passed", proc.stdout)


if __name__ == "__main__":
    unittest.main()
