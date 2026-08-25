# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Self-tests for the MR-03 shadow-surface completeness judge.

These tests do not run regrtest or test_cinderx. They pin the reviewer's
completeness contract: the frozen 440-module list must be executed (not
imported), test_cinderx must be recorded per case, and compile_success on a
passing module cannot be satisfied by a missing worker snapshot.
"""

from __future__ import annotations

from pathlib import Path
from unittest.mock import patch

from ci_pipeline.jit311 import report as jit_report
from ci_pipeline.jit311 import runners
from ci_pipeline.jit311 import shadow_surface as surface


def _snap(name: str = "test_placeholder", **overrides):
    snap = {field: 0 for field in jit_report.RUNTIME_FIELDS}
    snap["evaluator_installed"] = True
    snap["compile_requests"] = 2
    snap["compile_success"] = 2
    snap["compile_rejected"] = 0
    snap["shadow_codegen_bytes"] = 128
    snap["peak_rss_bytes"] = 4096
    for field in jit_report.HARNESS_FIELDS:
        snap[field] = None
    snap["compiled_functions"] = [
        {
            "filename": f"/Lib/test/{name}.py",
            "qualname": f"{name}.test_one",
        }
    ]
    snap.update(overrides)
    return snap


def _module(name: str, *, verdict: str = "pass", snap=None, cases=None, **extra):
    snap = snap if snap is not None else _snap(name)
    snaps = extra.pop("snapshots", [snap] if snap else [])
    cases = cases if cases is not None else {f"{name}.test_one": "pass"}
    default_rc = {"pass": 0, "skip": 0, "fail": 1, "no_result": 5, "crash": 139}.get(
        verdict, 0
    )
    record = {
        "kind": "libtest",
        "name": name,
        "verdict": verdict,
        "returncode": extra.pop("returncode", default_rc),
        "snapshots": snaps,
        "cases": cases,
        "compile_success": sum(int(s.get("compile_success") or 0) for s in snaps),
        "baseline_cases": extra.pop("baseline_cases", dict(cases)),
        "baseline_returncode": extra.pop("baseline_returncode", default_rc),
        "baseline_verdict": extra.pop("baseline_verdict", verdict),
    }
    record.update(extra)
    return record


def _cinderx_suites():
    return [
        {
            "name": name,
            "verdict": "pass",
            "returncode": 0,
            "case_count": 1,
            "cases": {"c::t": "pass"},
            "baseline_cases": {"c::t": "pass"},
            "baseline_verdict": "pass",
            "baseline_returncode": 0,
        }
        for name in surface.frozen_cinderx_suite_names()
    ]


def _cinderx(
    *,
    verdict: str = "fail",
    returncode: int = 1,
    snap=None,
    cases=None,
    baseline_cases=None,
    baseline_returncode=None,
    baseline_verdict=None,
    suites=None,
):
    if snap is None:
        snap = _snap()
        snap = dict(snap)
        snap["compiled_functions"] = [
            {
                "filename": "cinderx/PythonLib/test_cinderx/test_foo.py",
                "qualname": "TestFoo.test_bar",
            }
        ]
    else:
        snap = dict(snap)
    cases = cases if cases is not None else {
        "test_cinderx.test_foo.TestFoo.test_bar": "failure",
        "test_cinderx.test_foo.TestFoo.test_ok": "pass",
    }
    return {
        "kind": "test_cinderx",
        "name": "test_cinderx",
        "verdict": verdict,
        "returncode": returncode,
        "snapshots": [snap],
        "cases": cases,
        "compile_success": snap.get("compile_success", 0),
        "baseline_cases": dict(cases) if baseline_cases is None else baseline_cases,
        "baseline_returncode": (
            returncode if baseline_returncode is None else baseline_returncode
        ),
        "baseline_verdict": verdict if baseline_verdict is None else baseline_verdict,
        "suites": _cinderx_suites() if suites is None else suites,
    }


def _report(modules, cinderx=None, **extra):
    report = {
        "frozen_module_count": extra.pop("frozen_module_count", 440),
        "libtest_modules": modules,
        "libtest_worker_crashes": extra.pop(
            "libtest_worker_crashes",
            sum(1 for rec in modules if rec["verdict"] in ("crash", "no_result")),
        ),
        "test_cinderx": cinderx if cinderx is not None else _cinderx(),
    }
    report.update(extra)
    return report


def _full_surface():
    return _report([_module(f"test_{i}") for i in range(440)])


def test_sitecustomize_is_valid_python():
    compile(surface.SURFACE_SITECUSTOMIZE, "sitecustomize.py", "exec")
    assert "ModuleNotFoundError" in surface.SURFACE_SITECUSTOMIZE
    assert "JIT311_SURFACE_MODULE" in surface.SURFACE_SITECUSTOMIZE
    assert "JIT311_CINDERX_SITE" in surface.SURFACE_SITECUSTOMIZE
    assert "_evaluator_installed_at_start" in surface.SURFACE_SITECUSTOMIZE
    assert "compiled_functions" in surface.SURFACE_SITECUSTOMIZE


def test_stdlib_import_canary_is_still_72():
    assert len(runners.STDLIB_SHADOW_MODULES) == 72
    assert len(set(runners.STDLIB_SHADOW_MODULES)) == 72
    doc = runners.stdlib_shadow_runner.__doc__ or ""
    assert "canary" in doc.lower() or "MR-05" in doc


def test_frozen_surface_is_440():
    modules = runners.load_libtest_target_manifest()
    assert len(modules) == 440
    assert surface.load_frozen_modules() == modules


def test_healthy_surface_is_green():
    assert surface.judge_completeness(_full_surface()) == []


def test_import_only_style_zero_compile_on_executed_module_is_red():
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(
        _module(
            "test_int",
            snap=_snap(
                "test_int",
                compile_requests=0,
                compile_success=0,
                compile_rejected=0,
                compiled_functions=[],
            ),
        )
    )
    errors = surface.judge_completeness(_report(modules))
    assert any("compile_success=0" in err and "test_int" in err for err in errors)


def test_skip_without_compile_is_green():
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(
        _module(
            "test_windows",
            verdict="skip",
            cases={"test_windows.test_one": "skipped"},
            snap=_snap(
                compile_requests=0,
                compile_success=0,
                compile_rejected=0,
                compiled_functions=[],
            ),
        )
    )
    assert surface.judge_completeness(_report(modules)) == []


def test_crash_and_missing_snapshot_are_red():
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(_module("test_crash", verdict="crash", snapshots=[], cases={}))
    errors = surface.judge_completeness(_report(modules, libtest_worker_crashes=1))
    assert any("libtest_worker_crashes=1" in err for err in errors)
    assert any("test_crash" in err and "crash" in err for err in errors)
    assert any("no worker snapshots" in err for err in errors)


def test_unknown_rejects_and_machine_code_are_red():
    bad = _snap(
        "test_bad",
        unknown_rejects=1,
        compile_rejected=1,
        compile_success=1,
        compile_requests=2,
    )
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(_module("test_bad", snap=bad))
    errors = surface.judge_completeness(_report(modules))
    assert any("unknown_rejects" in err for err in errors)

    leaked = _snap("test_bad", machine_code_entries=1)
    modules[-1] = _module("test_bad", snap=leaked)
    errors = surface.judge_completeness(_report(modules))
    assert any("machine_code_entries" in err for err in errors)


def test_shrunk_surface_is_red():
    errors = surface.judge_completeness(_report([_module("test_int")]))
    assert any("440" in err for err in errors)


def test_missing_test_cinderx_is_red():
    report = _full_surface()
    report["test_cinderx"] = None
    errors = surface.judge_completeness(report)
    assert any("test_cinderx completeness record is missing" in err for err in errors)


def test_test_cinderx_pytest_failures_are_recorded_not_red():
    # Failures that already exist on the JIT-off baseline stay recorded.
    assert surface.judge_completeness(_full_surface()) == []


def test_new_test_cinderx_failure_vs_jit_off_is_red():
    report = _full_surface()
    report["test_cinderx"] = _cinderx(
        verdict="fail",
        returncode=1,
        cases={"test_cinderx.test_foo.TestFoo.test_bar": "failure"},
        baseline_cases={"test_cinderx.test_foo.TestFoo.test_bar": "pass"},
        baseline_returncode=0,
        baseline_verdict="pass",
    )
    errors = surface.judge_completeness(report)
    assert any("pass -> failure" in err or "new failure" in err for err in errors)


def test_new_test_cinderx_collection_error_is_red():
    report = _full_surface()
    report["test_cinderx"] = _cinderx(
        verdict="fail",
        returncode=2,
        baseline_returncode=0,
        baseline_verdict="pass",
        cases={},
        baseline_cases={},
    )
    errors = surface.judge_completeness(report)
    assert any("returncode 0 -> 2" in err for err in errors)


def test_baseline_test_cinderx_failure_is_recorded_not_red():
    report = _full_surface()
    report["test_cinderx"] = _cinderx(
        verdict="fail",
        returncode=1,
        cases={"suite::case": "failure"},
        baseline_cases={"suite::case": "failure"},
        baseline_returncode=1,
    )
    assert surface.judge_completeness(report) == []


def test_test_cinderx_worker_crash_is_red():
    report = _full_surface()
    report["test_cinderx"] = _cinderx(verdict="crash", returncode=-11)
    errors = surface.judge_completeness(report)
    assert any("crash" in err for err in errors)


def test_pytest_process_verdict():
    assert surface.pytest_process_verdict(0, {"a": "pass"}, False) == "pass"
    assert surface.pytest_process_verdict(1, {"a": "failure"}, False) == "fail"
    assert surface.pytest_process_verdict(2, {}, False) == "fail"
    assert surface.pytest_process_verdict(5, {}, False) == "no_result"
    assert surface.pytest_process_verdict(0, {}, True) == "crash"


def test_test_cinderx_suites_come_from_the_official_runner():
    suites = surface.load_test_cinderx_suites()
    names = [suite["name"] for suite in suites]
    assert "all_test_cinderx" in names
    assert "test_compiler" in names


def test_subset_does_not_claim_the_frozen_bar():
    report = _report(
        [_module("test_int")],
        frozen_module_count=1,
        test_cinderx=_cinderx(),
    )
    assert surface.judge_completeness(report, require_frozen=False) == []
    assert surface.judge_completeness(report, require_frozen=True)


def test_snapshot_extras_are_stripped_before_schema():
    snap = _snap()
    snap["surface_module"] = "test_int"
    snap["surface_kind"] = "libtest"
    snap["compiled_functions"] = [
        {"filename": "/Lib/test/test_int.py", "qualname": "test_one"}
    ]
    assert surface.snapshot_errors(snap) == []


def test_sequential_regrtest_success_is_pass_not_no_result():
    # 3.11.6 `python -m test test_int` (no -j) prints the module name
    # without "passed"; the epilogue plus junit is the verdict.
    log = (
        "0:00:00 load avg: 0.16 Run tests sequentially (timeout: 3 min)\n"
        "0:00:00 load avg: 0.16 [1/1] test_int\n"
        "\n"
        "== Tests result: SUCCESS ==\n"
        "\n"
        "1 test OK.\n"
        "Result: SUCCESS\n"
    )
    cases = {"test.test_int.IntTestCases.test_bit_length": "pass"}
    assert (
        surface.infer_libtest_verdict("test_int", log, 0, cases, False) == "pass"
    )
    assert (
        surface.infer_libtest_verdict("test_int", log, 0, {}, False) == "pass"
    )


def test_unrelated_compile_success_does_not_satisfy_the_target():
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(
        _module(
            "test_int",
            snap=_snap(
                "test_int",
                compile_success=1,
                compile_requests=1,
                compiled_functions=[
                    {
                        "filename": "/tmp/sitecustomize.py",
                        "qualname": "_jit311_surface_emit",
                    }
                ],
            ),
        )
    )
    errors = surface.judge_completeness(_report(modules))
    assert any(
        "not attributed" in err and "test_int" in err for err in errors
    )


def test_dotted_libtest_name_matches_nested_co_filename():
    name = "test.test_asyncio.test_locks"
    rec = _module(
        name,
        snap=_snap(
            name,
            compiled_functions=[
                {
                    "filename": "/usr/lib64/python3.11/test/test_asyncio/test_locks.py",
                    "qualname": "LockTests.test_acquire",
                }
            ],
        ),
        cases={f"{name}.LockTests.test_acquire": "pass"},
    )
    assert surface.compiled_belongs_to_target(
        "/usr/lib64/python3.11/test/test_asyncio/test_locks.py",
        "LockTests.test_acquire",
        rec,
    )
    assert not surface.compiled_belongs_to_target(
        "/usr/lib64/python3.11/asyncio/locks.py",
        "Lock.acquire",
        rec,
    )
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(rec)
    assert surface.judge_completeness(_report(modules)) == []


def test_package_libtest_matches_files_under_the_package():
    rec = _module(
        "test_json",
        snap=_snap(
            "test_json",
            compiled_functions=[
                {
                    "filename": "/usr/lib64/python3.11/test/test_json/test_decode.py",
                    "qualname": "TestDecode.test_dumps",
                }
            ],
        ),
        cases={"test_json.TestDecode.test_dumps": "pass"},
    )
    assert surface.compiled_belongs_to_target(
        "/usr/lib64/python3.11/test/test_json/test_decode.py",
        "TestDecode.test_dumps",
        rec,
    )
    assert not surface.compiled_belongs_to_target(
        "/usr/lib64/python3.11/json/decoder.py",
        "JSONDecoder.decode",
        rec,
    )


def test_empty_pass_with_stdlib_compiles_is_green_against_matching_baseline():
    # Platform skips (test_winreg, test_idle) report pass/0 cases while the
    # process still compiles argparse. Dual-run already holds the empty
    # junit against JIT-off; do not demand a compile from a file that
    # never ran.
    rec = _module(
        "test_winreg",
        verdict="pass",
        returncode=0,
        cases={},
        snap=_snap(
            "test_winreg",
            compile_success=1,
            compile_requests=1,
            compiled_functions=[
                {
                    "filename": "/usr/lib64/python3.11/argparse.py",
                    "qualname": "ArgumentParser.parse_args",
                }
            ],
        ),
    )
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(rec)
    assert surface.judge_completeness(_report(modules)) == []


def test_codecencodings_matches_multibytecodec_support_not_argparse():
    rec = _module(
        "test_codecencodings_cn",
        snap=_snap(
            "test_codecencodings_cn",
            compiled_functions=[
                {
                    "filename": "/usr/lib64/python3.11/test/multibytecodec_support.py",
                    "qualname": "TestBase.test_chunkcoding",
                }
            ],
        ),
        cases={"test.test_codecencodings_cn.Test_GB18030.test_chunkcoding": "pass"},
    )
    assert surface.compiled_belongs_to_target(
        "/usr/lib64/python3.11/test/multibytecodec_support.py",
        "TestBase.test_chunkcoding",
        rec,
    )
    assert not surface.compiled_belongs_to_target(
        "/usr/lib64/python3.11/argparse.py",
        "ArgumentParser.parse_args",
        rec,
    )


def test_ctypes_matches_ctypes_test_package_not_ctypes_impl():
    rec = _module(
        "test_ctypes",
        snap=_snap(
            "test_ctypes",
            compiled_functions=[
                {
                    "filename": "/usr/lib64/python3.11/ctypes/test/test_structures.py",
                    "qualname": "StructTest.test_fields",
                }
            ],
        ),
        cases={"test.test_ctypes.StructTest.test_fields": "pass"},
    )
    assert surface.compiled_belongs_to_target(
        "/usr/lib64/python3.11/ctypes/test/test_structures.py",
        "StructTest.test_fields",
        rec,
    )
    assert not surface.compiled_belongs_to_target(
        "/usr/lib64/python3.11/ctypes/__init__.py",
        "CFuncPtr.__call__",
        rec,
    )


def test_lib2to3_matches_lib2to3_tests_not_library():
    rec = _module(
        "test_lib2to3",
        snap=_snap(
            "test_lib2to3",
            compiled_functions=[
                {
                    "filename": "/usr/lib64/python3.11/lib2to3/tests/test_fixers.py",
                    "qualname": "TestFixers.test_print",
                }
            ],
        ),
        cases={"test.test_lib2to3.TestFixers.test_print": "pass"},
    )
    assert surface.compiled_belongs_to_target(
        "/usr/lib64/python3.11/lib2to3/tests/test_fixers.py",
        "TestFixers.test_print",
        rec,
    )
    assert not surface.compiled_belongs_to_target(
        "/usr/lib64/python3.11/lib2to3/pytree.py",
        "Node.__init__",
        rec,
    )


def test_fileutils_capi_one_shot_is_green_against_matching_baseline():
    rec = _module(
        "test_fileutils",
        snap=_snap(
            "test_fileutils",
            compiled_functions=[
                {
                    "filename": "/usr/lib64/python3.11/argparse.py",
                    "qualname": "ArgumentParser.parse_args",
                }
            ],
        ),
        cases={"test.test_fileutils.PathTests.test_capi_normalize_path": "pass"},
    )
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(rec)
    assert surface.judge_completeness(_report(modules)) == []


def test_longexp_eval_one_shot_is_green_against_matching_baseline():
    rec = _module(
        "test_longexp",
        snap=_snap(
            "test_longexp",
            compiled_functions=[
                {
                    "filename": "/usr/lib64/python3.11/argparse.py",
                    "qualname": "ArgumentParser.parse_args",
                }
            ],
        ),
        cases={"test.test_longexp.LongExpText.test_longexp": "pass"},
    )
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(rec)
    assert surface.judge_completeness(_report(modules)) == []


def test_fileutils_still_red_on_case_drift():
    rec = _module(
        "test_fileutils",
        verdict="fail",
        returncode=1,
        snap=_snap(
            "test_fileutils",
            compiled_functions=[
                {
                    "filename": "/usr/lib64/python3.11/argparse.py",
                    "qualname": "ArgumentParser.parse_args",
                }
            ],
        ),
        cases={"test.test_fileutils.PathTests.test_capi_normalize_path": "failure"},
        baseline_cases={
            "test.test_fileutils.PathTests.test_capi_normalize_path": "pass"
        },
        baseline_returncode=0,
        baseline_verdict="pass",
    )
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(rec)
    errors = surface.judge_completeness(_report(modules))
    assert any("pass -> failure" in err for err in errors)


def test_executed_module_with_only_stdlib_compiles_is_red():
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(
        _module(
            "test_int",
            snap=_snap(
                "test_int",
                compile_success=1,
                compile_requests=1,
                compiled_functions=[
                    {
                        "filename": "/usr/lib64/python3.11/argparse.py",
                        "qualname": "ArgumentParser.parse_args",
                    }
                ],
            ),
        )
    )
    errors = surface.judge_completeness(_report(modules))
    assert any(
        "not attributed" in err and "test_int" in err for err in errors
    )


def test_new_libtest_failure_vs_jit_off_is_red():
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(
        _module(
            "test_int",
            verdict="fail",
            cases={"test_int.test_one": "failure"},
            baseline_cases={"test_int.test_one": "pass"},
            baseline_returncode=0,
            baseline_verdict="pass",
        )
    )
    errors = surface.judge_completeness(_report(modules))
    assert any(
        "pass -> failure" in err or "new failure" in err for err in errors
    )


def test_baseline_libtest_failure_is_recorded_not_red():
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(
        _module(
            "test_int",
            verdict="fail",
            cases={"test_int.test_one": "failure"},
            baseline_cases={"test_int.test_one": "failure"},
            baseline_returncode=1,
        )
    )
    assert surface.judge_completeness(_report(modules)) == []


def test_empty_shadow_junit_against_passing_baseline_is_red():
    # Reviewer PoC: baseline pass/rc=0/cases=1, shadow fail/rc=1/cases=0,
    # with attributed compile_success, must not go green.
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(
        _module(
            "test_int",
            verdict="fail",
            returncode=1,
            cases={},
            baseline_cases={"test_int.test_one": "pass"},
            baseline_returncode=0,
            baseline_verdict="pass",
        )
    )
    errors = surface.judge_completeness(_report(modules))
    joined = "\n".join(errors)
    assert "baseline case disappeared" in joined
    assert "verdict pass -> fail" in joined
    assert "returncode 0 -> 1" in joined


def test_pass_to_skip_is_red():
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(
        _module(
            "test_int",
            verdict="skip",
            cases={"test_int.test_one": "skipped"},
            baseline_cases={"test_int.test_one": "pass"},
            baseline_returncode=0,
            baseline_verdict="pass",
            snap=_snap(
                "test_int",
                compile_requests=0,
                compile_success=0,
                compile_rejected=0,
                compiled_functions=[],
            ),
        )
    )
    errors = surface.judge_completeness(_report(modules))
    joined = "\n".join(errors)
    assert "pass -> skipped" in joined or "verdict pass -> skip" in joined


def test_shadow_only_extra_pass_is_red():
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(
        _module(
            "test_int",
            cases={"test_int.a": "pass", "test_int.b": "pass"},
            baseline_cases={"test_int.a": "pass"},
        )
    )
    errors = surface.judge_completeness(_report(modules))
    assert any("extra case" in err and "test_int.b" in err for err in errors)


def test_skipped_to_pass_is_red():
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(
        _module(
            "test_int",
            verdict="pass",
            cases={"test_int.a": "pass"},
            baseline_cases={"test_int.a": "skipped"},
            baseline_verdict="skip",
        )
    )
    errors = surface.judge_completeness(_report(modules))
    joined = "\n".join(errors)
    assert "skipped -> pass" in joined
    assert "verdict skip -> pass" in joined


def test_empty_junit_on_both_arms_is_red_for_test_int():
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(
        _module(
            "test_int",
            verdict="pass",
            returncode=0,
            cases={},
            snap=_snap(
                "test_int",
                compiled_functions=[
                    {
                        "filename": "/usr/lib64/python3.11/argparse.py",
                        "qualname": "ArgumentParser.parse_args",
                    }
                ],
            ),
        )
    )
    errors = surface.judge_completeness(_report(modules))
    assert any("empty junit" in err and "test_int" in err for err in errors)


def test_fileutils_empty_both_arms_is_red():
    rec = _module(
        "test_fileutils",
        verdict="pass",
        returncode=0,
        cases={},
        snap=_snap(
            "test_fileutils",
            compiled_functions=[
                {
                    "filename": "/usr/lib64/python3.11/argparse.py",
                    "qualname": "ArgumentParser.parse_args",
                }
            ],
        ),
    )
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(rec)
    errors = surface.judge_completeness(_report(modules))
    assert any(
        "missing required case" in err and "test_fileutils" in err for err in errors
    )


def test_cinderx_suite_pass_to_fail_is_red_even_if_aggregate_stays_fail():
    report = _full_surface()
    suites = report["test_cinderx"]["suites"]
    suites[0]["verdict"] = "fail"
    suites[0]["returncode"] = 1
    suites[0]["cases"] = {"a": "failure"}
    suites[0]["baseline_cases"] = {"a": "failure"}
    suites[0]["baseline_verdict"] = "fail"
    suites[0]["baseline_returncode"] = 1
    report["test_cinderx"]["verdict"] = "fail"
    report["test_cinderx"]["returncode"] = 1
    target = next(
        rec
        for rec in suites[1:]
        if rec["name"] not in surface.SHADOW_N_A_SUITES
    )
    target["verdict"] = "fail"
    target["returncode"] = 1
    target["cases"] = {"b": "failure"}
    target["baseline_cases"] = {"b": "pass"}
    target["baseline_verdict"] = "pass"
    target["baseline_returncode"] = 0
    errors = surface.judge_completeness(report)
    joined = "\n".join(errors)
    assert target["name"] in joined
    assert "pass -> failure" in joined


def test_instrumentation_suite_wrapper_passes_are_na():
    report = _full_surface()
    for rec in report["test_cinderx"]["suites"]:
        if rec["name"] != "test_jit_support_instrumentation":
            continue
        rec["verdict"] = "pass"
        rec["returncode"] = 0
        rec["case_count"] = 20
        rec["cases"] = {f"w{i}": "pass" for i in range(20)}
        rec["baseline_cases"] = {f"w{i}": "pass" for i in range(20)}
        rec["baseline_verdict"] = "pass"
        rec["baseline_returncode"] = 0
        break
    assert "test_jit_support_instrumentation" in surface.SHADOW_N_A_SUITES
    assert surface.judge_completeness(report) == []


def test_baseline_case_disappeared_from_shadow_is_red():
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(
        _module(
            "test_int",
            verdict="pass",
            cases={"test_int.test_two": "pass"},
            baseline_cases={
                "test_int.test_one": "pass",
                "test_int.test_two": "pass",
            },
            baseline_returncode=0,
            baseline_verdict="pass",
        )
    )
    errors = surface.judge_completeness(_report(modules))
    assert any(
        "baseline case disappeared" in err and "test_int.test_one" in err
        for err in errors
    )


def test_empty_cinderx_suite_is_red_even_if_other_suites_pass():
    report = _full_surface()
    suites = report["test_cinderx"]["suites"]
    suites[0]["case_count"] = 0
    suites[0]["verdict"] = "no_result"
    suites[0]["returncode"] = 5
    errors = surface.judge_completeness(report)
    assert any("case_count=0" in err or "verdict=no_result" in err for err in errors)


def test_missing_cinderx_suite_is_red():
    report = _full_surface()
    report["test_cinderx"]["suites"] = report["test_cinderx"]["suites"][1:]
    errors = surface.judge_completeness(report)
    assert any("suite names" in err for err in errors)


def test_apply_suite_env_does_not_copy_official_runner_env():
    env = surface.apply_suite_env(
        dict(surface.SHADOW_ENV),
        {
            "name": "x",
            "env": {
                "CINDERX_JIT_MODE": "turbo",
                "PYTHONJITLIGHTWEIGHTFRAME": "1",
            },
        },
    )
    assert env["CINDERX_JIT_MODE"] == "shadow"
    assert "PYTHONJITLIGHTWEIGHTFRAME" not in env


def test_all_official_suites_keep_shadow_without_runner_env():
    for suite in surface.load_test_cinderx_suites():
        env = surface.apply_suite_env(dict(surface.SHADOW_ENV), suite)
        assert env["CINDERX_JIT_MODE"] == "shadow"
        assert env["CINDERX_EVAL_MODE"] == "cinder"
        assert env["CINDERX_PLUGIN_ENABLE"] == "1"
        assert "PYTHONJITLIGHTWEIGHTFRAME" not in env
        assert "CINDERX_OSR_ENABLED" not in env
        assert "PYTHONJITSUPPORTINSTRUMENTATION" not in env
        if suite.get("allow_oss"):
            assert env["CINDERX_TEST_ALLOW_OSS_IMPORTS"] == "1"


def test_run_test_cinderx_subprocess_env_matches_each_official_suite(tmp_path=None):
    suites = surface.load_test_cinderx_suites()
    captured = []

    class Fake:
        returncode = 0

    def fake_run(cmd, **kwargs):
        captured.append(kwargs["env"])
        return Fake()

    dummy_snap = _snap()
    dummy_snap["compiled_functions"] = [
        {
            "filename": "cinderx/PythonLib/test_cinderx/test_foo.py",
            "qualname": "TestFoo.test_bar",
        }
    ]
    if tmp_path is None:
        import tempfile
        tmp_ctx = tempfile.TemporaryDirectory()
        out = Path(tmp_ctx.name)
    else:
        tmp_ctx = None
        out = Path(tmp_path)
    try:
        with (
            patch.object(surface.subprocess, "run", fake_run),
            patch.object(surface, "parse_junit", lambda path: {"c::t": "pass"}),
            patch.object(surface, "_load_snapshots", lambda path: [dummy_snap]),
        ):
            surface.run_test_cinderx(
                python="python3.11",
                out=out,
                startup=out / "startup",
                timeout=60,
                base_env={},
            )
    finally:
        if tmp_ctx is not None:
            tmp_ctx.cleanup()
    shadow_envs = [env for env in captured if env.get("CINDERX_JIT_MODE") == "shadow"]
    off_envs = [env for env in captured if env.get("CINDERX_JIT_MODE") == "off"]
    assert len(shadow_envs) == len(suites)
    assert len(off_envs) == len(suites)
    by_name = {
        env["JIT311_SURFACE_MODULE"].split(".", 1)[-1]: env for env in shadow_envs
    }
    for suite in suites:
        env = by_name[suite["name"]]
        assert env["CINDERX_JIT_MODE"] == "shadow"
        assert env["CINDERX_EVAL_MODE"] == "cinder"
        assert env["CINDERX_PLUGIN_ENABLE"] == "1"
        for key in suite.get("env") or {}:
            assert key not in env, (suite["name"], key)
        if suite.get("allow_oss"):
            assert env["CINDERX_TEST_ALLOW_OSS_IMPORTS"] == "1"
