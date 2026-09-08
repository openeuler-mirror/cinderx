# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Execution coverage for CPython 3.11 new-shape LOAD_ATTR caching."""

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
            self.x = 7

    box = Box()
    sentinel = object()
    box.x = sentinel

    def read(obj):
        return obj.x

    assert cinderjit.force_compile(read) is True
    assert read(box) is sentinel
    load_stats = cinderjit.get_attr_cache_stats()["load_attr"]
    before = load_stats["hits"]
    fills_before = load_stats["fills"]
    invalidations_before = load_stats["invalidations"]
    for _ in range(100):
        assert read(box) is sentinel
    after = cinderjit.get_attr_cache_stats()["load_attr"]["hits"]
    assert after == before, (before, after)
    load_stats = cinderjit.get_attr_cache_stats()["load_attr"]
    assert load_stats["fills"] == fills_before, load_stats
    assert load_stats["invalidations"] == invalidations_before, load_stats
    sentinel_refs = sys.getrefcount(sentinel)
    for _ in range(100):
        assert read(box) is sentinel
    assert sys.getrefcount(sentinel) == sentinel_refs

    mapping = vars(box)
    materialized_before = cinderjit.get_attr_cache_stats()["load_attr"]["hits"]
    for _ in range(100):
        assert read(box) is sentinel
    materialized_after = cinderjit.get_attr_cache_stats()["load_attr"]["hits"]
    assert materialized_after == materialized_before, (
        materialized_before, materialized_after)

    mapping[42] = "force-combined-keys"
    combined_before = cinderjit.get_attr_cache_stats()["load_attr"]["hits"]
    for _ in range(100):
        assert read(box) is sentinel
    combined_after = cinderjit.get_attr_cache_stats()["load_attr"]["hits"]
    assert combined_after == combined_before + 100, (
        combined_before, combined_after)

    del mapping["x"]
    try:
        read(box)
    except AttributeError:
        pass
    else:
        raise AssertionError("deleted attribute was returned")

    replacement = object()
    mapping["x"] = replacement
    assert read(box) is replacement
    replacement_before = cinderjit.get_attr_cache_stats()["load_attr"]["hits"]
    for _ in range(100):
        assert read(box) is replacement
    replacement_after = cinderjit.get_attr_cache_stats()["load_attr"]["hits"]
    assert replacement_after == replacement_before + 100, (
        replacement_before, replacement_after)

    class LateBox:
        def __init__(self):
            for index in range(30):
                setattr(self, f"a{index}", index)

    late = LateBox()
    vars(late)["value"] = sentinel

    def read_late(obj):
        return obj.value

    assert cinderjit.force_compile(read_late) is True
    assert read_late(late) is sentinel
    assert read_late(late) is sentinel
    late_before = cinderjit.get_attr_cache_stats()["load_attr"]["hits"]
    for _ in range(100):
        assert read_late(late) is sentinel
    late_after = cinderjit.get_attr_cache_stats()["load_attr"]["hits"]
    assert late_after == late_before, (late_before, late_after)

    def custom_getattribute(self, name):
        if name == "x":
            return 99
        return object.__getattribute__(self, name)

    Box.__getattribute__ = custom_getattribute
    assert read(box) == 99

    class SlotBox:
        __slots__ = ("value",)

    slot = SlotBox()
    slot.value = 21

    def read_slot(obj):
        return obj.value

    assert cinderjit.force_compile(read_slot) is True
    slot_before = cinderjit.get_attr_cache_stats()["load_attr"]["hits"]
    for _ in range(100):
        assert read_slot(slot) == 21
    slot_after = cinderjit.get_attr_cache_stats()["load_attr"]["hits"]
    assert slot_after == slot_before, (slot_before, slot_after)
    del slot.value
    try:
        read_slot(slot)
    except AttributeError:
        pass
    else:
        raise AssertionError("empty member slot was returned")

    class HookBox:
        def __init__(self):
            self.x = 1

        def __getattr__(self, name):
            return "fallback"

    def read_hook(obj):
        return obj.x

    hook_a = HookBox()
    assert cinderjit.force_compile(read_hook) is True
    assert read_hook(hook_a) == 1
    hook_b = HookBox()
    vars(hook_b)[42] = "force-general-keys"
    assert read_hook(hook_b) == 1
    hook_c = HookBox()
    del hook_c.x
    assert read_hook(hook_c) == "fallback"

    class HookLateBox:
        def __init__(self):
            for index in range(30):
                setattr(self, f"a{index}", index)

        def __getattr__(self, name):
            return "late-fallback"

    hook_late = HookLateBox()
    vars(hook_late)["target"] = 1

    def read_hook_late(obj):
        return obj.target

    assert cinderjit.force_compile(read_hook_late) is True
    assert read_hook_late(hook_late) == 1
    assert read_hook_late(HookLateBox()) == "late-fallback"

    meta_state = {"hidden": False}

    class Meta(type):
        def __getattribute__(cls, name):
            if name == "value" and meta_state["hidden"]:
                raise AttributeError(name)
            return type.__getattribute__(cls, name)

        def __getattr__(cls, name):
            return "meta-fallback"

    class MetaBox(metaclass=Meta):
        value = 1

    def read_type(cls):
        return cls.value

    assert cinderjit.force_compile(read_type) is True
    assert read_type(MetaBox) == 1
    meta_state["hidden"] = True
    assert read_type(MetaBox) == "meta-fallback"

    class RaisingMetaDescriptor:
        def __get__(self, obj, owner):
            raise AttributeError("descriptor-miss")

        def __set__(self, obj, value):
            raise AssertionError("not used")

    class GenericHookMeta(type):
        value = RaisingMetaDescriptor()

        def __getattr__(cls, name):
            return "descriptor-fallback"

    class GenericHookBox(metaclass=GenericHookMeta):
        pass

    def read_meta_descriptor(cls):
        return cls.value

    assert cinderjit.force_compile(read_meta_descriptor) is True
    assert read_meta_descriptor(GenericHookBox) == "descriptor-fallback"
    assert read_meta_descriptor(GenericHookBox) == "descriptor-fallback"

    class StaticBox:
        @staticmethod
        def value():
            return "old"

    def read_static(cls):
        return cls.value

    assert cinderjit.force_compile(read_static) is True
    old_static = read_static(StaticBox)
    assert old_static() == "old"
    StaticBox.__dict__["value"].__init__(lambda: "new")
    assert read_static(StaticBox)() == "new"
    """
)


@unittest.skipUnless(
    sys.version_info[:3] == (3, 11, 6),
    "the new-shape load stub targets CPython 3.11.6",
)
class AttrCacheNewShapeLoad311Test(unittest.TestCase):
    def test_materialize_rekey_delete_and_invalidate(self):
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
        self.assertIn("LoadAttrCachedFastPath", proc.stderr)


if __name__ == "__main__":
    unittest.main()
