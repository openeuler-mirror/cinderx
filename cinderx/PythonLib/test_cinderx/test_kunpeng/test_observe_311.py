# Copyright (c) Meta Platforms, Inc. and affiliates.

"""The CPython 3.11 runtime modes: off, observe, shadow and execute.

Hot counting at the frame entry, exactly one scheduling request per hot code
object, the typed refusal at the compile entry point, the execute mode that
installs machine code from the same request, and the startup controls that
select the mode.  Every test runs its scenario in a child interpreter because
the mode is parsed from the environment at install time.
"""

import json
import os
import sys
import tempfile
import unittest

from test.support.script_helper import assert_python_ok

REFUSAL = "CINDERX311_JIT_EXEC_DISABLED"
THRESHOLD = 30

PREAMBLE = """\
import json
import _cinderx
import cinderx

cinderx.init()
_cinderx.install_frame_evaluator()
"""


def run_child(source, **env):
    env.setdefault("CINDERX_JIT_MODE", "observe")
    env.setdefault("PYTHONJITAUTO", str(THRESHOLD))
    source = source.replace("@T@", str(THRESHOLD))
    rc, out, err = assert_python_ok("-c", source, **env)
    return json.loads(out.decode().strip().splitlines()[-1])


@unittest.skipUnless(
    sys.version_info[:3] == (3, 11, 6),
    "the CPython 3.11 evaluator is pinned to 3.11.6",
)
class Observe311Tests(unittest.TestCase):
    def test_threshold_boundary_and_dedup(self) -> None:
        payload = run_child(
            PREAMBLE
            + """\
def hot(a, b):
    return a * b + 1

for i in range(@T@ - 1):
    if hot(i, 1) != i + 1:
        raise SystemExit("wrong result before threshold")
before = _cinderx._get_observe_stats()
value = hot(6, 7)
at = _cinderx._get_observe_stats()
for i in range(5):
    hot(i, 2)
after = _cinderx._get_observe_stats()

def mine(stats):
    return [e for e in stats["events"] if e["qualname"] == "hot"]

print(json.dumps({
    "before": mine(before),
    "at": mine(at),
    "after": mine(after),
    "value": value,
    "enabled": after["enabled"],
    "threshold": after["threshold"],
}))
"""
        )
        self.assertTrue(payload["enabled"])
        self.assertEqual(payload["threshold"], THRESHOLD)
        self.assertEqual(payload["value"], 43)
        # Nothing fires before the threshold.
        self.assertEqual(payload["before"], [])
        # The threshold-crossing entry fires exactly one event, and further
        # calls never fire another.
        self.assertEqual(len(payload["at"]), 1)
        event = payload["at"][0]
        self.assertEqual(event["count"], THRESHOLD)
        self.assertEqual(event["result"], REFUSAL)
        self.assertEqual(payload["after"], payload["at"])

    def test_recursion_emits_a_single_event(self) -> None:
        payload = run_child(
            PREAMBLE
            + """\
import sys
sys.setrecursionlimit(10000)

def rec(n):
    if n <= 0:
        return 0
    return rec(n - 1) + 1

value = rec(@T@ * 3)
stats = _cinderx._get_observe_stats()
events = [e for e in stats["events"] if e["qualname"] == "rec"]
print(json.dumps({"value": value, "events": events}))
"""
        )
        self.assertEqual(payload["value"], THRESHOLD * 3)
        self.assertEqual(len(payload["events"]), 1)
        self.assertEqual(payload["events"][0]["count"], THRESHOLD)
        self.assertEqual(payload["events"][0]["result"], REFUSAL)

    def test_no_machine_code_after_the_event(self) -> None:
        payload = run_child(
            PREAMBLE
            + """\
import cinderx.jit

def hot():
    total = 0
    for i in range(10):
        total += i
    return total

for _ in range(@T@ + 10):
    hot()
stats = _cinderx._get_observe_stats()
print(json.dumps({
    "events": [e for e in stats["events"] if e["qualname"] == "hot"],
    "compiled": bool(cinderx.jit.is_jit_compiled(hot)),
    "value": hot(),
}))
"""
        )
        self.assertEqual(len(payload["events"]), 1)
        self.assertFalse(payload["compiled"])
        self.assertEqual(payload["value"], 45)

    def test_shadow_compiles_without_installing_or_entering_code(self) -> None:
        payload = run_child(
            PREAMBLE
            + """\
def shadow_add(a, b):
    return a + b

for i in range(@T@):
    if shadow_add(i, 2) != i + 2:
        raise SystemExit("wrong shadow result")

events = [
    event for event in _cinderx._get_observe_stats()["events"]
    if event["qualname"] == "shadow_add"
]
print(json.dumps({
    "events": events,
    "trigger": _cinderx._get_trigger_stats(),
    "value": shadow_add(40, 2),
}))
""",
            CINDERX_JIT_MODE="shadow",
        )
        self.assertEqual(payload["value"], 42)
        self.assertEqual(len(payload["events"]), 1)
        self.assertEqual(payload["events"][0]["result"], "compiled")
        self.assertTrue(payload["events"][0].get("filename"))
        trigger = payload["trigger"]
        self.assertGreater(trigger["shadow_compile_success"], 0)
        self.assertGreater(trigger["shadow_codegen_bytes"], 0)
        self.assertEqual(trigger["executable_alloc_calls"], 0)
        self.assertEqual(trigger["compiled_function_creations"], 0)
        self.assertEqual(trigger["machine_code_entries"], 0)

    def test_shadow_warm_path_consumes_specialized_bytecode(self) -> None:
        threshold = 64
        payload = run_child(
            PREAMBLE
            + """\
import dis

def shadow_warm_add(a, b):
    return a + b

for i in range(32):
    if shadow_warm_add(i, 2) != i + 2:
        raise SystemExit("wrong warm-up result")
adaptive = dis.Bytecode(shadow_warm_add, adaptive=True).dis()
for i in range(32):
    if shadow_warm_add(i, 3) != i + 3:
        raise SystemExit("wrong post-warm result")
events = [
    event for event in _cinderx._get_observe_stats()["events"]
    if event["qualname"] == "shadow_warm_add"
]
print(json.dumps({
    "adaptive": adaptive,
    "events": events,
    "trigger": _cinderx._get_trigger_stats(),
}))
""",
            CINDERX_JIT_MODE="shadow",
            PYTHONJITAUTO=str(threshold),
        )
        self.assertIn("BINARY_OP_ADD_INT", payload["adaptive"])
        self.assertEqual(len(payload["events"]), 1)
        self.assertEqual(payload["events"][0]["count"], threshold)
        self.assertEqual(payload["events"][0]["result"], "compiled")
        self.assertGreater(
            payload["trigger"]["shadow_specialized_opcodes_consumed"],
            0,
        )

    def test_shadow_refuses_static_code_with_stable_shape_reason(self) -> None:
        payload = run_child(
            PREAMBLE
            + """\
from cinderx.compiler.consts import CI_CO_STATICALLY_COMPILED

def static_shaped(value):
    return value + 1

static_shaped.__code__ = static_shaped.__code__.replace(
    co_flags=static_shaped.__code__.co_flags | CI_CO_STATICALLY_COMPILED
)
success_before = _cinderx._get_trigger_stats()["shadow_compile_success"]
for i in range(@T@):
    if static_shaped(i) != i + 1:
        raise SystemExit("wrong static-shaped result")

events = [
    event for event in _cinderx._get_observe_stats()["events"]
    if event["qualname"] == "static_shaped"
]
print(json.dumps({
    "events": events,
    "success_delta": (
        _cinderx._get_trigger_stats()["shadow_compile_success"]
        - success_before
    ),
}))
""",
            CINDERX_JIT_MODE="shadow",
        )
        self.assertEqual(len(payload["events"]), 1)
        self.assertEqual(
            payload["events"][0]["result"],
            "REFUSE_SHAPE_STATIC_RUNTIME_CACHE",
        )
        self.assertEqual(payload["success_delta"], 0)

    def test_surrogate_co_names_refuses_without_crashing(self) -> None:
        payload = run_child(
            PREAMBLE
            + """\
def surrogate_named(value):
    return value + 1

surrogate_named.__code__ = surrogate_named.__code__.replace(
    co_names=("\\ud800",)
)
results = []
for i in range(@T@):
    results.append(surrogate_named(i))
events = [
    event for event in _cinderx._get_observe_stats()["events"]
    if event["qualname"] == "surrogate_named"
]
print(json.dumps({
    "results": results[-1],
    "events": events,
    "alive": True,
}))
""",
            CINDERX_JIT_MODE="shadow",
        )
        self.assertTrue(payload["alive"])
        self.assertEqual(payload["results"], THRESHOLD)
        self.assertEqual(len(payload["events"]), 1)
        self.assertEqual(
            payload["events"][0]["result"],
            "REFUSE_SHAPE_INVALID_UTF8_NAME",
        )
        self.assertIn("filename", payload["events"][0])

    def test_off_mode_observes_nothing(self) -> None:
        payload = run_child(
            PREAMBLE
            + """\
def hot(a):
    return a + 1

for i in range(@T@ * 2):
    hot(i)
print(json.dumps(_cinderx._get_observe_stats()))
""",
            CINDERX_JIT_MODE="off",
        )
        self.assertFalse(payload["enabled"])
        self.assertEqual(payload["events"], [])

    def test_unknown_mode_refuses_the_takeover(self) -> None:
        payload = run_child(
            """\
import json
import _cinderx
import cinderx

cinderx.init()
try:
    _cinderx.install_frame_evaluator()
    raise SystemExit("install unexpectedly succeeded")
except RuntimeError as exc:
    message = str(exc)
print(json.dumps({
    "installed": _cinderx.is_frame_evaluator_installed(),
    "message": message,
    "stats": _cinderx._get_observe_stats(),
}))
""",
            CINDERX_JIT_MODE="turbo",
        )
        # The stock entry point stays in place and the reason is explicit.
        self.assertFalse(payload["installed"])
        self.assertIn("not accepted", payload["message"])
        # The accepted spellings are all named, and nothing was configured.
        for spelling in ("off", "observe", "shadow", "execute", "canary"):
            self.assertIn(spelling, payload["message"])
        self.assertFalse(payload["stats"]["enabled"])
        self.assertEqual(payload["stats"]["mode"], "off")

    EXECUTE_PROBE = PREAMBLE + """\
import cinderjit

def hot(a, b):
    total = a - a
    i = total
    while i < b:
        total = total + a
        i = i + 1
    return total

before = _cinderx._get_trigger_stats()["machine_code_entries"]
values = [hot(i, 4) for i in range(@T@ + 5)]
after = _cinderx._get_trigger_stats()["machine_code_entries"]
stats = _cinderx._get_observe_stats()
print(json.dumps({
    "values_ok": values == [i * 4 for i in range(@T@ + 5)],
    "events": [e for e in stats["events"] if e["qualname"] == "hot"],
    "mode": stats["mode"],
    "requested_mode": stats["requested_mode"],
    "enabled": stats["enabled"],
    "threshold": stats["threshold"],
    "compiled": cinderjit.is_jit_compiled(hot),
    "entries": after - before,
    "jit_enabled": cinderjit.is_enabled(),
}))
"""

    def test_execute_mode_installs_from_the_scheduling_request(self) -> None:
        # The product mode: the request that observe refuses and shadow
        # discards compiles, installs and enters machine code, from the same
        # counter and the same one-request-per-code discipline.
        payload = run_child(self.EXECUTE_PROBE, CINDERX_JIT_MODE="execute")
        self.assertTrue(payload["values_ok"])
        self.assertTrue(payload["enabled"])
        self.assertTrue(payload["jit_enabled"])
        self.assertEqual(payload["mode"], "execute")
        self.assertEqual(payload["requested_mode"], "execute")
        self.assertEqual(payload["threshold"], THRESHOLD)
        self.assertEqual(len(payload["events"]), 1)
        self.assertEqual(payload["events"][0]["count"], THRESHOLD)
        self.assertEqual(payload["events"][0]["result"], "installed")
        self.assertTrue(payload["compiled"])
        # The calls after the threshold crossing ran compiled.
        self.assertEqual(payload["entries"], 5)

    def test_post_publication_interpreter_frame_is_counted(self) -> None:
        payload = run_child(
            PREAMBLE
            + """\
import sys

def published(value):
    return value + 1

assert published(1) == 2
before = [
    event for event in _cinderx._get_observe_stats()["events"]
    if event["qualname"] == "published"
][0]

def trace(frame, event, arg):
    return trace

sys.settrace(trace)
try:
    assert published(2) == 3
finally:
    sys.settrace(None)
stats = _cinderx._get_observe_stats()
after = [
    event for event in stats["events"]
    if event["qualname"] == "published"
][0]
print(json.dumps({
    "before": before,
    "after": after,
    "total": stats["post_publication_interpreted_frames"],
}))
""",
            CINDERX_JIT_MODE="execute",
            PYTHONJITAUTO="1",
        )
        self.assertEqual(payload["before"]["result"], "installed")
        self.assertEqual(
            payload["before"]["post_publication_interpreted_frames"], 0
        )
        self.assertEqual(
            payload["after"]["post_publication_interpreted_frames"], 1
        )
        self.assertGreaterEqual(payload["total"], 1)

    def test_canary_is_the_test_spelling_of_execute(self) -> None:
        # Same machinery, same policy, reported under its own name so the
        # earlier gate legs keep their configuration and their evidence.
        payload = run_child(self.EXECUTE_PROBE, CINDERX_JIT_MODE="canary")
        self.assertEqual(payload["mode"], "execute")
        self.assertEqual(payload["requested_mode"], "canary")
        self.assertEqual(payload["events"][0]["result"], "installed")
        self.assertEqual(payload["entries"], 5)

    def test_disable_switch_outranks_the_execute_mode(self) -> None:
        # PYTHONJITDISABLE / CINDERX_JIT_DISABLE are the product's "no
        # machine code" switch: with either set, execute is off -- no
        # counting, no compilation, no cinderjit module.
        for switch in ("PYTHONJITDISABLE", "CINDERX_JIT_DISABLE"):
            with self.subTest(switch=switch):
                payload = run_child(
                    PREAMBLE
                    + """\
def hot(a):
    return a + 1

for i in range(@T@ * 2):
    hot(i)
try:
    import cinderjit
except ImportError:
    cinderjit = None
stats = _cinderx._get_observe_stats()
print(json.dumps({
    "stats": stats,
    "cinderjit": cinderjit is not None,
    "trigger": _cinderx._get_trigger_stats(),
}))
""",
                    CINDERX_JIT_MODE="execute",
                    **{switch: "1"},
                )
                self.assertFalse(payload["stats"]["enabled"])
                self.assertEqual(payload["stats"]["mode"], "off")
                self.assertEqual(payload["stats"]["requested_mode"], "execute")
                self.assertEqual(payload["stats"]["events"], [])
                self.assertFalse(payload["cinderjit"])
                self.assertTrue(all(v == 0 for v in payload["trigger"].values()))

    def test_execute_mode_refuses_the_auto_classifier(self) -> None:
        # The 3.12+ behaviour classifier is not part of this port: the
        # environment spelling is refused by the threshold parser, and the
        # -X option is refused by the mode initialization itself.
        from test.support.script_helper import assert_python_failure

        rc, out, err = assert_python_failure(
            "-X",
            "jit-auto=auto:4",
            "-c",
            "import _cinderx",
            CINDERX_JIT_MODE="execute",
        )
        self.assertIn(b"classification is not supported", err)

    def test_unusable_threshold_refuses_the_takeover(self) -> None:
        payload = run_child(
            """\
import json
import _cinderx
import cinderx

cinderx.init()
try:
    _cinderx.install_frame_evaluator()
    raise SystemExit("install unexpectedly succeeded")
except RuntimeError as exc:
    message = str(exc)
print(json.dumps({
    "installed": _cinderx.is_frame_evaluator_installed(),
    "message": message,
}))
""",
            PYTHONJITAUTO="auto",
        )
        self.assertFalse(payload["installed"])
        self.assertIn("positive integer", payload["message"])

    def test_observe_file_records_the_event(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "observe.log")
            run_child(
                PREAMBLE
                + """\
def hot(a):
    return a - 1

for i in range(@T@ + 3):
    hot(i)
print(json.dumps({"done": True}))
""",
                CINDERX_JIT_OBSERVE_FILE=path,
            )
            with open(path, encoding="utf-8") as recorded:
                lines = [line for line in recorded.read().splitlines() if "hot" in line]
        self.assertEqual(len(lines), 1)
        self.assertIn(str(THRESHOLD), lines[0])
        self.assertIn(REFUSAL, lines[0])

    def test_counters_follow_the_code_lifecycle(self) -> None:
        payload = run_child(
            PREAMBLE
            + """\
import gc

made = {}
for index in range(5):
    namespace = {}
    exec("def burst_" + str(index) + "(x):\\n    return x + " + str(index), namespace)
    fn = namespace["burst_" + str(index)]
    for _ in range(@T@):
        fn(1)
    made[index] = fn
del made, namespace, fn
gc.collect()

def late(x):
    return x * 2

for _ in range(@T@):
    late(3)
stats = _cinderx._get_observe_stats()
names = [e["qualname"] for e in stats["events"]]
print(json.dumps({"names": names}))
"""
        )
        names = payload["names"]
        expected = {f"burst_{i}" for i in range(5)} | {"late"}
        self.assertTrue(expected.issubset(set(names)))
        # One event per code object, including the ones already collected.
        for name in expected:
            self.assertEqual(names.count(name), 1)

    def test_startup_control_installs_on_request(self) -> None:
        probe = (
            "import _cinderx_auto, _cinderx, json; "
            "print(json.dumps(_cinderx.is_frame_evaluator_installed()))"
        )
        rc, out, err = assert_python_ok(
            "-c", probe, CINDERX_PLUGIN_ENABLE="1", CINDERX_EVAL_MODE="cinder"
        )
        self.assertTrue(json.loads(out.decode().strip().splitlines()[-1]))

        rc, out, err = assert_python_ok(
            "-c", probe, CINDERX_PLUGIN_ENABLE="1", CINDERX_EVAL_MODE="stock"
        )
        self.assertFalse(json.loads(out.decode().strip().splitlines()[-1]))

    def test_startup_control_publishes_cinderjit_only_in_execute(self) -> None:
        probe = (
            "import _cinderx_auto, json; "
            "print(json.dumps({"
            "'cinderjit': _cinderx_auto.cinderjit is not None, "
            "'installed': _cinderx_auto._cinderx.is_frame_evaluator_installed(), "
            "'mode': _cinderx_auto._cinderx._get_observe_stats()['mode']}))"
        )
        expectations = {
            "execute": (True, "execute"),
            "canary": (True, "execute"),
            "shadow": (False, "shadow"),
            "observe": (False, "observe"),
            "off": (False, "off"),
        }
        for mode, (has_cinderjit, resolved) in expectations.items():
            with self.subTest(mode=mode):
                rc, out, err = assert_python_ok(
                    "-c",
                    probe,
                    CINDERX_PLUGIN_ENABLE="1",
                    CINDERX_EVAL_MODE="cinder",
                    CINDERX_JIT_MODE=mode,
                )
                payload = json.loads(out.decode().strip().splitlines()[-1])
                self.assertEqual(payload["cinderjit"], has_cinderjit)
                self.assertTrue(payload["installed"])
                self.assertEqual(payload["mode"], resolved)
        # The disable switch wins at startup too.
        rc, out, err = assert_python_ok(
            "-c",
            probe,
            CINDERX_PLUGIN_ENABLE="1",
            CINDERX_EVAL_MODE="cinder",
            CINDERX_JIT_MODE="execute",
            PYTHONJITDISABLE="1",
        )
        payload = json.loads(out.decode().strip().splitlines()[-1])
        self.assertFalse(payload["cinderjit"])
        self.assertEqual(payload["mode"], "off")

    def test_startup_control_refuses_unknown_modes(self) -> None:
        from test.support.script_helper import assert_python_failure

        rc, out, err = assert_python_failure(
            "-c",
            "import _cinderx_auto",
            CINDERX_PLUGIN_ENABLE="1",
            CINDERX_EVAL_MODE="jitter",
        )
        self.assertIn(b"not accepted", err)


if __name__ == "__main__":
    unittest.main()
