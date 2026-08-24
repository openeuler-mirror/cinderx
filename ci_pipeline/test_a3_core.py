from ci_pipeline.jit311.a3_census import BOOLEAN_PATHS, REQUIRED_PATHS
from ci_pipeline.jit311.a3_core import (
    apply_core_policy,
    final_status,
    judge_penetration_path,
    judge_semantic_results,
    match_sensitive,
)


def _snapshot(**overrides):
    document = {
        "schema": "cp311-jit-a3-lifecycle-v1",
        "generator": {"status": "GENERATOR_NATIVE_GAUGE_NOT_AVAILABLE"},
    }
    for path in REQUIRED_PATHS:
        target = document
        parts = path.split(".")
        for part in parts[:-1]:
            target = target.setdefault(part, {})
        target[parts[-1]] = False if path in BOOLEAN_PATHS else 0
    for path, value in overrides.items():
        target = document
        parts = path.split("__")
        for part in parts[:-1]:
            target = target[part]
        target[parts[-1]] = value
    return document


def _sample(label, snapshot):
    return {
        "label": label,
        "snapshot": snapshot,
        "invariants": {"ok": True, "errors": []},
        "python_liveness": {},
        "errors": [],
    }


def _c1_result(*, live=True, deaths=100, capacity_error=True):
    samples = [_sample("baseline", _snapshot())]
    if live:
        samples.append(
            _sample(
                "live_1",
                _snapshot(
                    jit__installed_functions=1, jit__watched_functions=1
                ),
            )
        )
    samples.append(
        _sample(
            "after_gc_2",
            _snapshot(runtime__function_destroyed_notifications=deaths),
        )
    )
    errors = []
    if capacity_error:
        errors.append(
            "capacity jit.code_runtimes_allocated grows from after_10=11 to "
            "after_100=101"
        )
    return {
        "scenario": "C1",
        "result": "FAIL" if errors else "PASS",
        "cycles": [1, 10, 100],
        "samples": samples,
        "plateau": {"capacity": {"jit.code_runtimes_allocated": {"plateau": False}}},
        "errors": errors,
    }


def test_capacity_errors_become_diagnostics_not_verdicts():
    result = apply_core_policy(_c1_result())
    assert result["result"] == "PASS"
    assert result["errors"] == []
    demoted = result["resource_stability_diagnostic"]["errors_demoted"]
    assert any("code_runtimes_allocated" in entry for entry in demoted)


def test_vacuous_run_fails_even_with_clean_gauges():
    # A scenario whose populations never rose must not pass on empty
    # invariants: the live_1 rise and the cumulative movement are both
    # required (v1.1 §10).
    silent = _c1_result(live=False, deaths=0, capacity_error=False)
    result = apply_core_policy(silent)
    assert result["result"] == "FAIL"
    assert any("live_1" in error for error in result["errors"])

    lazy = _c1_result(deaths=3, capacity_error=False)
    result = apply_core_policy(lazy)
    assert result["result"] == "FAIL"
    assert any("function_destroyed_notifications" in e for e in result["errors"])


def test_real_errors_survive_the_demotion():
    result = _c1_result(capacity_error=False)
    result["errors"] = ["strict live gauges drifted: ['jit.watched_functions']"]
    result["result"] = "FAIL"
    judged = apply_core_policy(result)
    assert judged["result"] == "FAIL"
    assert judged["errors"][0].startswith("strict live gauges drifted")


BROAD_PATTERNS = [
    "cinderx/Jit/**",
    "cinderx/Common/**",
    "cinderx/Interpreter/**",
    "cinderx/module_state.h",
    "ci_pipeline/jit311/a3_census.py",
]


def test_trigger_covers_the_files_the_lifecycle_fixes_touched():
    # The v1.1 draft's narrow list missed exactly these; the shipped broad
    # list must not.
    touched = [
        "cinderx/Jit/gen_data_footer.h",
        "cinderx/Jit/jit_rt.cpp",
        "cinderx/Common/slab_arena.h",
        "cinderx/Jit/context_iface.h",
        "cinderx/Common/code_extra.cpp",
        "cinderx/Interpreter/3.11/observe.c",
    ]
    assert match_sensitive(touched, BROAD_PATTERNS) == touched


def test_trigger_ignores_unrelated_paths():
    changed = ["docs/design/whatever.md", "ci_pipeline/test_a3_core.py"]
    assert match_sensitive(changed, BROAD_PATTERNS) == []


def _classification(**overrides):
    document = {
        "target_modules": 72,
        "errors": [],
        "unknown_refusals": [],
        "totals": {
            "worker_jit_active": 72,
            "machine_entries": 500_000,
            "actual_own_code_machine_entry_modules": 37,
            "ledger_dropped": 0,
            "events_dropped": 0,
        },
        "modules": {
            f"test_mod{i}": {"scheduler_threshold": 50} for i in range(72)
        },
    }
    for key, value in overrides.items():
        if key in document["totals"]:
            document["totals"][key] = value
        else:
            document[key] = value
    return document


def test_path_proof_accepts_the_frozen_baseline_shape():
    assert judge_penetration_path(_classification()) == []


def test_path_proof_rejects_a_jit_silently_absent_run():
    # Review P0-1: 72/72 semantic passes with the JIT off must not pass.
    dead = _classification(
        worker_jit_active=0,
        machine_entries=0,
        actual_own_code_machine_entry_modules=0,
    )
    errors = judge_penetration_path(dead)
    assert any("JIT active in 0/72" in error for error in errors)
    assert any("no machine-code entries" in error for error in errors)
    assert any("own compiled code" in error for error in errors)


def test_path_proof_rejects_threshold_and_evidence_drift():
    drifted = _classification()
    drifted["modules"]["test_mod0"]["scheduler_threshold"] = 1
    drifted["totals"]["events_dropped"] = 3
    drifted["errors"] = ["missing worker summaries: ['test_mod9']"]
    errors = judge_penetration_path(drifted)
    assert any("threshold" in error for error in errors)
    assert any("events dropped" in error for error in errors)
    assert any("missing worker summaries" in error for error in errors)
    assert judge_penetration_path(None) == [
        "penetration classifier produced no output"
    ]


def test_semantic_results_require_the_full_population():
    # Review follow-up on P0-1: 71/72 all-pass must not read as PASS -- a
    # run that lost results is judged incomplete, not judged on the
    # survivors.
    full = {f"m{i}": "pass" for i in range(72)}
    assert judge_semantic_results(full, expected=72) == []
    short = {f"m{i}": "pass" for i in range(71)}
    assert any(
        "71/72" in error for error in judge_semantic_results(short, expected=72)
    )
    assert any(
        "0/72" in error for error in judge_semantic_results({}, expected=72)
    )
    failing = dict(full, m0="fail")
    assert any(
        "non-pass" in error
        for error in judge_semantic_results(failing, expected=72)
    )


_ALL = {"LIFECYCLE", "SHUTDOWN", "MEMSAFE"}
_GREEN = {name: {"result": "PASS"} for name in _ALL}


def test_subset_or_skipped_hook_never_claims_a_full_pass():
    # Review P0-2.
    subset = final_status(
        selected={"LIFECYCLE"},
        cases={"LIFECYCLE": {"result": "PASS"}},
        hook_required=False,
        hook_skipped=False,
        hook_run=None,
        infra=[],
    )
    assert subset == "NOT_FULLY_RUN"
    skipped = final_status(
        selected=set(_ALL),
        cases=dict(_GREEN),
        hook_required=False,
        hook_skipped=True,
        hook_run=None,
        infra=[],
    )
    assert skipped == "NOT_FULLY_RUN"


def test_full_pass_requires_every_case_and_the_hook_when_required():
    full = final_status(
        selected=set(_ALL),
        cases=dict(_GREEN),
        hook_required=True,
        hook_skipped=False,
        hook_run={"result": "PASS"},
        infra=[],
    )
    assert full == "PASS"
    untriggered = final_status(
        selected=set(_ALL),
        cases=dict(_GREEN),
        hook_required=False,
        hook_skipped=False,
        hook_run=None,
        infra=[],
    )
    assert untriggered == "PASS"
    failing_hook = final_status(
        selected=set(_ALL),
        cases=dict(_GREEN),
        hook_required=True,
        hook_skipped=False,
        hook_run={"result": "FAIL"},
        infra=[],
    )
    assert failing_hook == "FAIL"


def _c4_result(rows):
    return {
        "scenario": "C4",
        "result": "PASS",
        "cycles": [1, 10, 100],
        "samples": [
            _sample("baseline", _snapshot()),
            _sample(
                "after_gc_2",
                _snapshot(runtime__function_destroyed_notifications=200),
            ),
        ],
        "plateau": {"capacity": {}},
        "evidence": {"phase_evidence": rows},
        "errors": [],
    }


def test_c4_phase_evidence_is_part_of_non_vacuity():
    # Review P1-1: pause -> partial death -> surviving re-attach must be
    # visible in the recorded populations, not only in death
    # notifications.  Both pause models qualify: the flag-level pause
    # keeps the population installed (this branch's observed shape), a
    # per-function park moves it to parked.
    flag_pause = apply_core_policy(
        _c4_result(
            [
                {
                    "round": 100,
                    "disabled_installed": 4,
                    "disabled_parked": 0,
                    "half_dead_parked": 0,
                    "enabled_installed": 2,
                }
            ]
        )
    )
    assert flag_pause["result"] == "PASS"

    parked_model = apply_core_policy(
        _c4_result(
            [
                {
                    "round": 100,
                    "disabled_installed": 0,
                    "disabled_parked": 4,
                    "half_dead_parked": 2,
                    "enabled_installed": 2,
                }
            ]
        )
    )
    assert parked_model["result"] == "PASS"

    never_populated = apply_core_policy(
        _c4_result(
            [
                {
                    "round": 100,
                    "disabled_installed": 0,
                    "disabled_parked": 0,
                    "half_dead_parked": 0,
                    "enabled_installed": 0,
                }
            ]
        )
    )
    assert never_populated["result"] == "FAIL"

    no_death_effect = apply_core_policy(
        _c4_result(
            [
                {
                    "round": 100,
                    "disabled_installed": 4,
                    "disabled_parked": 0,
                    "half_dead_parked": 0,
                    "enabled_installed": 4,
                }
            ]
        )
    )
    assert no_death_effect["result"] == "FAIL"
    assert any("phase evidence" in e for e in no_death_effect["errors"])

    missing = apply_core_policy(_c4_result([]))
    assert missing["result"] == "FAIL"
