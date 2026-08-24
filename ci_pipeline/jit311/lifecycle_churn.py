"""C1-C8 lifecycle churn discovery against the private numeric census."""

from __future__ import annotations

import argparse
import gc
import json
from pathlib import Path
import sys
import traceback
import types
import weakref

from ci_pipeline.jit311.lifecycle_snapshot import checkpoint, judge_plateau, snapshot


CHECKPOINTS = {
    "C1": (1, 10, 100, 1000),
    "C2": (100, 1000, 10000),
    "C3": (1, 10, 100, 1000),
    "C4": (1, 10, 100, 1000),
    "C5": (1, 10, 100, 1000),
    "C6": (1, 10, 100, 1000),
    "C7": (1, 10, 100, 1000),
    "C8": (1, 10, 100),
}

QUICK_CHECKPOINTS = {
    "C1": (1, 4),
    "C2": (4, 16),
    "C3": (1, 4),
    "C4": (1, 4),
    "C5": (1, 4),
    "C6": (1, 4),
    "C7": (1, 4),
    "C8": (1, 4),
}

# The lifecycle acceptance profile (simplified plan v1.1): L1/L2 at 100 cycles, the
# observer smoke at 1000 codes, park/die/re-enable at 100 rounds and a
# 10-round multithread batch.  Full-scale endurance stays on the full
# profile.
ACCEPTANCE_CHECKPOINTS = {
    "C1": (1, 10, 100),
    "C2": (100, 1000),
    "C4": (1, 10, 100),
    "C5": (1, 10, 100),
    "C8": (1, 10),
}

ACTIVE_CHECKPOINTS = CHECKPOINTS


def _initialize():
    import _cinderx
    import cinderjit
    import cinderx

    cinderx.init()
    _cinderx.install_frame_evaluator()
    if not _cinderx.is_frame_evaluator_installed():
        raise RuntimeError("the lifecycle probes require the installed CPython 3.11 frame evaluator")
    if _cinderx._get_observe_stats().get("mode") != "execute":
        raise RuntimeError("lifecycle churn requires execute/canary mode")
    return _cinderx, cinderjit


def _suppress_harness():
    """Keep the probe's own persistent control code out of JIT-ALL census."""
    from cinderx.jit import jit_suppress

    helpers = (
        _suppress_harness,
        _alive_count,
        _count_exact_type,
        _make_plain,
        _collect_sample,
        _settle_census,
        _finalize_samples,
        _compile_and_enter,
        *SCENARIOS.values(),
    )
    for helper in helpers:
        jit_suppress(helper)


def _make_plain(index: int, prefix: str):
    name = f"{prefix}_{index}"
    source = (
        f"def {name}(a, b, one):\n"
        "    total = a - a\n"
        "    i = total\n"
        "    while i < b:\n"
        "        total = total + a\n"
        "        i = i + one\n"
        f"    return total + {index}\n"
    )
    namespace = {"__builtins__": __builtins__, "__name__": "__main__"}
    exec(compile(source, f"<a3-{prefix}-{index}>", "exec"), namespace, namespace)
    function = namespace.pop(name)
    return function, namespace, 15 + index


def _alive_count(refs) -> int:
    alive = 0
    for reference in refs:
        if reference() is not None:
            alive += 1
    return alive


def _count_exact_type(objects, expected_type) -> int:
    count = 0
    for obj in objects:
        if type(obj) is expected_type:
            count += 1
    return count


def _collect_sample(cinderjit, label: str, refs=None, extra=None):
    refs = refs or []
    python = {
        "weakrefs_total": len(refs),
        "weakrefs_alive": _alive_count(refs),
    }
    if extra:
        python.update(extra)
    return checkpoint(cinderjit, label, python=python)


def _settle_census(cinderjit):
    """Run the observer stack before the measured baseline.

    Under C2's JIT-ALL configuration the probe's own long-lived helpers are
    eligible too.  Settling them first keeps harness compilation out of the
    ephemeral-code lifetime measurement.
    """
    warm, namespace, expected = _make_plain(900_000_000, "census_settle")
    _compile_and_enter(cinderjit, warm, (3, 5, 1), expected)
    del warm, namespace, expected
    _alive_count([])
    _count_exact_type([], object)
    unmeasured = []
    _finalize_samples(cinderjit, unmeasured, [])
    _collect_sample(cinderjit, "unmeasured_settle_3")


def _finalize_samples(cinderjit, samples, refs, *, extra=None):
    samples.append(_collect_sample(cinderjit, "after_gc_1", refs, extra))
    samples.append(_collect_sample(cinderjit, "after_gc_2", refs, extra))


def _compile_and_enter(cinderjit, function, args, expected):
    if not cinderjit.force_compile(function):
        raise AssertionError(f"force_compile refused {function.__qualname__}")
    import _cinderx

    before = _cinderx._get_trigger_stats()["machine_code_entries"]
    value = function(*args)
    after = _cinderx._get_trigger_stats()["machine_code_entries"]
    if value != expected or after <= before:
        raise AssertionError(
            f"compiled entry missing or wrong result: {value}, {before}->{after}"
        )


def scenario_c1(_cinderx, cinderjit):
    checkpoints = ACTIVE_CHECKPOINTS["C1"]
    _settle_census(cinderjit)
    samples = [_collect_sample(cinderjit, "baseline")]
    refs = []
    code_refs = []
    for index in range(1, checkpoints[-1] + 1):
        function, namespace, expected = _make_plain(index, "c1")
        refs.append(weakref.ref(function))
        try:
            code_refs.append(weakref.ref(function.__code__))
        except TypeError:
            pass
        _compile_and_enter(cinderjit, function, (3, 5, 1), expected)
        if index == 1:
            # Non-vacuity evidence (v1.1 §10): sample while the population
            # is alive, so the final return-to-baseline provably describes
            # a rise and fall rather than a scenario that never populated.
            samples.append(_collect_sample(cinderjit, "live_1", refs))
        del function, namespace
        if index in checkpoints:
            samples.append(
                _collect_sample(
                    cinderjit,
                    f"after_{index}",
                    refs,
                    {"code_weakrefs_alive": _alive_count(code_refs)},
                )
            )
    _finalize_samples(
        cinderjit,
        samples,
        refs,
        extra={"code_weakrefs_alive": _alive_count(code_refs)},
    )
    return samples, {"function_weakrefs": len(refs), "code_weakrefs": len(code_refs)}


def scenario_c2(_cinderx, cinderjit):
    checkpoints = ACTIVE_CHECKPOINTS["C2"]
    warm, warm_namespace, warm_expected = _make_plain(0, "c2_warm")
    if warm(3, 5, 1) != warm_expected or warm(3, 5, 1) != warm_expected:
        raise AssertionError("C2 harness warmup failed")
    del warm, warm_namespace, warm_expected
    _settle_census(cinderjit)
    samples = [_collect_sample(cinderjit, "baseline")]
    refs = []
    code_refs = []
    identities = set()
    for index in range(1, checkpoints[-1] + 1):
        function, namespace, expected = _make_plain(index, "c2")
        identities.add(function.__code__.co_filename)
        refs.append(weakref.ref(function))
        try:
            code_refs.append(weakref.ref(function.__code__))
        except TypeError:
            pass
        before = _cinderx._get_trigger_stats()["machine_code_entries"]
        if function(3, 5, 1) != expected or function(3, 5, 1) != expected:
            raise AssertionError("C2 semantic failure")
        if _cinderx._get_trigger_stats()["machine_code_entries"] <= before:
            raise AssertionError("C2 code object did not enter machine code")
        if index == 1:
            # Non-vacuity evidence (v1.1 §10): the observer population must
            # be seen alive before the smoke's return-to-baseline means
            # anything.
            samples.append(_collect_sample(cinderjit, "live_1", refs))
        del function, namespace
        if index in checkpoints:
            samples.append(
                _collect_sample(
                    cinderjit,
                    f"after_{index}",
                    refs,
                    {
                        "unique_code_identities": len(identities),
                        "code_weakrefs_alive": _alive_count(code_refs),
                    },
                )
            )
    _finalize_samples(
        cinderjit,
        samples,
        refs,
        extra={
            "unique_code_identities": len(identities),
            "code_weakrefs_alive": _alive_count(code_refs),
        },
    )
    return samples, {"unique_code_identities": len(identities)}


def scenario_c3(_cinderx, cinderjit):
    checkpoints = ACTIVE_CHECKPOINTS["C3"]
    _settle_census(cinderjit)
    samples = [_collect_sample(cinderjit, "baseline")]
    owner, namespace, expected = _make_plain(1, "c3_owner")
    _compile_and_enter(cinderjit, owner, (3, 5, 1), expected)
    code = owner.__code__
    samples.append(_collect_sample(cinderjit, "owner_baseline"))
    refs = []
    for index in range(1, checkpoints[-1] + 1):
        fresh = types.FunctionType(code, namespace, f"c3_fresh_{index}")
        refs.append(weakref.ref(fresh))
        _compile_and_enter(cinderjit, fresh, (3, 5, 1), expected)
        del fresh
        if index in checkpoints:
            samples.append(_collect_sample(cinderjit, f"after_{index}", refs))
    del owner, code, namespace
    _finalize_samples(cinderjit, samples, refs)
    return samples, {"transient_functions": len(refs)}


def scenario_c4(_cinderx, cinderjit):
    checkpoints = ACTIVE_CHECKPOINTS["C4"]
    _settle_census(cinderjit)
    samples = [_collect_sample(cinderjit, "baseline")]
    refs = []
    phase_evidence = []
    for round_index in range(1, checkpoints[-1] + 1):
        subjects = []
        for member in range(4):
            function, namespace, expected = _make_plain(
                round_index * 10 + member, "c4"
            )
            refs.append(weakref.ref(function))
            _compile_and_enter(cinderjit, function, (3, 5, 1), expected)
            subjects.append((function, namespace, expected))
        cinderjit.disable()
        disabled = snapshot(cinderjit)
        dead = subjects[:2]
        subjects = subjects[2:]
        del dead
        gc.collect()
        half_dead = snapshot(cinderjit)
        cinderjit.enable()
        for function, namespace, expected in subjects:
            if function(3, 5, 1) != expected:
                raise AssertionError("C4 survivor semantic failure")
        enabled = snapshot(cinderjit)
        del subjects
        del function, namespace, expected
        if round_index in checkpoints:
            phase_evidence.append(
                {
                    "round": round_index,
                    "disabled_installed": disabled["jit"]["installed_functions"],
                    "disabled_parked": disabled["jit"]["parked_functions"],
                    "half_dead_parked": half_dead["jit"]["parked_functions"],
                    "enabled_installed": enabled["jit"]["installed_functions"],
                }
            )
            samples.append(
                _collect_sample(cinderjit, f"after_{round_index}", refs)
            )
    _finalize_samples(cinderjit, samples, refs)
    return samples, {"phase_evidence": phase_evidence}


def scenario_c5(_cinderx, cinderjit):
    checkpoints = ACTIVE_CHECKPOINTS["C5"]
    _settle_census(cinderjit)
    samples = [_collect_sample(cinderjit, "baseline")]
    victim, namespace_a, expected_a = _make_plain(1, "c5_a")
    donor, namespace_b, expected_b = _make_plain(2, "c5_b")
    code_a = victim.__code__
    code_b = donor.__code__
    _compile_and_enter(cinderjit, victim, (3, 5, 1), expected_a)
    victim.__code__ = code_b
    _compile_and_enter(cinderjit, victim, (3, 5, 1), expected_b)
    victim.__code__ = code_a
    _compile_and_enter(cinderjit, victim, (3, 5, 1), expected_a)
    samples.append(_collect_sample(cinderjit, "owner_baseline"))
    for index in range(1, checkpoints[-1] + 1):
        use_b = bool(index & 1)
        victim.__code__ = code_b if use_b else code_a
        expected = expected_b if use_b else expected_a
        if not cinderjit.is_jit_compiled(victim):
            if not cinderjit.force_compile(victim):
                raise AssertionError("C5 force_compile refused")
        if victim(3, 5, 1) != expected:
            raise AssertionError("C5 code-swap semantic failure")
        if index in checkpoints:
            samples.append(_collect_sample(cinderjit, f"after_{index}"))
    function_ref = weakref.ref(victim)
    del victim, donor, code_a, code_b, namespace_a, namespace_b
    _finalize_samples(cinderjit, samples, [function_ref])
    return samples, {"swaps": checkpoints[-1]}


def scenario_c6(_cinderx, cinderjit):
    checkpoints = ACTIVE_CHECKPOINTS["C6"]
    _settle_census(cinderjit)
    namespace = {"__builtins__": __builtins__, "__name__": "__main__"}
    exec(
        compile(
            "def lifecycle_generator(value):\n"
            "    yield value\n"
            "    yield value + 1\n",
            "<a3-C6-generator>",
            "exec",
        ),
        namespace,
        namespace,
    )
    function = namespace.pop("lifecycle_generator")
    type_probe = function(0)
    generator_type = type(type_probe)
    type_probe.close()
    del type_probe
    gc.collect()
    baseline_generators = _count_exact_type(gc.get_objects(), generator_type)
    samples = [
        _collect_sample(
            cinderjit,
            "baseline",
            extra={"jit_generator_gc_objects": baseline_generators},
        )
    ]
    if not cinderjit.force_compile(function):
        raise AssertionError("C6 generator function did not compile")
    samples.append(_collect_sample(cinderjit, "owner_baseline"))
    refs = []
    modes = ("exhaust", "close", "throw", "suspended-drop")
    for index in range(1, checkpoints[-1] + 1):
        for mode in modes:
            generator = function(index)
            generator_type = type(generator)
            refs.append(weakref.ref(generator))
            if mode == "exhaust":
                list(generator)
            elif mode == "close":
                next(generator)
                generator.close()
            elif mode == "throw":
                next(generator)
                try:
                    generator.throw(ValueError("a3-c6"))
                except ValueError:
                    pass
            else:
                next(generator)
            del generator
        if index in checkpoints:
            gc.collect()
            live_type = _count_exact_type(gc.get_objects(), generator_type)
            samples.append(
                _collect_sample(
                    cinderjit,
                    f"after_{index}",
                    refs,
                    {"jit_generator_gc_objects": live_type},
                )
            )
    del function, namespace
    gc.collect()
    live_type = _count_exact_type(gc.get_objects(), generator_type)
    _finalize_samples(
        cinderjit,
        samples,
        refs,
        extra={"jit_generator_gc_objects": live_type},
    )
    return samples, {
        "generator_modes": list(modes),
        "generator_native_gauge": "GENERATOR_NATIVE_GAUGE_NOT_AVAILABLE",
    }


def scenario_c7(_cinderx, cinderjit):
    checkpoints = ACTIVE_CHECKPOINTS["C7"]
    _settle_census(cinderjit)
    samples = [_collect_sample(cinderjit, "baseline")]
    refs = []
    failures = 0
    refusals = 0
    for index in range(1, checkpoints[-1] + 1):
        function, namespace, expected = _make_plain(index, "c7")
        refs.append(weakref.ref(function))
        step = 1 + ((index - 1) % 7)
        failed = cinderjit._jit311_compile_with_publish_failure(function, step)
        if failed is not True or cinderjit.is_jit_compiled(function):
            raise AssertionError(f"C7 publish fault step {step} did not unwind")
        failures += 1
        stable = snapshot(cinderjit)
        for field in (
            "active_compiles",
            "completed_compiles",
            "deferred_finalizations",
            "deferred_anchor_releases",
        ):
            if stable["jit"][field] != 0:
                raise AssertionError(f"C7 transaction residue: {field}")
        if not cinderjit.force_compile(function):
            raise AssertionError("C7 clean compile after fault failed")
        if function(3, 5, 1) != expected:
            raise AssertionError("C7 success semantic failure")
        async_namespace = {"__builtins__": __builtins__, "__name__": "__main__"}
        exec("async def denied():\n    return 1\n", async_namespace, async_namespace)
        denied = async_namespace.pop("denied")
        try:
            cinderjit.force_compile(denied)
        except RuntimeError as exc:
            if str(exc) != "REFUSE_SHAPE_ASYNC_CODE":
                raise
        else:
            raise AssertionError("C7 async refusal unexpectedly compiled")
        refusals += 1
        del function, namespace, denied, async_namespace
        if index in checkpoints:
            samples.append(_collect_sample(cinderjit, f"after_{index}", refs))
    _finalize_samples(cinderjit, samples, refs)
    return samples, {"injected_failures": failures, "typed_refusals": refusals}


def scenario_c8(_cinderx, cinderjit):
    checkpoints = ACTIVE_CHECKPOINTS["C8"]
    if not cinderjit._jit311_multithreaded_compile_test_enabled():
        raise RuntimeError("C8 requires PYTHONJITMULTITHREADEDCOMPILETEST=1")
    _settle_census(cinderjit)
    samples = [_collect_sample(cinderjit, "baseline")]
    refs = []
    for round_index in range(1, checkpoints[-1] + 1):
        subjects = []
        for member in range(8):
            function, namespace, expected = _make_plain(
                round_index * 10 + member, "c8"
            )
            refs.append(weakref.ref(function))
            if not cinderjit._jit311_register_for_compile(function):
                raise AssertionError("C8 function registration failed")
            subjects.append((function, namespace, expected))
        cinderjit._jit311_multithreaded_compile_test()
        for function, namespace, expected in subjects:
            if not cinderjit.is_jit_compiled(function):
                raise AssertionError("C8 batch compile did not install function")
            if function(3, 5, 1) != expected:
                raise AssertionError("C8 compiled function returned wrong result")
        del subjects
        del function, namespace, expected
        if round_index in checkpoints:
            samples.append(
                _collect_sample(cinderjit, f"after_{round_index}", refs)
            )
    _finalize_samples(cinderjit, samples, refs)
    return samples, {"rounds": checkpoints[-1], "population_per_round": 8}


SCENARIOS = {
    "C1": scenario_c1,
    "C2": scenario_c2,
    "C3": scenario_c3,
    "C4": scenario_c4,
    "C5": scenario_c5,
    "C6": scenario_c6,
    "C7": scenario_c7,
    "C8": scenario_c8,
}


SCALES = {
    "full": CHECKPOINTS,
    "quick": QUICK_CHECKPOINTS,
    "acceptance": ACCEPTANCE_CHECKPOINTS,
}


def run(scenario: str, *, quick: bool = False, scale: str | None = None) -> dict:
    global ACTIVE_CHECKPOINTS
    if scale is None:
        scale = "quick" if quick else "full"
    if scenario not in SCALES[scale]:
        raise ValueError(f"scenario {scenario} has no {scale} scale")
    ACTIVE_CHECKPOINTS = SCALES[scale]
    _cinderx, cinderjit = _initialize()
    _suppress_harness()
    before_entries = _cinderx._get_trigger_stats()["machine_code_entries"]
    samples, evidence = SCENARIOS[scenario](_cinderx, cinderjit)
    after_entries = _cinderx._get_trigger_stats()["machine_code_entries"]
    cycles = ACTIVE_CHECKPOINTS[scenario]
    capacity_pair = (f"after_{cycles[-2]}", f"after_{cycles[-1]}")
    plateau = judge_plateau(samples, capacity_pair=capacity_pair)
    errors = list(plateau["errors"])
    final_liveness = samples[-1].get("python_liveness", {})
    for field in ("weakrefs_alive", "code_weakrefs_alive"):
        if int(final_liveness.get(field, 0)) != 0:
            errors.append(f"final Python liveness {field} is non-zero")
    if scenario == "C6":
        baseline_generators = samples[0]["python_liveness"].get(
            "jit_generator_gc_objects"
        )
        final_generators = final_liveness.get("jit_generator_gc_objects")
        if final_generators != baseline_generators:
            errors.append(
                "generator GC census did not return to baseline: "
                f"{baseline_generators}->{final_generators}"
            )
    machine_entries = after_entries - before_entries
    if machine_entries <= 0:
        errors.append("scenario has no machine-code entry proof")
    final_invariants = dict(cinderjit._jit311_lifecycle_invariants())
    if final_invariants.get("ok") is not True:
        errors.extend(final_invariants.get("errors", []))
    return {
        "scenario": scenario,
        "result": "PASS" if not errors else "FAIL",
        "cycles": list(cycles),
        "scale": scale,
        "machine_code_entries": machine_entries,
        "samples": samples,
        "plateau": plateau,
        "evidence": evidence,
        "final_invariants": final_invariants,
        "errors": errors,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", choices=sorted(SCENARIOS), required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--quick",
        action="store_true",
        help="use a tiny developer-only scale; official discovery never sets this",
    )
    parser.add_argument(
        "--scale",
        choices=sorted(SCALES),
        help="checkpoint scale; 'acceptance' is the formal profile, overriding --quick",
    )
    args = parser.parse_args(argv)
    try:
        result = run(args.scenario, quick=args.quick, scale=args.scale)
    except BaseException as exc:
        result = {
            "scenario": args.scenario,
            "result": "INFRA_FAIL",
            "execution_error": f"{type(exc).__name__}: {exc}",
            "traceback": traceback.format_exc(),
            "errors": [f"{type(exc).__name__}: {exc}"],
        }
    args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(
        json.dumps(
            {
                "scenario": args.scenario,
                "result": result["result"],
                "errors": result.get("errors", []),
            },
            sort_keys=True,
        )
    )
    return 2 if result["result"] == "INFRA_FAIL" else 0


if __name__ == "__main__":
    raise SystemExit(main())
