"""Fresh-process semantic cases for IC observation during AutoJIT warmup."""

import argparse
import dis
import json
import os
from pathlib import Path

import cinderx

cinderx.init()

import cinderjit
import cinderx.jit as jit


def target_event(target):
    path = Path(os.environ["CINDERX_AUTOJIT_COMPILE_EVENTS_FILE"])
    events = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
    matches = [
        event for event in events
        if event["qualname"] == target.__qualname__
        and event["filename"] == target.__code__.co_filename
    ]
    assert len(matches) == 1, matches
    assert jit.is_jit_compiled(target), target.__qualname__
    return matches[0]


def warm(target, arguments, expected, *, minimum_calls=64):
    # The specification allows at most 128 extra interpreted calls beyond the
    # original admission point. Every call uses natural AutoJIT admission.
    # A few additional calls make the installed path, not merely admission,
    # observable. Calls go through the real function, without force_compile.
    saw_compiled = False
    for index in range(minimum_calls + 148):
        value = target(*arguments)
        assert value == expected, (index, value, expected)
        compiled = jit.is_jit_compiled(target)
        if index < minimum_calls:
            assert not compiled, (index, "compiled before original threshold")
        saw_compiled |= compiled
    assert saw_compiled
    return target_event(target)


def mature():
    def target(value):
        return (value + 1) * 3

    event = warm(target, (7,), 24)
    for value in (-17, 0, 2**80, 2.5):
        assert target(value) == (value + 1) * 3
    try:
        target(None)
    except TypeError:
        pass
    else:
        raise AssertionError("TypeError was swallowed")
    return {"event": event}


def inplace_numeric():
    def target(value, operand):
        value += operand
        value -= operand
        value *= operand
        return value

    event = warm(target, (7.0, 2.0), 14.0)
    for value, operand in ((7, 2), (2**80, 3), (2.5, 1.5)):
        assert target(value, operand) == ((value + operand) - operand) * operand

    class Mutable:
        def __init__(self, fail=False):
            self.operations = []
            self.fail = fail

        def __iadd__(self, operand):
            self.operations.append(("add", operand))
            return self

        def __isub__(self, operand):
            self.operations.append(("subtract", operand))
            if self.fail:
                raise LookupError("in-place subtraction failed")
            return self

        def __imul__(self, operand):
            self.operations.append(("multiply", operand))
            return self

    mutable = Mutable()
    assert target(mutable, 2) is mutable
    assert mutable.operations == [("add", 2), ("subtract", 2), ("multiply", 2)]
    failing = Mutable(fail=True)
    try:
        target(failing, 3)
    except LookupError as error:
        assert str(error) == "in-place subtraction failed"
    else:
        raise AssertionError("in-place exception was swallowed")
    assert failing.operations == [("add", 3), ("subtract", 3)]
    return {
        "event": event,
        "operations": mutable.operations,
        "failed_operations": failing.operations,
    }


def slot_feedback_target(slot_count):
    # Independent slot types let the caller invalidate one real cache at a
    # time. The generated target has no loop or helper call, and its float
    # anchor prevents a no_known_sites bypass while slot versions change.
    classes = [
        type(f"Receiver{index}", (), {"__slots__": ("value",)})
        for index in range(slot_count)
    ]
    receivers = [cls() for cls in classes]
    for index, receiver in enumerate(receivers):
        receiver.value = index + 7
    parameters = ", ".join(f"r{index}" for index in range(slot_count))
    values = ", ".join(f"r{index}.value" for index in range(slot_count))
    namespace = {"__name__": __name__}
    source = (
        f"def slot_target({parameters}, value):\n"
        f"    return ({values}, value + 1.0)\n"
    )
    exec(compile(source, __file__, "exec"), namespace)
    target = namespace["slot_target"]
    assert len(target.__code__.co_code) // 2 <= 1024
    return target, classes, receivers


def assert_warm_slot_feedback(target, slot_count):
    opnames = [item.opname for item in dis.get_instructions(target, adaptive=True)]
    assert opnames.count("LOAD_ATTR_SLOT") == slot_count, opnames
    assert opnames.count("BINARY_OP_ADD_FLOAT") == 1, opnames


def natural_late_stable():
    target, classes, receivers = slot_feedback_target(1)
    arguments = (*receivers, 1.0)
    for calls in range(64):
        assert target(*arguments) == (7, 2.0)
        assert not jit.is_jit_compiled(target), calls
    assert_warm_slot_feedback(target, 1)
    # Invalidate the previously consumed slot version immediately before the
    # original admission point. This is an ordinary Python type mutation.
    classes[0].unrelated = 1
    for calls in range(64, 73):
        assert target(*arguments) == (7, 2.0)
        assert jit.is_jit_compiled(target) == (calls == 72), calls
    event = target_event(target)
    receivers[0].value = 23
    assert target(*arguments) == (23, 2.0)

    class Other:
        value = 31

    assert target(Other(), 2.0) == (31, 3.0)
    try:
        target(object(), 2.0)
    except AttributeError:
        pass
    else:
        raise AssertionError("post-admission AttributeError was swallowed")
    return {"event": event, "first_compiled_invocation": 73}


def natural_wait_cap():
    # 31 slot sites plus the float anchor stay within the 32-ready-site limit.
    target, classes, receivers = slot_feedback_target(31)
    arguments = (*receivers, 1.0)
    expected = (*range(7, 38), 2.0)
    for calls in range(193):
        if calls == 64:
            assert_warm_slot_feedback(target, 31)
        if 64 <= calls <= 184 and (calls - 64) % 4 == 0:
            # Change a fresh, still-valid slot version at each observation.
            # Older sites may re-specialize; either transition changes the
            # consumed fingerprint. At 188 at most two samples can agree;
            # the 192 entry must be released by the 128-call cap instead.
            classes[(calls - 64) // 4].unrelated = calls
        assert target(*arguments) == expected, calls
        assert jit.is_jit_compiled(target) == (calls == 192), calls
    event = target_event(target)
    receivers[0].value = 99
    assert target(*arguments) == (99, *expected[1:])
    return {"event": event, "first_compiled_invocation": 193}


def manual():
    # This setter intentionally switches to numeric/non-classifying AutoJIT;
    # the new optional gate must not silently re-enable classification.
    jit.compile_after_n_calls(64)
    assert jit.get_compile_after_n_calls() == 64
    return mature()


def explicit_jit_list():
    jit.append_jit_list(f"{__name__}:explicit_jit_list.<locals>.target")

    def target(value):
        return (value + 1) * 3

    # Explicit-list scheduling installs the CinderX frame evaluator. In auto
    # mode the vectorcall gate and frame entry can both advance the shared
    # internal counter; Python invocation count is not that counter's unit.
    # Preserve the bounded natural-call/value/installed-code checks and verify
    # the exact internal threshold in the native event instead.
    assert not jit.is_jit_compiled(target)
    first_compiled_invocation = None
    invocation_count = 64 + 276
    arguments = (7,)
    for invocation in range(1, invocation_count + 1):
        value = target(*arguments)
        assert value == 24, (invocation, value, 24)
        if jit.is_jit_compiled(target) and first_compiled_invocation is None:
            first_compiled_invocation = invocation
    assert first_compiled_invocation is not None
    event = target_event(target)
    assert event["calls"] == 64, event
    assert event["ic_stability_reason"] == "explicit", event
    assert event["ic_stability_samples"] == 0, event
    return {
        "event": event,
        "first_compiled_invocation": first_compiled_invocation,
        "python_invocations": invocation_count,
        "checked_value": 24,
    }


def low_threshold():
    class Receiver:
        __slots__ = ("value",)

        def __init__(self):
            self.value = 7

    def target(receiver):
        total = 0
        for _ in range(128):
            total += receiver.value
        return total

    return {"event": warm(target, (Receiver(),), 896, minimum_calls=2)}


def explicit_loop():
    jit.append_jit_list(f"{__name__}:explicit_loop.<locals>.target")

    def target(value):
        total = 0
        for _ in range(128):
            total += value
        return total

    assert not jit.is_jit_compiled(target)
    first_compiled_invocation = None
    for invocation in range(1, 33):
        assert target(7) == 896, invocation
        if jit.is_jit_compiled(target) and first_compiled_invocation is None:
            first_compiled_invocation = invocation
    event = target_event(target)
    assert event["calls"] == 2, event
    assert event["ic_stability_reason"] == "explicit", event
    assert event["ic_stability_samples"] == 0, event
    return {"event": event, "first_compiled_invocation": first_compiled_invocation}


def late_warmup_change():
    class A:
        __slots__ = ("value",)

        def __init__(self):
            self.value = 7

    class B:
        __slots__ = ("padding", "value")

        def __init__(self):
            self.padding = -1
            self.value = 11

    def target(receiver):
        total = 0
        for _ in range(128):
            total += receiver.value
        return total

    a, b = A(), B()
    for index in range(64):
        # A's IC is unchanged before the threshold; the final interpreted
        # invocation changes both receiver type and slot location. Repeated
        # accesses let CPython update its IC within this invocation.
        receiver = b if index == 63 else a
        assert target(receiver) == 128 * receiver.value
        assert not jit.is_jit_compiled(target), index
    assert target(b) == 1408
    # Existing lowering guards must preserve the new type/layout while the
    # loop-work policy admits at the original threshold, without extra waiting.
    assert jit.is_jit_compiled(target), "loop work missed the original threshold"
    for _ in range(276):
        assert target(b) == 1408
    return {"event": target_event(target)}


def unknown_then_known():
    class Receiver:
        __slots__ = ("value",)

        def __init__(self):
            self.value = 7

    def target(take_known, receiver):
        if take_known:
            total = 0
            for _ in range(128):
                total += receiver.value
            return total
        return 42

    receiver = Receiver()
    for index in range(64):
        # The near-threshold observation at count 56 sees only unknown sites;
        # the observation at count 60 can see the newly exercised known path.
        take_known = index >= 58
        assert target(take_known, receiver) == (896 if take_known else 42)
        assert not jit.is_jit_compiled(target), index
    assert target(True, receiver) == 896
    assert jit.is_jit_compiled(target), "newly visited loop work was delayed"
    for _ in range(276):
        assert target(True, receiver) == 896
    return {"event": target_event(target)}


def cold_branch():
    def target(take_cold, value, receiver):
        if take_cold:
            return receiver.payload
        return value + 1.0

    event = warm(target, (False, 10.0, None), 11.0)
    reads = []

    class WithProperty:
        @property
        def payload(self):
            reads.append("read")
            return 99

    assert target(True, 0, WithProperty()) == 99
    try:
        target(True, 0, object())
    except AttributeError:
        error = "AttributeError"
    else:
        raise AssertionError("Cold-path AttributeError was swallowed")
    assert reads == ["read"], reads
    assert target(False, 41.0, None) == 42.0
    return {"event": event, "descriptor_reads": len(reads), "cold_exception": error}


def all_unknown():
    def target(take_cold, receiver):
        if take_cold:
            return receiver.payload
        return receiver

    marker = object()
    event = warm(target, (False, marker), marker)
    reads = []

    class WithProperty:
        @property
        def payload(self):
            reads.append("read")
            return 73

    assert target(True, WithProperty()) == 73
    try:
        target(True, object())
    except AttributeError:
        exception = "AttributeError"
    else:
        raise AssertionError("AttributeError was swallowed")
    assert jit.is_jit_compiled(target)
    return {
        "event": event,
        "descriptor_reads": len(reads),
        "cold_exception": exception,
    }


def no_sites():
    def target(value):
        return value

    marker = object()
    event = warm(target, (marker,), marker)
    assert target(marker) is marker
    return {"event": event}


def attribute_phases():
    class Receiver:
        __slots__ = ("value",)

        def __init__(self, value):
            self.value = value

    def target(receiver):
        return receiver.value + 1.0

    receiver = Receiver(7.0)
    event = warm(target, (receiver,), 8)
    results = [target(receiver)]
    receiver.value = 20.0
    results.append(target(receiver))
    reads = []

    def property_value(self):
        reads.append("get")
        if len(reads) == 2:
            raise LookupError("phase change")
        return 30.0

    original_slot = Receiver.value
    Receiver.value = property(property_value)
    results.append(target(receiver))
    try:
        target(receiver)
    except LookupError as error:
        assert str(error) == "phase change"
        exception = type(error).__name__
    else:
        raise AssertionError("Descriptor exception was swallowed")
    Receiver.value = original_slot
    receiver.value = 41.0
    results.append(target(receiver))
    assert reads == ["get", "get"], reads
    return {"event": event, "phase_results": results,
            "property_reads": len(reads), "exception": exception}


def subscript():
    def target(container, index):
        return container[index]

    event = warm(target, ([11], 0), 11)
    values = [target([11], 0), target((23,), 0), target({"k": 37}, "k")]
    errors = []
    for container, index, expected in (([], 0, IndexError), ({}, "k", KeyError), (None, 0, TypeError)):
        try:
            target(container, index)
        except expected as error:
            errors.append(type(error).__name__)
        else:
            raise AssertionError(f"Expected {expected.__name__}")
    return {"event": event, "values": values, "errors": errors}


def instance_value_type_churn():
    class A:
        def __init__(self):
            self.value = 7

    class B:
        def __init__(self):
            self.padding = None
            self.value = 11

    def target(receiver):
        return receiver.value

    a, b = A(), B()
    for index in range(340):
        receiver, expected = (a, 7) if index % 2 == 0 else (b, 11)
        assert target(receiver) == expected
        if index < 64:
            assert not jit.is_jit_compiled(target), index
    return {"event": target_event(target), "values": [target(a), target(b)]}


def method_with_values():
    class Receiver:
        def __init__(self):
            self.padding = None

        def method(self):
            return 41

    def target(receiver):
        return receiver.method()

    return {"event": warm(target, (Receiver(),), 41)}


def instance_value_semantics():
    getattr_reads = []

    class Receiver:
        def __init__(self, value):
            self.value = value

        def __getattr__(self, name):
            assert name == "value"
            getattr_reads.append(name)
            return 31

    def target(receiver):
        return receiver.value

    receiver = Receiver(7)
    event = warm(target, (receiver,), 7)
    jit.clear_runtime_stats()

    values = [target(receiver)]
    receiver.value = 11
    values.append(target(receiver))

    del receiver.value
    values.append(target(receiver))

    receiver.value = 17
    materialized = receiver.__dict__
    assert materialized["value"] == 17
    receiver.value = 19
    values.append(target(receiver))

    class Other:
        value = 23

    class Subclass(Receiver):
        pass

    values.append(target(Other()))
    values.append(target(Subclass(29)))

    property_reads = []

    def property_value(self):
        property_reads.append("property")
        if len(property_reads) == 2:
            raise LookupError("descriptor failure")
        return 37

    Receiver.value = property(property_value)
    values.append(target(receiver))
    try:
        target(receiver)
    except LookupError as error:
        assert str(error) == "descriptor failure"
        exception = type(error).__name__
    else:
        raise AssertionError("descriptor exception was swallowed")

    method_getattr_reads = []

    class Callable:
        def __init__(self, delta):
            self.delta = delta

        def __call__(self, value):
            return value + self.delta

    class MethodReceiver:
        def __init__(self):
            self.callback = Callable(2)

        def __getattr__(self, name):
            assert name == "callback"
            method_getattr_reads.append(name)
            return Callable(9)

    def invoke(receiver, value):
        return receiver.callback(value)

    method_receiver = MethodReceiver()
    method_event = warm(invoke, (method_receiver, 40), 42)
    method_values = [invoke(method_receiver, 1)]
    method_receiver.callback = Callable(5)
    method_values.append(invoke(method_receiver, 1))
    del method_receiver.callback
    method_values.append(invoke(method_receiver, 1))

    stats = jit.get_and_clear_runtime_stats()
    deopts = stats.get("deopt") or ()
    assert jit.is_jit_compiled(target)
    assert jit.is_jit_compiled(invoke)
    # This deliberately raised descriptor exception takes the existing generic
    # LoadAttrCached exception exit. Require exactly that exit, not zero exits
    # and not an arbitrary allowance for unrelated guard failures.
    assert len(deopts) == 1, deopts
    expected_deopt = deopts[0]
    normal = expected_deopt["normal"]
    integers = expected_deopt["int"]
    assert normal["func_qualname"] == target.__qualname__, expected_deopt
    assert Path(normal["filename"]).resolve() == Path(__file__).resolve(), expected_deopt
    assert normal["reason"] == "UnhandledException", expected_deopt
    assert normal["description"] == "LoadAttrCached", expected_deopt
    assert normal["guilty_type"] == "<none>", expected_deopt
    assert integers["lineno"] == target.__code__.co_firstlineno + 1, expected_deopt
    assert integers["bc_offset"] == 4, expected_deopt
    assert integers["opcode"] == 80, expected_deopt
    assert integers["specialized_opcode"] == 180, expected_deopt
    assert integers["count"] == 1, expected_deopt
    return {
        "event": event,
        "method_event": method_event,
        "values": values,
        "getattr_reads": len(getattr_reads),
        "property_reads": len(property_reads),
        "exception": exception,
        "method_values": method_values,
        "method_getattr_reads": len(method_getattr_reads),
        "deopt_count": len(deopts),
    }


def main():
    cases = {case.__name__: case for case in (
        mature, inplace_numeric, natural_late_stable, natural_wait_cap,
        manual, cold_branch, all_unknown, no_sites, attribute_phases,
        subscript, instance_value_semantics, instance_value_type_churn,
        method_with_values,
        explicit_jit_list, explicit_loop, low_threshold, late_warmup_change,
        unknown_then_known,
    )}
    parser = argparse.ArgumentParser()
    parser.add_argument("case", choices=cases)
    args = parser.parse_args()
    assert jit.is_enabled()
    cinderjit._clear_autojit_gate_stats()
    result = cases[args.case]()
    result.update(case=args.case, correct=True)
    print("IC_STABILITY_RESULT=" + json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
