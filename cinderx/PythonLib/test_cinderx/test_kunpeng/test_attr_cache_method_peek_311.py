# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Execution coverage for CPython 3.11 METHODPEEK/LMMAT caching."""

import os
import subprocess
import sys
import textwrap
import unittest


PROBE = textwrap.dedent(
    """
    import _cinderx
    import cinderx
    import sys

    cinderx.init()
    _cinderx.install_frame_evaluator()
    import cinderjit

    class Box:
        def method(self):
            return "class"

    first = Box()
    second = Box()

    def call(obj):
        return obj.method()

    assert cinderjit.force_compile(call) is True
    vars(first)
    vars(second)
    for _ in range(20):
        assert call(first) == "class"
        assert call(second) == "class"

    method_stats = cinderjit.get_attr_cache_stats()["load_method"]
    before = method_stats["hits"]
    fills_before = method_stats["fills"]
    invalidations_before = method_stats["invalidations"]
    method_refs = sys.getrefcount(Box.__dict__["method"])
    first_refs = sys.getrefcount(first)
    for _ in range(100):
        assert call(first) == "class"
        assert call(second) == "class"
    after = cinderjit.get_attr_cache_stats()["load_method"]["hits"]
    assert after == before, (before, after)
    method_stats = cinderjit.get_attr_cache_stats()["load_method"]
    assert method_stats["fills"] == fills_before, method_stats
    assert method_stats["invalidations"] == invalidations_before, method_stats
    assert sys.getrefcount(Box.__dict__["method"]) == method_refs
    assert sys.getrefcount(first) == first_refs

    vars(first)["method"] = lambda: "instance"
    assert call(first) == "instance"
    assert call(second) == "class"
    del vars(first)["method"]
    assert call(first) == "class"

    # Fill the shared keys so a later attribute lives in a combined unicode
    # dict, then delete it.  The deleted key remains as a nullptr tombstone;
    # LoadMethod's shadow scan must skip it before comparing later keys.
    for index in range(40):
        setattr(first, f"padding_{index}", index)
    first.deleted_before_method = 1
    del first.deleted_before_method
    assert call(first) == "class"

    class ReusedKeys:
        def method(self):
            return "class"

    reused = ReusedKeys()
    reused.__dict__ = {"x": 1}

    def call_reused(obj):
        return obj.method()

    assert cinderjit.force_compile(call_reused) is True
    for _ in range(3):
        assert call_reused(reused) == "class"
    reused.__dict__.clear()
    reused.method = lambda: "shadow"
    assert call_reused(reused) == "shadow"

    class Reentrant:
        def method(self):
            return "old"

    reentrant = Reentrant()
    reentrant.__dict__ = {}

    def call_reentrant(obj):
        return obj.method()

    assert cinderjit.force_compile(call_reentrant) is True
    for _ in range(3):
        assert call_reentrant(reentrant) == "old"

    class MutatingKey:
        def __hash__(self):
            return hash("method")

        def __eq__(self, other):
            reentrant.__dict__ = {}
            Reentrant.method = lambda self: "new"
            return False

    reentrant.__dict__[MutatingKey()] = 1
    assert call_reentrant(reentrant) == "old"
    assert call_reentrant(reentrant) == "new"

    Box.method = lambda self: "mutated"
    assert call(first) == "mutated"
    assert call(second) == "mutated"

    class Dictless:
        __slots__ = ()
        def method(self):
            return 42

    plain = Dictless()
    def call_plain(obj):
        return obj.method()
    assert cinderjit.force_compile(call_plain) is True
    assert call_plain(plain) == 42
    plain_stats = cinderjit.get_attr_cache_stats()["load_method"]
    plain_before = plain_stats["hits"]
    plain_fills_before = plain_stats["fills"]
    plain_invalidations_before = plain_stats["invalidations"]
    for _ in range(100):
        assert call_plain(plain) == 42
    plain_after = cinderjit.get_attr_cache_stats()["load_method"]["hits"]
    assert plain_after == plain_before, (plain_before, plain_after)
    plain_stats = cinderjit.get_attr_cache_stats()["load_method"]
    assert plain_stats["fills"] == plain_fills_before, plain_stats
    assert plain_stats["invalidations"] == plain_invalidations_before, plain_stats
    """
)


@unittest.skipUnless(
    sys.version_info[:3] == (3, 11, 6),
    "METHODPEEK/LMMAT targets CPython 3.11.6",
)
class AttrCacheMethodPeek311Test(unittest.TestCase):
    def test_materialized_shadow_and_sticky_anchor(self):
        env = {
            key: value
            for key, value in os.environ.items()
            if key != "PYTHONPATH"
            and not key.startswith(("PYTHONJIT", "CINDERX_", "PARALLEL_GC_"))
        }
        env.update(
            CINDERX_JIT_MODE="execute",
            PYTHONJITAUTO="1000000",
            PYTHONJITDUMPLIR="1",
            PYTHONJITLIGHTWEIGHTFRAME="0",
        )
        proc = subprocess.run(
            [sys.executable, "-c", PROBE],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        self.assertIn("LoadMethodCachedFastPath", proc.stderr)


if __name__ == "__main__":
    unittest.main()
