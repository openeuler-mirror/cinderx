# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Isolated method descriptor scenarios for the Kunpeng regression tests."""

import sys

import cinderx


def call_hook(obj):
    return obj.hook()


def descriptor_class(cls, data_descriptor):
    if not data_descriptor:
        return cls

    class DataDescriptor(cls):
        def __set__(self, obj, value):
            pass

    return DataDescriptor


def unset_slot_falls_back_to_getattr(data_descriptor=False):
    class Receiver:
        __slots__ = ("hook",)

        def __getattr__(self, name):
            assert name == "hook"
            self.hook = lambda: "fallback"
            return self.hook

    warm = Receiver()
    warm.hook = lambda: "warm"
    assert call_hook(warm) == "warm"
    fresh = Receiver()
    assert call_hook(fresh) == "fallback"
    assert call_hook(fresh) == "fallback"


def descriptor_attribute_error_uses_getattr(data_descriptor=False):
    class Descriptor:

        def __get__(self, obj, owner):
            obj.events.append("get")
            raise AttributeError("descriptor")

    class Receiver:
        hook = descriptor_class(Descriptor, data_descriptor)()

        def __init__(self):
            self.events = []

        def __getattr__(self, name):
            self.events.append(("fallback", name))
            return lambda: "fallback"

    obj = Receiver()
    for _ in range(3):
        assert call_hook(obj) == "fallback"
    assert obj.events == ["get", ("fallback", "hook")] * 3


def other_descriptor_exception_is_preserved(data_descriptor=False):
    error = ValueError("descriptor")

    class Descriptor:

        def __get__(self, obj, owner):
            raise error

    class Receiver:
        hook = descriptor_class(Descriptor, data_descriptor)()

        def __getattr__(self, name):
            raise AssertionError("unexpected fallback")

    for _ in range(3):
        try:
            call_hook(Receiver())
        except ValueError as exc:
            assert exc is error
        else:
            raise AssertionError("missing exception")


def descriptor_without_getattr_keeps_attribute_error(data_descriptor=False):
    error = AttributeError("descriptor")

    class Descriptor:

        def __get__(self, obj, owner):
            raise error

    class Receiver:
        hook = Descriptor()

    try:
        call_hook(Receiver())
    except AttributeError as exc:
        assert exc is error
    else:
        raise AssertionError("missing exception")


def successful_descriptor_does_not_call_getattr(data_descriptor=False):
    class Descriptor:

        def __get__(self, obj, owner):
            return lambda: "descriptor"

    class Receiver:
        hook = descriptor_class(Descriptor, data_descriptor)()

        def __getattr__(self, name):
            raise AssertionError("unexpected fallback")

    for _ in range(3):
        assert call_hook(Receiver()) == "descriptor"


def instance_attribute_shadows_non_data_descriptor(data_descriptor=False):
    class Descriptor:

        def __get__(self, obj, owner):
            raise AssertionError("shadowed")

    class Receiver:
        hook = Descriptor()

        def __getattr__(self, name):
            raise AssertionError("unexpected fallback")

    obj = Receiver()
    obj.hook = lambda: "instance"
    assert call_hook(obj) == "instance"


def getattr_added_by_descriptor_applies_to_next_lookup(data_descriptor=False):
    error = AttributeError("descriptor")

    class Descriptor:

        def __get__(self, obj, owner):
            owner.__getattr__ = lambda self, name: lambda: "new hook"
            raise error

    class Receiver:
        hook = descriptor_class(Descriptor, data_descriptor)()

    obj = Receiver()
    try:
        call_hook(obj)
    except AttributeError as exc:
        assert exc is error
    else:
        raise AssertionError("hook applied to current lookup")
    assert call_hook(obj) == "new hook"


def getattr_exception_is_preserved(data_descriptor=False):
    error = RuntimeError("fallback")

    class Receiver:
        __slots__ = ("hook",)

        def __getattr__(self, name):
            raise error

    try:
        call_hook(Receiver())
    except RuntimeError as exc:
        assert exc is error
    else:
        raise AssertionError("missing exception")


def custom_getattribute_is_respected(data_descriptor=False):
    class Receiver:

        def __getattribute__(self, name):
            if name == "hook":
                return lambda: "custom"
            return object.__getattribute__(self, name)

        def __getattr__(self, name):
            raise AssertionError("unexpected fallback")

    assert call_hook(Receiver()) == "custom"


CASES = {
    "unset_slot_falls_back_to_getattr": unset_slot_falls_back_to_getattr,
    "descriptor_attribute_error_uses_getattr": descriptor_attribute_error_uses_getattr,
    "other_descriptor_exception_is_preserved": other_descriptor_exception_is_preserved,
    "descriptor_without_getattr_keeps_attribute_error": descriptor_without_getattr_keeps_attribute_error,
    "successful_descriptor_does_not_call_getattr": successful_descriptor_does_not_call_getattr,
    "instance_attribute_shadows_non_data_descriptor": instance_attribute_shadows_non_data_descriptor,
    "getattr_added_by_descriptor_applies_to_next_lookup": getattr_added_by_descriptor_applies_to_next_lookup,
    "getattr_exception_is_preserved": getattr_exception_is_preserved,
    "custom_getattribute_is_respected": custom_getattribute_is_respected,
}


if __name__ == "__main__":
    cinderx.init()
    import cinderx.jit as jit

    assert jit.force_compile(call_hook)
    assert jit.is_jit_compiled(call_hook)
    CASES[sys.argv[1]](sys.argv[2] == "data")
