# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Execution coverage for CPython 3.11 STORE_ATTR cache shapes."""

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
        def __init__(self):
            self.x = 1

    box = Box()

    def store(obj, value):
        obj.x = value

    assert cinderjit.force_compile(store) is True
    store(box, -1)
    store_stats = cinderjit.get_attr_cache_stats()["store_attr"]
    before = store_stats["hits"]
    fills_before = store_stats["fills"]
    invalidations_before = store_stats["invalidations"]
    for value in range(100):
        store(box, value)
    after = cinderjit.get_attr_cache_stats()["store_attr"]["hits"]
    assert after == before, (before, after)
    store_stats = cinderjit.get_attr_cache_stats()["store_attr"]
    assert store_stats["fills"] == fills_before, store_stats
    assert store_stats["invalidations"] == invalidations_before, store_stats
    store(box, box.x)

    mapping = vars(box)
    store(box, 101)
    assert mapping["x"] == 101

    mapping[42] = "force-rekey"
    store(box, 102)
    assert mapping["x"] == 102

    del mapping["x"]
    store(box, 103)
    assert mapping["x"] == 103

    class Descriptor:
        def __set__(self, obj, value):
            obj.seen = value + 1

    Box.x = Descriptor()
    store(box, 200)
    assert box.seen == 201

    class SlotBox:
        __slots__ = ("value",)

    slot = SlotBox()

    def store_slot(obj, value):
        obj.value = value

    assert cinderjit.force_compile(store_slot) is True
    store_slot(slot, -1)
    slot_stats = cinderjit.get_attr_cache_stats()["store_attr"]
    slot_before = slot_stats["hits"]
    slot_fills_before = slot_stats["fills"]
    slot_invalidations_before = slot_stats["invalidations"]
    for value in range(100):
        store_slot(slot, value)
    slot_after = cinderjit.get_attr_cache_stats()["store_attr"]["hits"]
    assert slot_after == slot_before, (slot_before, slot_after)
    slot_stats = cinderjit.get_attr_cache_stats()["store_attr"]
    assert slot_stats["fills"] == slot_fills_before, slot_stats
    assert slot_stats["invalidations"] == slot_invalidations_before, slot_stats
    assert slot.value == 99

    fresh = SlotBox()
    sentinel = object()
    sentinel_refs = sys.getrefcount(sentinel)
    fresh_before = cinderjit.get_attr_cache_stats()["store_attr"]["hits"]
    store_slot(fresh, sentinel)
    fresh_after = cinderjit.get_attr_cache_stats()["store_attr"]["hits"]
    assert fresh_after == fresh_before, (fresh_before, fresh_after)
    assert fresh.value is sentinel
    assert sys.getrefcount(sentinel) == sentinel_refs + 1

    class LateBox:
        def __init__(self):
            for index in range(30):
                setattr(self, f"a{index}", index)

    late = LateBox()
    vars(late)["value"] = 0

    def store_late(obj, value):
        obj.value = value

    assert cinderjit.force_compile(store_late) is True
    for value in range(20):
        store_late(late, value)
    late_before = cinderjit.get_attr_cache_stats()["store_attr"]["hits"]
    for value in range(100):
        store_late(late, value)
    late_after = cinderjit.get_attr_cache_stats()["store_attr"]["hits"]
    assert late_after == late_before + 100, (late_before, late_after)
    assert late.value == 99
    """
)


@unittest.skipUnless(
    sys.version_info[:3] == (3, 11, 6),
    "the new-shape store path targets CPython 3.11.6",
)
class AttrCacheNewShapeStore311Test(unittest.TestCase):
    def test_store_shapes_and_fallbacks(self):
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
        self.assertIn("StoreAttrCachedFastPath", proc.stderr)


if __name__ == "__main__":
    unittest.main()
