"""Tests for the CPython 3.11 JIT refusal-reason report classifier."""

from ci_pipeline.jit311 import report


def test_report_loads_opcode_and_shape_reasons_from_support_list():
    registry = report.load_refusal_reason_registry()

    assert "REFUSE_UNPORTED" in registry["opcode"]
    assert "REFUSE_SHAPE_GENERATOR_RUNTIME_UNAUDITED" in registry["shape"]
    assert report.KNOWN_REFUSAL_REASONS >= registry["opcode"] | registry["shape"]


def test_report_classifies_each_refusal_namespace():
    assert (
        report.classify_refusal_reason("CINDERX311_JIT_EXEC_DISABLED")
        == "capability"
    )
    assert report.classify_refusal_reason("REFUSE_UNPORTED") == "opcode"
    assert (
        report.classify_refusal_reason("REFUSE_SHAPE_ASYNC_CODE")
        == "shape"
    )
    assert report.classify_refusal_reason("REFUSE_FUTURE_UNKNOWN") == "unknown"
    assert report.classify_refusal_reason(None) == "unknown"


def test_shape_reason_is_not_accepted_from_the_opcode_namespace():
    registry = {
        "opcode": frozenset({"REFUSE_SHAPE_BAD_MUTATION"}),
        "shape": frozenset(),
    }

    assert (
        report.classify_refusal_reason(
            "REFUSE_SHAPE_ASYNC_CODE",
            registry,
        )
        == "unknown"
    )


def test_snapshot_counts_only_unregistered_reasons_as_unknown(monkeypatch):
    monkeypatch.setattr(
        report,
        "_trigger_stats",
        lambda: {
            "compiled_function_creations": 0,
            "machine_code_entries": 0,
            "executable_alloc_calls": 0,
            "executable_alloc_bytes": 0,
            "shadow_compile_success": 0,
            "shadow_specialized_opcodes_consumed": 0,
            "shadow_codegen_bytes": 0,
            "forced_deopt_hits": 0,
            "organic_deopt_hits": 0,
        },
    )
    monkeypatch.setattr(
        report,
        "_observe_stats",
        lambda: {
            "events": [
                {"result": "REFUSE_UNPORTED"},
                {"result": "REFUSE_SHAPE_ASYNC_CODE"},
                {"result": "REFUSE_FUTURE_UNKNOWN"},
                {"result": "SUPPORTED_OPCODE_FAILURE"},
                {"result": "ok"},
            ],
            "events_dropped": 0,
        },
    )
    monkeypatch.setattr(report, "_evaluator_installed", lambda: True)
    monkeypatch.setattr(report, "_compiled_function_count", lambda: 0)
    monkeypatch.setattr(report, "_peak_rss_bytes", lambda: 123456)

    snapshot = report.snapshot()

    assert snapshot["compile_requests"] == 5
    assert snapshot["compile_rejected"] == 4
    assert snapshot["compile_success"] == 0
    assert snapshot["opcode_rejects"] == 1
    assert snapshot["shape_rejects"] == 1
    assert snapshot["supported_opcode_failures"] == 1
    assert snapshot["unknown_rejects"] == 2
    assert snapshot["peak_rss_bytes"] == 123456


def test_snapshot_keeps_dropped_events_out_of_request_classification(monkeypatch):
    monkeypatch.setattr(
        report,
        "_trigger_stats",
        lambda: {
            "compiled_function_creations": 0,
            "machine_code_entries": 0,
            "executable_alloc_calls": 0,
            "executable_alloc_bytes": 0,
            "shadow_compile_success": 2,
            "shadow_specialized_opcodes_consumed": 7,
            "shadow_codegen_bytes": 4096,
            "forced_deopt_hits": 0,
            "organic_deopt_hits": 0,
        },
    )
    monkeypatch.setattr(
        report,
        "_observe_stats",
        lambda: {
            "events": [
                {"result": "compiled"},
                {"result": "CINDERX311_JIT_EXEC_DISABLED"},
            ],
            "events_dropped": 3,
        },
    )
    monkeypatch.setattr(report, "_evaluator_installed", lambda: True)
    monkeypatch.setattr(report, "_compiled_function_count", lambda: 0)
    monkeypatch.setattr(report, "_peak_rss_bytes", lambda: 8192)

    snapshot = report.snapshot()

    assert snapshot["compile_requests"] == 2
    assert snapshot["compile_success"] == 2
    assert snapshot["compile_rejected"] == 1
    assert snapshot["events_dropped"] == 3
    assert snapshot["capability_rejects"] == 1
    assert snapshot["specialized_opcodes_consumed"] == 7
    assert snapshot["shadow_codegen_bytes"] == 4096


def test_none_result_is_an_unknown_reject_not_a_success(monkeypatch):
    monkeypatch.setattr(
        report,
        "_trigger_stats",
        lambda: {
            "compiled_function_creations": 0,
            "machine_code_entries": 0,
            "executable_alloc_calls": 0,
            "executable_alloc_bytes": 0,
            "shadow_compile_success": 0,
            "shadow_specialized_opcodes_consumed": 0,
            "shadow_codegen_bytes": 0,
            "forced_deopt_hits": 0,
            "organic_deopt_hits": 0,
        },
    )
    monkeypatch.setattr(
        report,
        "_observe_stats",
        lambda: {"events": [{"result": None}], "events_dropped": 0},
    )
    monkeypatch.setattr(report, "_evaluator_installed", lambda: True)
    monkeypatch.setattr(report, "_compiled_function_count", lambda: 0)
    monkeypatch.setattr(report, "_peak_rss_bytes", lambda: 1)

    snapshot = report.snapshot()

    assert snapshot["compile_rejected"] == 1
    assert snapshot["unknown_rejects"] == 1


def test_snapshot_requires_deopt_counter_keys(monkeypatch):
    monkeypatch.setattr(
        report,
        "_trigger_stats",
        lambda: {
            "compiled_function_creations": 0,
            "machine_code_entries": 0,
            "executable_alloc_calls": 0,
            "executable_alloc_bytes": 0,
            "shadow_compile_success": 0,
            "shadow_specialized_opcodes_consumed": 0,
            "shadow_codegen_bytes": 0,
            "organic_deopt_hits": 0,
        },
    )
    monkeypatch.setattr(
        report,
        "_observe_stats",
        lambda: {"events": [], "events_dropped": 0},
    )
    monkeypatch.setattr(report, "_evaluator_installed", lambda: True)
    monkeypatch.setattr(report, "_compiled_function_count", lambda: 0)
    monkeypatch.setattr(report, "_peak_rss_bytes", lambda: 1)

    try:
        report.snapshot()
    except KeyError as error:
        assert error.args == ("forced_deopt_hits",)
    else:
        raise AssertionError("missing deopt counter was default-filled")
