"""Inline-cache invalidation matrix: attr / method / global mutation between
calls of a compiled reader. Any stale cached value shows up as a diff vs the
interpreter oracle. All mutations are restored in finally so cases stay
independent.
"""

import builtins

G_VALUE = "g-original"


class Base:
    attr = "class-attr"

    def method(self):
        return "base-method"


class Sibling(Base):
    attr = "sibling-attr"

    def method(self):
        return "sibling-method"


def read_attr(obj):
    return obj.attr


def read_method(obj):
    return obj.method()


def read_global():
    return G_VALUE


def read_len(seq):
    return len(seq)


def _reset_base():
    Base.attr = "class-attr"
    if "attr" in vars(Base) and Base.attr != "class-attr":
        Base.attr = "class-attr"


def case_attr_class_reassign():
    obj = Base()
    before = read_attr(obj)
    try:
        Base.attr = "mutated"
        after = read_attr(obj)
    finally:
        Base.attr = "class-attr"
    return (before, after)


def case_attr_instance_shadow_then_del():
    obj = Base()
    before = read_attr(obj)
    obj.attr = "instance-shadow"
    mid = read_attr(obj)
    del obj.attr
    after = read_attr(obj)
    return (before, mid, after)


def case_attr_class_del_raises():
    class Temp:
        attr = "temp"

    obj = Temp()
    before = read_attr(obj)
    del Temp.attr
    try:
        return (before, read_attr(obj))
    except AttributeError as e:
        return (before, "AttributeError: {}".format(e))


def case_attr_property_added_later():
    class Temp:
        attr = "plain"

    obj = Temp()
    before = read_attr(obj)
    Temp.attr = property(lambda self: "via-property")
    after = read_attr(obj)
    return (before, after)


def case_attr_dunder_class_reassign():
    obj = Base()
    before = read_attr(obj)
    try:
        obj.__class__ = Sibling
        after = read_attr(obj)
        after_m = read_method(obj)
    finally:
        obj.__class__ = Base
    return (before, after, after_m)


def case_attr_mro_bases_change():
    class A:
        attr = "from-A"

    class B:
        attr = "from-B"

    class C(A):
        pass

    obj = C()
    before = read_attr(obj)
    C.__bases__ = (B,)
    after = read_attr(obj)
    return (before, after)


def case_method_monkeypatch():
    obj = Base()
    before = read_method(obj)
    try:
        Base.method = lambda self: "patched-method"
        after = read_method(obj)
    finally:
        del Base.method

        def method(self):
            return "base-method"

        Base.method = method
    return (before, after)


def case_method_instance_override():
    obj = Base()
    before = read_method(obj)
    obj.method = lambda: "instance-method"
    after = read_method(obj)
    return (before, after)


def case_slots_attr_mutation():
    class Slotted:
        __slots__ = ("attr",)

        def __init__(self):
            self.attr = "slot-1"

    obj = Slotted()
    before = read_attr(obj)
    obj.attr = "slot-2"
    after = read_attr(obj)
    return (before, after)


def case_descriptor_nondata_vs_instance():
    class NonData:
        def __get__(self, obj, owner):
            return "non-data"

    class Temp:
        attr = NonData()

    obj = Temp()
    before = read_attr(obj)
    obj.__dict__["attr"] = "instance-wins"
    after = read_attr(obj)
    return (before, after)


def case_global_rebind():
    global G_VALUE
    before = read_global()
    try:
        G_VALUE = "g-mutated"
        after = read_global()
    finally:
        G_VALUE = "g-original"
    return (before, after)


def case_global_del_then_read():
    global G_VALUE
    before = read_global()
    try:
        del G_VALUE
        try:
            after = read_global()
        except NameError as e:
            after = "NameError: {}".format(e)
    finally:
        G_VALUE = "g-original"
    return (before, after)


def case_global_shadows_builtin_then_del():
    g = globals()
    before = read_len((1, 2, 3))
    try:
        g["len"] = lambda seq: "shadowed-len"
        mid = read_len((1, 2, 3))
    finally:
        del g["len"]
    after = read_len((1, 2, 3))
    return (before, mid, after)


def case_builtin_added_then_removed():
    def probe():
        return _diffgate_injected_builtin()  # noqa: F821

    try:
        result_missing = "no-error"
        try:
            probe()
        except NameError as e:
            result_missing = "NameError: {}".format(e)
        builtins._diffgate_injected_builtin = lambda: "from-builtins"
        result_present = probe()
    finally:
        if hasattr(builtins, "_diffgate_injected_builtin"):
            del builtins._diffgate_injected_builtin
    return (result_missing, result_present)


def case_type_attr_on_metaclass_path():
    class Meta(type):
        meta_attr = "from-meta"

    class WithMeta(metaclass=Meta):
        pass

    before = WithMeta.meta_attr
    Meta.meta_attr = "meta-mutated"
    after = WithMeta.meta_attr
    return (before, after)


case_attr_class_reassign.helpers = [read_attr]
case_attr_instance_shadow_then_del.helpers = [read_attr]
case_attr_class_del_raises.helpers = [read_attr]
case_attr_property_added_later.helpers = [read_attr]
case_attr_dunder_class_reassign.helpers = [read_attr, read_method]
case_attr_mro_bases_change.helpers = [read_attr]
case_method_monkeypatch.helpers = [read_method]
case_method_instance_override.helpers = [read_method]
case_slots_attr_mutation.helpers = [read_attr]
case_descriptor_nondata_vs_instance.helpers = [read_attr]
case_global_rebind.helpers = [read_global]
case_global_del_then_read.helpers = [read_global]
case_global_shadows_builtin_then_del.helpers = [read_len]
