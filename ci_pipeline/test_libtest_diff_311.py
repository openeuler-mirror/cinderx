"""Regression semantics of the 3.11 Lib/test differential engine.

The engine has two comparators with two different contracts.  The JIT-off
differential is regression-only red: anything that passed on the stock arm
and no longer passes on the CinderX arm is a regression, at the module
level and at the case level -- including a case that silently vanishes
from a module that otherwise still reports.  The execute differential is
symmetric: the two arms must produce the same case identities in the same
states, and the asymmetries that are genuinely environmental are named in
a frozen baseline.
"""

import inspect
import json

import ci_pipeline.libtest_diff_311 as libtest_diff


def _arm(modules, cases):
    return {"modules": modules, "cases": cases}


def test_case_failure_is_a_regression():
    stock = _arm({"test_x": "pass"}, {"test.test_x.T.test_a": "pass"})
    cinderx = _arm({"test_x": "fail"}, {"test.test_x.T.test_a": "failure"})
    diff = libtest_diff.diff_results(stock, cinderx)
    assert diff["case_regressions"] == {
        "test.test_x.T.test_a": {"stock": "pass", "cinderx": "failure"}
    }
    assert diff["module_regressions"] == {
        "test_x": {"stock": "pass", "cinderx": "fail"}
    }


def test_case_vanishing_from_a_reporting_module_is_a_regression():
    stock = _arm(
        {"test_x": "pass"},
        {"test.test_x.T.test_a": "pass", "test.test_x.T.test_b": "pass"},
    )
    cinderx = _arm({"test_x": "pass"}, {"test.test_x.T.test_a": "pass"})
    diff = libtest_diff.diff_results(stock, cinderx)
    assert diff["case_regressions"] == {
        "test.test_x.T.test_b": {"stock": "pass", "cinderx": "missing"}
    }


def test_dead_module_reports_once_at_module_level_not_per_case():
    stock = _arm(
        {"test_x": "pass"},
        {"test.test_x.T.test_a": "pass", "test.test_x.T.test_b": "pass"},
    )
    cinderx = _arm({"test_x": "no_result"}, {})
    diff = libtest_diff.diff_results(stock, cinderx)
    assert diff["module_regressions"] == {
        "test_x": {"stock": "pass", "cinderx": "no_result"}
    }
    assert diff["case_regressions"] == {}


def test_case_pass_to_skip_is_a_regression():
    # A case that starts skipping under the evaluator shrank coverage
    # while staying green -- same rule as the module level.
    stock = _arm({"test_x": "pass"}, {"test.test_x.T.test_a": "pass"})
    cinderx = _arm({"test_x": "pass"}, {"test.test_x.T.test_a": "skipped"})
    diff = libtest_diff.diff_results(stock, cinderx)
    assert diff["case_regressions"] == {
        "test.test_x.T.test_a": {"stock": "pass", "cinderx": "skipped"}
    }
    assert diff["module_regressions"] == {}


def test_crashed_worker_keeps_a_distinct_verdict():
    # 3.11.6 regrtest prints "<name> process crashed" (libregrtest/
    # runtest.py); the bare form is kept for robustness.
    log = "0:00:01 load avg: 1.0 [1/2] test_x process crashed (SIGSEGV)\n" \
          "0:00:01 load avg: 1.0 [2/2] test_y crashed"
    verdicts = libtest_diff.parse_regrtest_modules(log, ["test_x", "test_y"])
    assert verdicts == {"test_x": "crash", "test_y": "crash"}
    assert libtest_diff.crash_count(verdicts) == 2


def test_symmetric_crash_cannot_launder_to_baseline():
    # Two arms crashing identically must never read as green: the verdict
    # stays "crash" (not "fail"), the arm-level crash gate fires before
    # any diff, and an asymmetric crash is a regression in the diff too.
    stock = _arm({"test_x": "pass"}, {})
    cinderx = _arm({"test_x": "crash"}, {})
    diff = libtest_diff.diff_results(stock, cinderx)
    assert diff["module_regressions"] == {
        "test_x": {"stock": "pass", "cinderx": "crash"}
    }


def test_stock_failures_are_baseline_not_regressions():
    stock = _arm({"test_x": "fail"}, {"test.test_x.T.test_a": "failure"})
    cinderx = _arm({"test_x": "fail"}, {"test.test_x.T.test_a": "failure"})
    diff = libtest_diff.diff_results(stock, cinderx)
    assert diff["case_regressions"] == {}
    assert diff["module_regressions"] == {}


def test_module_verdicts_come_from_regrtest_lines():
    log = (
        "0:00:00 load avg: 7.78 [  1/440] test_a passed\n"
        "0:00:00 load avg: 7.78 [  2/440/1] test_b failed (uncaught exception)\n"
        "test test_b crashed -- Traceback (most recent call last):\n"
        "0:00:01 load avg: 7.78 [  3/440/1] test_c skipped (resource denied)\n"
        "0:00:01 load avg: 7.78 [  4/440/1] test_d skipped\n"
        "0:00:01 load avg: 7.78 [  5/440/1] test.test_pkg.test_sub passed\n"
        "0:00:02 load avg: 7.78 [  6/440/1] test_unrequested passed\n"
        "0:00:02 load avg: 7.78 [  7/440/2] test_e failed (env changed)\n"
    )
    requested = ["test_a", "test_b", "test_c", "test_d",
                 "test.test_pkg.test_sub", "test_e", "test_worker_died"]
    verdicts = libtest_diff.parse_regrtest_modules(log, requested)
    assert verdicts == {
        "test_a": "pass",
        "test_b": "fail",
        "test_c": "skip",
        "test_d": "skip",
        "test.test_pkg.test_sub": "pass",
        "test_e": "fail",
    }
    # A requested module without a result line stays absent: the caller
    # records it as no_result, the only verdict left for a dead worker.
    assert "test_worker_died" not in verdicts
    # Unrequested names (phantom keys) never enter the accounting.
    assert "test_unrequested" not in verdicts


def test_pass_to_skip_is_a_regression():
    stock = _arm({"test_x": "pass"}, {"test.test_x.T.test_a": "pass"})
    cinderx = _arm({"test_x": "skip"}, {})
    diff = libtest_diff.diff_results(stock, cinderx)
    assert diff["module_regressions"] == {
        "test_x": {"stock": "pass", "cinderx": "skip"}
    }
    # The module-level entry carries the signal; no per-case spam.
    assert diff["case_regressions"] == {}


def test_symmetric_skip_and_fail_are_baseline():
    stock = _arm({"test_x": "skip", "test_y": "fail"}, {})
    cinderx = _arm({"test_x": "skip", "test_y": "fail"}, {})
    diff = libtest_diff.diff_results(stock, cinderx)
    assert diff["module_regressions"] == {}
    assert diff["module_warnings"] == {}


def test_attest_reader(tmp_path):
    attest = tmp_path / "attest.log"
    count, ok = libtest_diff.read_attest(attest)
    assert (count, ok) == (0, True)

    attest.write_text("101 True\n102 True\n103 True\n")
    assert libtest_diff.read_attest(attest) == (3, True)

    attest.write_text("101 True\n102 False\n")
    assert libtest_diff.read_attest(attest) == (2, False)


def test_startup_sitecustomize_compiles():
    compile(libtest_diff.STARTUP_SITECUSTOMIZE, "sitecustomize.py", "exec")


def test_missing_verdicts_flags_only_unreported_modules():
    # pass/fail/skip are all verdicts; only no_result means the module was
    # never reported at all, which must fail the arm rather than flow into
    # a possibly-symmetric (false green) diff.
    assert libtest_diff.missing_verdicts(
        {"a": "pass", "b": "fail", "c": "skip"}
    ) == 0
    assert libtest_diff.missing_verdicts(
        {"a": "pass", "b": "no_result", "c": "no_result"}
    ) == 2


def test_package_path_cases_resolve_to_their_requested_module():
    stock = _arm(
        {"test.test_pkg.test_sub": "pass"},
        {"test.test_pkg.test_sub.T.test_a": "pass"},
    )
    cinderx = _arm({"test.test_pkg.test_sub": "no_result"}, {})
    diff = libtest_diff.diff_results(stock, cinderx)
    # The vanished case maps back onto the dead package module and is
    # suppressed in favor of the module-level entry.
    assert diff["case_regressions"] == {}
    assert "test.test_pkg.test_sub" in diff["module_regressions"]


def test_arm_environment_is_sanitized():
    # An inherited PYTHONPATH / sitecustomize hook / CINDERX_* variable
    # could activate machinery in BOTH arms and fake a neutral diff.
    dirty = {
        "PATH": "/bin",
        "PYTHONPATH": "/somewhere/evil",
        "PYTHONSTARTUP": "/evil.py",
        "PYTHONHOME": "/opt/py",
        "PYTHONHASHSEED": "random",
        "PYTHONJITAUTO": "4",
        "PARALLEL_GC_THRESHOLD": "9",
        "CINDERX_DIFF_ATTEST": "/tmp/x",
        "CINDERX_JIT_MODE": "shadow",
    }
    env = libtest_diff.arm_environment(dirty)
    # The inherited random hash seed is REPLACED, not merely defaulted.
    assert env == {"PATH": "/bin", "PYTHONHASHSEED": "0"}, env


def test_stock_startup_attests_purity(tmp_path):
    import subprocess
    import sys as _sys

    startup = tmp_path / "startup"
    startup.mkdir()
    (startup / "sitecustomize.py").write_text(libtest_diff.STOCK_SITECUSTOMIZE)
    ledger = tmp_path / "ledger.log"
    base_env = {
        "PYTHONPATH": str(startup),
        "CINDERX_STOCK_ATTEST": str(ledger),
        "PATH": "/usr/bin:/bin",
    }

    subprocess.run([_sys.executable, "-c", "pass"], env=base_env, check=True)
    subprocess.run(
        [_sys.executable, "-c", "import sys; sys.modules['cinderx'] = sys"],
        env=base_env, check=True,
    )
    count, clean = libtest_diff.read_stock_attest(ledger)
    assert count == 2 and not clean
    lines = ledger.read_text().splitlines()
    assert lines[0] == "clean"
    assert lines[1] == "POLLUTED:cinderx"


def test_arm_completion_discipline():
    log_full = (
        "0:00:01 load avg: 1.0 [1/2] test_a passed\n"
        "0:00:01 load avg: 1.0 [2/2] test_b passed\n"
        "== Tests result: SUCCESS ==\n"
    )
    assert libtest_diff.arm_run_completed(0, log_full) is None
    assert libtest_diff.arm_run_completed(2, log_full) is None
    assert libtest_diff.arm_run_completed(3, log_full) is None
    # Full per-module verdicts plus a signal death: the verdict lines must
    # not certify a run whose main process was killed.
    err = libtest_diff.arm_run_completed(-9, log_full)
    assert err and "abnormally" in err
    assert libtest_diff.arm_run_completed(130, log_full)
    assert libtest_diff.arm_run_completed(1, log_full)
    # A normal-looking exit code without the completion epilogue is a
    # truncated run, not a completed one.
    assert libtest_diff.arm_run_completed(
        0, "0:00:01 load avg: 1.0 [1/1] test_a passed\n"
    )


def test_worker_thread_failure_cannot_be_certified():
    # 3.11.6 regrtest reports an internal worker-thread death AFTER the
    # module verdicts, then still prints a normal FAILURE epilogue and
    # exits 2 -- verdict parsing alone would certify the run and publish
    # worker_crashes=0.  The real output shape must be refused.
    log = (
        "0:00:01 load avg: 1.0 [1/2] test_a passed\n"
        "0:00:01 load avg: 1.0 [2/2] test_b passed\n"
        "regrtest worker thread failed: Traceback (most recent call last):\n"
        '  File "/usr/lib64/python3.11/test/libregrtest/runtest_mp.py"\n'
        "== Tests result: FAILURE ==\n"
    )
    err = libtest_diff.arm_run_completed(2, log)
    assert err and "harness-internal" in err

# -- the execute arm's symmetric comparator ------------------------------


def _arms(stock_cases, execute_cases, modules=("test_x", "pass")):
    mod, verdict = modules
    return (
        {"modules": {mod: verdict}, "cases": stock_cases},
        {"modules": {mod: verdict}, "cases": execute_cases},
    )


def test_symmetric_arms_agree():
    a, b = _arms({"test_x.T.a": "pass"}, {"test_x.T.a": "pass"})
    report = libtest_diff.diff_results_symmetric(a, b, {})
    assert report["differences"] == {}
    assert report["unexpected"] == {}


def test_a_case_that_only_the_execute_arm_passes_is_a_difference():
    # The regression-only comparator walks stock's PASSING cases, so a
    # failure that turns into a pass under Auto-JIT is invisible to it.
    # It is still a semantic difference the execute mode must not produce.
    a, b = _arms({"test_x.T.a": "failure"}, {"test_x.T.a": "pass"})
    assert libtest_diff.diff_results(a, b)["case_regressions"] == {}
    report = libtest_diff.diff_results_symmetric(a, b, {})
    assert report["unexpected"] == {
        "test_x.T.a": {"stock": "failure", "execute": "pass"}
    }


def test_a_case_that_changes_how_it_fails_is_a_difference():
    a, b = _arms({"test_x.T.a": "failure"}, {"test_x.T.a": "error"})
    assert libtest_diff.diff_results(a, b)["case_regressions"] == {}
    report = libtest_diff.diff_results_symmetric(a, b, {})
    assert report["unexpected"] == {
        "test_x.T.a": {"stock": "failure", "execute": "error"}
    }


def test_a_case_only_the_execute_arm_ran_is_a_difference():
    a, b = _arms({}, {"test_x.T.new": "pass"})
    report = libtest_diff.diff_results_symmetric(a, b, {})
    assert report["unexpected"] == {
        "test_x.T.new": {"stock": "missing", "execute": "pass"}
    }


def test_a_module_verdict_difference_is_reported():
    a = {"modules": {"test_x": "pass"}, "cases": {}}
    b = {"modules": {"test_x": "skip"}, "cases": {}}
    report = libtest_diff.diff_results_symmetric(a, b, {})
    assert report["unexpected"] == {
        "<module> test_x": {"stock": "pass", "execute": "skip"}
    }


def test_a_baselined_asymmetry_is_allowed():
    a, b = _arms({"test_x.T.a": "pass"}, {"test_x.T.a": "skipped"})
    allowed = {"test_x.T.a": {"stock": "pass", "execute": "skipped"}}
    report = libtest_diff.diff_results_symmetric(a, b, allowed)
    assert report["differences"] and report["unexpected"] == {}
    assert report["stale_baseline"] == {}


def test_a_baseline_entry_matching_a_different_state_does_not_excuse_it():
    # The allowance is for one specific asymmetry, not for the case.
    a, b = _arms({"test_x.T.a": "pass"}, {"test_x.T.a": "failure"})
    allowed = {"test_x.T.a": {"stock": "pass", "execute": "skipped"}}
    report = libtest_diff.diff_results_symmetric(a, b, allowed)
    assert report["unexpected"] == {
        "test_x.T.a": {"stock": "pass", "execute": "failure"}
    }
    assert report["stale_baseline"] == allowed


def test_a_baseline_entry_that_stopped_reproducing_is_reported():
    # An allowance nobody can justify any more is how a gate stops gating.
    a, b = _arms({"test_x.T.a": "pass"}, {"test_x.T.a": "pass"})
    allowed = {"test_x.T.a": {"stock": "pass", "execute": "skipped"}}
    report = libtest_diff.diff_results_symmetric(a, b, allowed)
    assert report["unexpected"] == {}
    assert report["stale_baseline"] == allowed


def test_a_missing_baseline_file_is_an_empty_allowance(tmp_path):
    assert libtest_diff.load_execute_baseline(tmp_path / "nope.json") == {}


def test_a_baseline_file_round_trips(tmp_path):
    path = tmp_path / "baseline.json"
    entry = {"test_x.T.a": {"stock": "pass", "execute": "skipped"}}
    path.write_text(json.dumps({"comment": "why", "asymmetries": entry}))
    assert libtest_diff.load_execute_baseline(path) == entry

def test_same_failure_tag_different_reason_is_a_difference():
    # The reviewer's case: both arms report `failure`, for unrelated
    # reasons.  A comparator that stops at the tag calls them identical.
    a = {
        "modules": {"test_x": "pass"},
        "cases": {"test_x.T.a": "failure"},
        "diagnostics": {
            "test_x.T.a": "TypeError: foo() missing 1 required argument"
        },
    }
    b = {
        "modules": {"test_x": "pass"},
        "cases": {"test_x.T.a": "failure"},
        "diagnostics": {"test_x.T.a": "TypeError: unsupported operand type"},
    }
    report = libtest_diff.diff_results_symmetric(a, b, {})
    assert report["unexpected"] == {
        "<diagnostic> test_x.T.a": {
            "stock": "TypeError: foo() missing 1 required argument",
            "execute": "TypeError: unsupported operand type",
        }
    }


def test_identical_diagnostics_are_not_a_difference():
    diag = {"test_x.T.a": "TypeError: same reason"}
    arm = {
        "modules": {"test_x": "pass"},
        "cases": {"test_x.T.a": "failure"},
        "diagnostics": diag,
    }
    assert libtest_diff.diff_results_symmetric(arm, dict(arm), {})["differences"] == {}


def test_a_state_difference_does_not_also_report_a_diagnostic():
    # Saying it twice buries the finding.
    a = {
        "modules": {},
        "cases": {"test_x.T.a": "failure"},
        "diagnostics": {"test_x.T.a": "TypeError: x"},
    }
    b = {"modules": {}, "cases": {"test_x.T.a": "pass"}, "diagnostics": {}}
    report = libtest_diff.diff_results_symmetric(a, b, {})
    assert list(report["differences"]) == ["test_x.T.a"]


def test_a_different_skip_reason_is_a_difference():
    a = {
        "modules": {},
        "cases": {"test_x.T.a": "skipped"},
        "diagnostics": {"test_x.T.a": "skipped: needs network"},
    }
    b = {
        "modules": {},
        "cases": {"test_x.T.a": "skipped"},
        "diagnostics": {"test_x.T.a": "skipped: JIT does not support this"},
    }
    report = libtest_diff.diff_results_symmetric(a, b, {})
    assert "<diagnostic> test_x.T.a" in report["unexpected"]


def test_diagnostic_normalization_removes_run_to_run_noise():
    n = libtest_diff.normalize_diagnostic
    assert n("AssertionError: <obj at 0x7f9a1c>") == n("AssertionError: <obj at 0xdeadbeef>")
    assert n("OSError: /tmp/pytest-abc/x") == n("OSError: /tmp/pytest-zzz/x")
    assert n("RuntimeError:  spaced   out ") == "RuntimeError: spaced out"
    # Real differences survive normalization.
    assert n("TypeError: a") != n("TypeError: b")

# -- trigger attribution --------------------------------------------------


def _ledger(tmp_path, rows):
    path = tmp_path / "trigger.log"
    path.write_text("".join(" ".join(str(f) for f in row) + "\n" for row in rows))
    return path


def test_a_worker_that_compiled_its_own_module_is_attributed(tmp_path):
    proof = libtest_diff.read_trigger_ledger(
        _ledger(tmp_path, [[11, "worker", "test_call", 500, 9, 4]])
    )
    assert proof["executing_tests"] == {"test_call"}
    assert proof["compiling_tests"] == {"test_call"}
    assert proof["unattributed_tests"] == set()


def test_harness_only_machine_code_is_not_attributed_to_the_module(tmp_path):
    # The counters are process-wide: sitecustomize, the import machinery
    # and regrtest's harness all move them.  A worker that entered machine
    # code but compiled nothing of its own module proves only that CinderX
    # ran in that process.
    proof = libtest_diff.read_trigger_ledger(
        _ledger(tmp_path, [[11, "worker", "test_call", 500, 9, 0]])
    )
    assert proof["executing_tests"] == {"test_call"}
    assert proof["compiling_tests"] == set()


def test_an_unknown_attribution_is_not_counted_as_evidence(tmp_path):
    # -1 means the module could not be resolved at exit.  Unknown must not
    # read as either yes or no.
    proof = libtest_diff.read_trigger_ledger(
        _ledger(tmp_path, [[11, "worker", "test_call", 500, 9, -1]])
    )
    assert proof["compiling_tests"] == set()
    assert proof["unattributed_tests"] == {"test_call"}


def test_non_worker_rows_are_never_attributed(tmp_path):
    proof = libtest_diff.read_trigger_ledger(
        _ledger(tmp_path, [[11, "other", "-", 900, 20, -1]])
    )
    assert proof["workers"] == 0
    assert proof["compiling_tests"] == set()
    assert proof["unattributed_tests"] == set()


def test_a_ledger_row_of_the_old_width_is_ignored(tmp_path):
    # Five fields is the pre-attribution format; accepting it would let a
    # stale writer satisfy the new judge with no attribution at all.
    proof = libtest_diff.read_trigger_ledger(
        _ledger(tmp_path, [[11, "worker", "test_call", 500, 9]])
    )
    assert proof["rows"] == 0
    assert proof["workers"] == 0

# -- pinned direct-JIT coverage -------------------------------------------


def test_the_pinned_coverage_list_ignores_comments_and_blanks(tmp_path):
    path = tmp_path / "cov.txt"
    path.write_text("# why this list exists\n\ntest_call\n  test_int  \n")
    assert libtest_diff.load_execute_jit_coverage(path) == {
        "test_call",
        "test_int",
    }


def test_a_missing_coverage_list_pins_nothing(tmp_path):
    assert libtest_diff.load_execute_jit_coverage(tmp_path / "absent.txt") == set()


def test_the_gate_fails_when_a_pinned_module_stops_compiling():
    src = inspect.getsource(libtest_diff.cmd_execute_gate)
    # Per-module, not "at least one anywhere".
    assert "no longer do" in src
    assert "load_execute_jit_coverage" in src
    # And a module that newly compiles is reported rather than silently
    # widening the pin.
    assert "not in the pinned list" in src


def test_the_shipped_coverage_list_is_a_subset_of_the_target_modules():
    # A pinned module that is not even in the corpus could never be
    # satisfied, and would fail the gate for the wrong reason.
    pinned = libtest_diff.load_execute_jit_coverage(
        libtest_diff.EXECUTE_JIT_COVERAGE
    )
    assert pinned, "the shipped list must not be empty"
    assert pinned <= set(libtest_diff.load_stdlib72_modules())
