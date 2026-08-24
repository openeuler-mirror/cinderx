from ci_pipeline.jit311.lifecycle_snapshot import BOOLEAN_PATHS, REQUIRED_PATHS, judge_plateau
from ci_pipeline.jit311.lifecycle_discovery_report import classify_blockers, judge


def _snapshot(**overrides):
    document = {
        "schema": "cp311-jit-a3-lifecycle-v1",
        "jit": {},
        "module": {},
        "runtime": {},
        "observer": {},
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


def test_missing_c_result_is_infrastructure_failure():
    status, _ = judge(
        prerequisite={"result": "PASS"},
        c_results={},
        ownership=None,
        finalize=None,
        command_failures=[],
    )
    assert status == "INFRA_FAIL"


def test_resident_drift_is_clustered_as_code_buffer_not_function_watch():
    baseline = _snapshot()
    final = _snapshot(runtime__resident_code_buffers=1)
    result = {
        "result": "FAIL",
        "errors": ["strict live gauges drifted: ['runtime.resident_code_buffers']"],
        "plateau": {
            "gauge_drift": {"runtime.resident_code_buffers": {"baseline": 0, "final": 1}}
        },
        "samples": [_sample("baseline", baseline), _sample("after_gc_2", final)],
    }
    blockers = classify_blockers({"C1": result}, None, None)
    assert [blocker["id"] for blocker in blockers] == ["RESOURCE_RETENTION"]


def test_function_liveness_signal_stays_in_function_watch_cluster():
    result = {
        "result": "FAIL",
        "errors": ["final Python liveness weakrefs_alive is non-zero"],
        "plateau": {"gauge_drift": {}},
        "samples": [_sample("baseline", _snapshot()), _sample("after_gc_2", _snapshot())],
    }
    blockers = classify_blockers({"C1": result}, None, None)
    assert [blocker["id"] for blocker in blockers] == ["FUNCTION_WATCH_OWNERSHIP"]


def test_refcount_failure_is_not_an_approved_deviation():
    blockers = classify_blockers({}, {"result": "FAIL", "errors": ["drift"]}, None)
    assert blockers[0]["id"] == "NATIVE_MEMORY_SAFETY"


def test_observer_keyed_slots_allow_bounded_tombstone_jitter_only():
    baseline = _sample("baseline", _snapshot())
    final = _sample("after_gc_2", _snapshot())
    left = _sample("after_100", _snapshot(observer__keyed_slots=100))
    bounded = _sample("after_1000", _snapshot(observer__keyed_slots=108))
    assert judge_plateau([baseline, left, bounded, final])["result"] == "PASS"
    linear = _sample("after_1000", _snapshot(observer__keyed_slots=109))
    result = judge_plateau([baseline, left, linear, final])
    assert result["result"] == "FAIL"
    assert "observer.keyed_slots" in result["errors"][0]


def test_multithread_shutdown_crash_is_both_lifetime_and_finalize_blocker():
    finalize = {
        "result": "FAIL",
        "failures": [
            {
                "state": "multithread-completed",
                "returncode": -11,
                "errors": ["exit code -11"],
            }
        ],
    }
    blockers = classify_blockers({}, None, finalize)
    assert [blocker["id"] for blocker in blockers] == ["MULTITHREAD_COMPILE_LIFETIME", "MULTITHREAD_SHUTDOWN"]
    assert all(blocker["errors"] == ["exit code -11"] for blocker in blockers)


def test_shutdown_blocker_evidence_is_the_ledger_not_the_repetition_list():
    crash = {
        "state": "multithread-completed",
        "returncode": -11,
        "errors": ["exit code -11"],
    }
    finalize = {
        "result": "FAIL",
        "failures": [dict(crash, iteration=index) for index in range(1, 2001)],
        "per_state": {
            "multithread-completed": {
                "attempts": 2000,
                "successes": 0,
                "sigsegv": 2000,
                "cores": ["cores/multithread-completed-0001/core"],
                "native_backtrace": "#2 jit::CodeRuntime::releaseReferences()",
            }
        },
    }
    blockers = classify_blockers({}, None, finalize)
    for blocker in blockers:
        evidence = blocker["evidence"]["shutdown"]
        assert len(evidence["failures"]) == 5
        ledger = evidence["per_state"]["multithread-completed"]
        assert ledger["sigsegv"] == 2000
        assert "releaseReferences" in ledger["native_backtrace"]
