# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Self-tests for the trigger-proof drivers (ci_pipeline/jit311/runners.py).

The drivers' whole value is that they turn red when a claimed trigger did
not happen.  These tests prove the red paths by fabricating each blocking
condition from the development plan and asserting the driver fails:

  * expected trigger absent  -> failure, with the missing field named;
  * worker death             -> failure, never a skip;
  * missing report           -> failure, counted as a crash;
  * configuration not seen   -> failure raised inside the child;
  * schema drift             -> failure in validate_schema.

They need a CPython 3.11 interpreter with the cinderx wheel importable
(the gate runs them on the venv the wheel job builds).
"""

import json
import sys

import pytest

if sys.version_info[:2] != (3, 11):
    pytest.skip(
        "the 3.11 trigger-proof drivers run under CPython 3.11 only",
        allow_module_level=True,
    )

try:
    import _cinderx  # noqa: F401
except ImportError:
    pytest.skip(
        "cinderx is not importable; run under the gate venv",
        allow_module_level=True,
    )

from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from ci_pipeline.jit311 import report as jit_report
from ci_pipeline.jit311 import runners


def test_expected_trigger_missing_turns_red():
    # An extreme threshold keeps compile_requests at zero; a judge that
    # demands requests must therefore fail, naming the field.
    spec = runners.auto_like_runner(
        threshold=1_000_000,
        iters=50,
        judges=[runners.expect("compile_requests", ">", 0)],
    )
    result = runners.run(spec)
    assert not result.ok
    assert any("compile_requests" in err for err in result.errors)


def test_worker_death_is_a_failure_not_a_skip():
    spec = runners.RunnerSpec(
        name="worker_death",
        payload="import os\nos._exit(7)\n",
        judges=runners.gate_holds(),
    )
    result = runners.run(spec)
    assert not result.ok
    assert result.returncode == 7
    assert any("worker exited 7" in err for err in result.errors)
    assert any("no report" in err for err in result.errors)


def test_config_not_effective_turns_red():
    # The child asserts the variable itself; the harness deliberately does
    # not set it, modeling configuration that silently failed to apply.
    spec = runners.RunnerSpec(
        name="config_lost",
        payload="pass\n",
        env={},
        asserted_env={"CINDERX_JIT_MODE": "observe"},
        judges=[],
    )
    result = runners.run(spec)
    assert not result.ok
    assert any("worker exited" in err for err in result.errors)


def test_schema_drift_is_detected():
    snap = jit_report.snapshot()
    assert jit_report.validate_schema(snap) == []

    missing = dict(snap)
    del missing["machine_code_entries"]
    assert any(
        "missing field machine_code_entries" in err
        for err in jit_report.validate_schema(missing)
    )

    extra = dict(snap)
    extra["surprise"] = 1
    assert any(
        "unknown field surprise" in err
        for err in jit_report.validate_schema(extra)
    )

    wrong_type = dict(snap)
    wrong_type["compile_requests"] = "0"
    assert any(
        "compile_requests" in err
        for err in jit_report.validate_schema(wrong_type)
    )


def test_gate_defaults_pass_on_the_gated_build():
    # Positive control: the stage-default expectations hold on the
    # capability-gated build for a cold driver and an organic driver.
    for spec in (runners.cold_compile_runner(), runners.auto_like_runner()):
        result = runners.run(spec)
        assert result.ok, result.summary()


def test_unrelated_failure_cannot_impersonate_shadow_rejection():
    # Same exit code as the pinned rejection, wrong reason: an unrelated
    # RuntimeError must not pass the shadow pin (review probe
    # unrelated_exception_passes).
    spec = runners.RunnerSpec(
        name="unrelated_exception",
        payload="raise RuntimeError('unrelated')\n",
        judges=[],
        expect_returncode=1,
        expect_report=False,
        expect_stderr_contains=(
            "CINDERX_JIT_MODE=shadow is not accepted on CPython 3.11"
        ),
    )
    result = runners.run(spec)
    assert not result.ok
    assert any("rejection marker" in err for err in result.errors)


def test_missing_evaluator_turns_red(monkeypatch):
    # A preamble that silently fails to install the evaluator must fail the
    # gate judges (review probe evaluator_false_passes).
    broken = runners.CHILD_PREAMBLE.replace(
        "_cinderx.install_frame_evaluator()", "pass"
    )
    monkeypatch.setattr(runners, "CHILD_PREAMBLE", broken)
    result = runners.run(runners.cold_compile_runner())
    assert not result.ok
    assert any("evaluator_installed" in err for err in result.errors)


def test_child_schema_violation_turns_red():
    # A child that emits a corrupt report (and skips atexit) must be caught
    # by the harness-side schema validation, not judged field-by-field.
    spec = runners.RunnerSpec(
        name="corrupt_report",
        payload=(
            "import json, os\n"
            "with open(os.environ['JIT311_REPORT_PATH'], 'w') as fp:\n"
            "    json.dump({'bogus': 1}, fp)\n"
            "os._exit(0)\n"
        ),
        judges=[],
    )
    result = runners.run(spec)
    assert not result.ok
    assert any("report schema" in err for err in result.errors)


def test_corpus_module_missing_turns_red():
    # The completeness contract is asserted inside the child against the
    # manifest count; a shrunken corpus (modeled by demanding one module
    # more than exists) exits nonzero and turns red.
    spec = runners.corpus_completeness_runner(expected_modules=11)
    result = runners.run(spec)
    assert not result.ok
    assert any("worker exited" in err for err in result.errors)


def test_refcount_matrix_harness_runs_on_a_corpus_slice(tmp_path):
    # Consumer smoke for the migrated refcount-matrix harness: one small
    # corpus module in interp mode must measure targets and emit JSON.
    import json as _json
    import subprocess as _sp

    corpus_dir = Path(runners.REPO_ROOT) / "ci_pipeline" / "jit311"
    out = tmp_path / "rcm.json"
    proc = _sp.run(
        [
            sys.executable,
            str(corpus_dir / "refcount_matrix.py"),
            str(corpus_dir),
            "corpus_controlflow",
            "interp",
            str(out),
        ],
        capture_output=True,
        timeout=120,
    )
    assert proc.returncode == 0, proc.stderr.decode(errors="replace")[-400:]
    data = _json.loads(out.read_text())
    assert data, "refcount matrix measured no targets"

    # Interpreted execution may not impersonate JIT evidence: on the
    # capability-gated build, jit mode must fail loudly, never fall back.
    proc = _sp.run(
        [
            sys.executable,
            str(corpus_dir / "refcount_matrix.py"),
            str(corpus_dir),
            "corpus_controlflow",
            "jit",
            str(out),
        ],
        capture_output=True,
        timeout=120,
    )
    assert proc.returncode == 2, (proc.returncode, proc.stderr[-300:])
    assert b"refusing to fall back" in proc.stderr or b"force_compile refused" in proc.stderr

    # diff mode compares drift AND semantic outcome, and refuses a report
    # whose outcome table is missing -- otherwise it silently degrades to
    # the drift-only check it used to be.
    rcm = str(corpus_dir / "refcount_matrix.py")
    a = tmp_path / "a.json"; b = tmp_path / "b.json"

    def diff_rc(doc_a, doc_b):
        a.write_text(_json.dumps(doc_a))
        b.write_text(_json.dumps(doc_b))
        return _sp.run([sys.executable, rcm, "diff", str(a), str(b)]).returncode

    equal = {"drift": {"case_x": {"o": 1}}, "outcome": {"case_x": "ok:1234"}}
    assert diff_rc(equal, equal) == 0
    # Drift differs.
    assert diff_rc(equal, {"drift": {"case_x": {"o": 2}},
                           "outcome": {"case_x": "ok:1234"}}) == 1
    # Drift agrees but the answer does not: a refcount-neutral lowering that
    # returns the wrong result must still fail.
    assert diff_rc(equal, {"drift": {"case_x": {"o": 1}},
                           "outcome": {"case_x": "ok:9999"}}) == 1
    # No outcome table at all.
    assert diff_rc(equal, {"drift": {"case_x": {"o": 1}}}) == 1
    # An executing jit report that recorded a case with no machine-code
    # entries is not execution evidence.
    assert diff_rc(equal, {"drift": {"case_x": {"o": 1}},
                           "outcome": {"case_x": "ok:1234"},
                           "mode": "jit", "executing": True,
                           "machine_code_entries": {"case_x": 0}}) == 1


GATE_REQUIRED_JOBS = {
    "vendored_manifest", "wheel_build_import", "diff_engine_selftest",
    "bytecode_support_gate", "dynsym_allowlist", "interpreter_and_eval_hook",
    "trigger_stats_gate", "jit311_runner_selftests",
    "runtime_tests_311_green", "libtest_jitoff_diff", "unified_report_gate",
    "observe_gate", "shadow_compile_gate", "release_canary_execute",
}
DAILY_REQUIRED_JOBS = {
    "asan_build_311", "debug_build_311", "runtime_tests_311_census",
    "jit311_drivers", "jit311_pyperf_completeness", "jit311_pyperf_canary",
    "jit311_shadow_surface",
}


def _suite_jobs(path):
    import re as _re

    return set(_re.findall(r'name = "([a-z0-9_]+)"', Path(path).read_text()))


def test_daily_pyperf_job_runs_the_full_applicable_set():
    # Review P1: daily must not shrink to the 33-name --pyperf tranche.
    # `--pyperf` is a prefix of `--pyperformance-all`; match argv tokens.
    import re as _re

    daily = (
        Path(runners.REPO_ROOT) / "ci_pipeline" / "suites" / "cp311_daily.toml"
    ).read_text()
    assert "pyperformance==1.13.0" in daily
    assert _re.search(r"runners --pyperformance-all(?:['\"\s]|$)", daily)
    assert not _re.search(r"runners --pyperf(?:['\"\s]|$)", daily)


def test_pyperformance_all_flag_is_not_swallowed_by_pyperf(monkeypatch):
    # `--pyperf` is a prefix of `--pyperformance-all`. Dispatch must call
    # discover_all, never the 33-name manifest loader.
    seen = {}

    def fake_discover():
        seen["discover"] = True
        return ["nbody"]

    def fake_load():
        seen["load"] = True
        return ["nbody"]

    def fake_runner(*, benchmarks=None):
        seen["benchmarks"] = list(benchmarks or [])
        return None

    monkeypatch.setattr(runners, "discover_all_pyperf_benchmarks", fake_discover)
    monkeypatch.setattr(runners, "load_pyperf_benchmarks", fake_load)
    monkeypatch.setattr(
        runners, "pyperformance_completeness_runner", fake_runner
    )
    rc = runners.main(["--pyperformance-all"])
    assert rc == 2
    assert seen.get("discover") is True
    assert "load" not in seen
    assert seen.get("benchmarks") == ["nbody"]


def test_suites_carry_the_required_jobs():
    # The trigger-proof arms exist only if the suites actually run them; a
    # merge accident that drops a job must fail here, not in review.
    root = Path(runners.REPO_ROOT) / "ci_pipeline" / "suites"
    assert GATE_REQUIRED_JOBS <= _suite_jobs(root / "cp311_gate.toml")
    assert DAILY_REQUIRED_JOBS <= _suite_jobs(root / "cp311_daily.toml")
    assert "rt314_differential" in _suite_jobs(root / "cp314_reference.toml")


def test_shadow_compile_succeeds_without_install():
    # MR-03: shadow mode is accepted, compiles, and discards the artifact.
    result = runners.run(runners.shadow_compile_runner())
    assert result.ok, result.summary()
    assert result.returncode == 0
    assert result.report is not None
    assert result.report["compiled_function_creations"] == 0
    assert result.report["machine_code_entries"] == 0


def _load_run_gate():
    import importlib.util

    path = Path(runners.REPO_ROOT) / "ci_pipeline" / "run_gate.py"
    spec = importlib.util.spec_from_file_location("_run_gate_under_test", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_all_suite_commands_survive_formatting(tmp_path):
    # Job commands pass through str.format() before any shell sees them:
    # an unescaped ${VAR} dies as KeyError at dispatch, which means the job
    # can never start.  Every command in every suite must survive the real
    # formatting path.
    import tomllib

    rg = _load_run_gate()
    suites = sorted(
        (Path(runners.REPO_ROOT) / "ci_pipeline" / "suites").glob("*.toml")
    )
    assert suites
    for suite in suites:
        for job in tomllib.loads(suite.read_text()).get("jobs", []):
            if str(job.get("kind", "command")) != "command":
                continue
            command = rg.command_for_job(job, tmp_path, "", {})
            assert command.strip(), (suite.name, job.get("name"))


def _write_fake_jit(dir_, *, refuse):
    (dir_ / "cinderx.py").write_text("def init():\n    pass\n")
    body = (
        "_compiled = set()\n"
        "def force_compile(fn):\n"
        "    if %s or id(fn) in _compiled:\n"
        "        return False\n"
        "    _compiled.add(id(fn))\n"
        "    return True\n"
        "def is_jit_compiled(fn):\n"
        "    return id(fn) in _compiled\n"
    ) % ("True" if refuse else "False")
    (dir_ / "cinderjit.py").write_text(body)


def _run_rcm_jit(fake_dir, tmp_path):
    import os as _os
    import subprocess as _sp

    corpus_dir = Path(runners.REPO_ROOT) / "ci_pipeline" / "jit311"
    env = dict(_os.environ)
    env["PYTHONPATH"] = str(fake_dir) + _os.pathsep + env.get("PYTHONPATH", "")
    return _sp.run(
        [
            sys.executable,
            str(corpus_dir / "refcount_matrix.py"),
            str(corpus_dir),
            "corpus_generators",
            "jit",
            str(tmp_path / "out.json"),
        ],
        capture_output=True,
        env=env,
        timeout=180,
    )


def test_rcm_tolerates_already_compiled_shared_helpers(tmp_path):
    # Real force_compile() returns False for an already-compiled function,
    # and corpus helpers are shared across cases (gen_acc appears twice in
    # corpus_generators).  A fake jit with exactly那 once-semantics must
    # pass: the truth condition is is_jit_compiled(), not the return value.
    fake = tmp_path / "fake"
    fake.mkdir()
    _write_fake_jit(fake, refuse=False)
    proc = _run_rcm_jit(fake, tmp_path)
    assert proc.returncode == 0, proc.stderr.decode(errors="replace")[-400:]


def test_rcm_still_refuses_when_nothing_compiles(tmp_path):
    fake = tmp_path / "fake"
    fake.mkdir()
    _write_fake_jit(fake, refuse=True)
    proc = _run_rcm_jit(fake, tmp_path)
    assert proc.returncode == 2, (proc.returncode, proc.stderr[-300:])
    assert b"force_compile refused" in proc.stderr


def test_pyperf_benchmark_manifest_is_wellformed():
    import re as _re

    manifest = runners.load_pyperf_benchmarks()
    assert manifest
    for task, results in manifest.items():
        assert _re.fullmatch(r"[a-z0-9_]+", task)
        assert results
        assert all(_re.fullmatch(r"[a-z0-9_]+", r) for r in results)
    try:
        import pyperformance  # noqa: F401
    except ImportError:
        return  # name validation against the tool runs on provisioned runners
    import subprocess as _sp

    # Only the TASK column exists in the tool's own list; result names are
    # the manifest's frozen expectation and have no external oracle.
    listed = _sp.run(
        [sys.executable, "-m", "pyperformance", "list"],
        capture_output=True, text=True, timeout=120,
    ).stdout
    available = set(_re.findall(r"^- (\S+)", listed, _re.M))
    missing = set(manifest) - available
    assert not missing, f"manifest tasks unknown to pyperformance: {missing}"


def test_pyperf_manifest_parser_rejects_malformed(monkeypatch, tmp_path):
    # The parser is fail-closed on every way the two-column format can rot:
    # a missing colon, a duplicated task, a result duplicated across tasks,
    # and a result that does not sit under its task (swapped columns).
    cases = {
        "no_colon": "chaos chaos\n",
        "duplicate_task": "chaos: chaos\nchaos: chaos_2\n",
        "duplicate_result": "pickle: pickle_dict\npickle_dict: pickle_dict\n",
        "foreign_result": "chaos: richards\n",
        "empty_results": "chaos:\n",
    }
    for name, content in cases.items():
        bad = tmp_path / f"{name}.txt"
        bad.write_text(content)
        monkeypatch.setattr(runners, "PYPERF_BENCHMARKS", bad)
        with pytest.raises(SystemExit):
            runners.load_pyperf_benchmarks()


def test_pyperf_completion_judge_turns_red():
    # The judge that caught nothing catches nothing forever: prove both
    # red directions on fabricated result sets before it judges a real run.
    ok = runners.pyperf_completion_errors({"scimark_sor"}, {"scimark_sor"})
    assert ok == []

    missing = runners.pyperf_completion_errors(
        {"scimark_sor", "scimark_fft"}, {"scimark_sor"}
    )
    assert len(missing) == 1
    assert "scimark_fft" in missing[0] and "not reported" in missing[0]

    unexpected = runners.pyperf_completion_errors(
        {"scimark_sor"}, {"scimark_sor", "scimark_fft2"}
    )
    assert len(unexpected) == 1
    assert "scimark_fft2" in unexpected[0]
    assert "outside the manifest" in unexpected[0]

    # A task name leaking into the reported set must be flagged, not
    # silently absorbed -- this is the historical defect shape.
    both = runners.pyperf_completion_errors(
        {"scimark_sor"}, {"scimark"}
    )
    assert len(both) == 2


def test_pyperformance_sitecustomize_loads_cinderx_from_host_site():
    # Nested pyperformance venvs do not install _cinderx.  The completeness
    # driver must pass the host-venv site-packages path through inherit so
    # worker sitecustomize does not ModuleNotFoundError onto pyperf stdout.
    assert "JIT311_CINDERX_SITE" in runners.PYPERFORMANCE_SITECUSTOMIZE
    spec = runners.pyperformance_completeness_runner(benchmarks=["nbody"])
    if spec is None:
        return
    assert "JIT311_CINDERX_SITE" in spec.payload
    assert "_cinderx.__file__" in spec.payload
    assert "k.startswith('PIP_')" in spec.payload


def test_pyperformance_canary_rejects_deopt_storms():
    spec = runners.pyperformance_completeness_runner(
        mode="canary", benchmarks=["nbody"]
    )
    if spec is None:
        return
    assert "_organic <= _entered" in spec.payload
    assert "_worker_organic_deopts < _worker_entries" in spec.payload
    errors = [
        error
        for judge in spec.judges
        for error in judge({"organic_deopt_hits": 1})
    ]
    assert any("organic_deopt_hits == 0" in error for error in errors)


def test_libtest_target_manifest_is_wellformed():
    sys.path.insert(0, str(Path(runners.REPO_ROOT) / "ci_pipeline"))
    import libtest_diff_311 as lt

    modules = lt.load_target_manifest()
    assert modules and len(modules) == len(set(modules))
    assert all(m.startswith("test") for m in modules)
    # The frozen surface is exactly the harvested 457-17=440; regenerating
    # it is a deliberate edit that updates this pin together.
    assert len(modules) == 440, len(modules)


def test_unified_report_strict_validation_rejects_null_fields():
    snap = jit_report.snapshot()
    errors = jit_report.validate_schema(snap, strict=True)
    assert any("target_modules_attempted" in err for err in errors)
    for field in jit_report.HARNESS_FIELDS:
        snap[field] = 0
    snap["target_modules_attempted"] = 440
    assert not jit_report.validate_schema(snap, strict=True)


def test_unify_produces_a_fully_typed_report(tmp_path):
    import json as _json

    fields = tmp_path / "fields.json"
    out = tmp_path / "unified.json"
    fields.write_text(
        '{"target_modules_attempted": 440, "worker_crashes": 0}\n'
    )
    rc = runners.main(["--unify", str(fields), "-o", str(out)])
    assert rc == 0
    data = _json.loads(out.read_text())
    assert data["target_modules_attempted"] == 440
    assert not jit_report.validate_schema(data, strict=True)

    # A null field from the leg must be refused, not laundered.
    fields.write_text(
        '{"target_modules_attempted": null, "worker_crashes": 0}\n'
    )
    assert runners.main(["--unify", str(fields), "-o", str(out)]) == 1

    # The leg owns worker_crashes: when its emission does not carry the
    # field, the aggregator must refuse rather than default-fill zero.
    fields.write_text('{"target_modules_attempted": 440}\n')
    assert runners.main(["--unify", str(fields), "-o", str(out)]) == 1

    # Typing is not the contract: the surface must be exactly the frozen
    # manifest and the crash count exactly zero.
    for bad in (
        '{"target_modules_attempted": 439, "worker_crashes": 0}',
        '{"target_modules_attempted": 441, "worker_crashes": 0}',
        '{"target_modules_attempted": 440, "worker_crashes": 7}',
        # A leg may not override the probe child's own at-exit sample.
        '{"target_modules_attempted": 440, "worker_crashes": 0,'
        ' "live_compiled_functions_at_exit": 5}',
    ):
        fields.write_text(bad + "\n")
        assert runners.main(["--unify", str(fields), "-o", str(out)]) == 1


def _rt_data(name):
    path = (
        Path(runners.REPO_ROOT) / "ci_pipeline" / "jit311" / "data" / name
    )
    return [
        line.strip()
        for line in path.read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]


def test_runtime_tests_manifests_are_consistent():
    registered = _rt_data("rt311_registered_tests.txt")
    known_fail = _rt_data("rt311_known_failures.txt")
    families = _rt_data("rt311_green_families.txt")
    assert registered and len(registered) == len(set(registered))
    # Every known failure is a registered test, every green family has a
    # registered population, and no green family appears in the known-fail
    # set -- the three data files describe one and the same binary.
    assert set(known_fail) <= set(registered)
    suites = {entry.split(".", 1)[0] for entry in registered}
    missing = [fam for fam in families if fam not in suites]
    assert not missing, missing
    failing_suites = {entry.split(".", 1)[0] for entry in known_fail}
    overlap = failing_suites & set(families)
    assert not overlap, overlap
    allowed = _rt_data("rt311_allowed_skips.txt")
    green_skips = [
        entry for entry in allowed if entry.split(".", 1)[0] in set(families)
    ]
    assert not green_skips, green_skips
    assert "HIRBuildTest" in families
    writeoff = {
        "LIRGeneratorTest.IsNegativeAndErrOccurredSetErrBranchesToDone",
        "LIRGeneratorTest.RaiseOnlyFunctionHasVerifierSafeSyntheticExit",
        "HIRBuildTest.TryLoopReturningHandler311BuildsHIR",
        "HIRBuildTest.RaiseOnlyFunction311BuildsHIR",
    }
    assert writeoff <= set(registered)
    assert not (writeoff & set(known_fail))


def _writeoff_rows(name):
    """Parse a write-off table into (item, status, [evidence])."""
    rows = []
    for line in _rt_data(name):
        parts = [field.strip() for field in line.split("\t")]
        assert len(parts) == 3, (name, line)
        item, status, evidence = parts
        assert status in ("landed", "not-applicable"), (name, line)
        cases = [case.strip() for case in evidence.split(";") if case.strip()]
        assert cases, (name, line)
        rows.append((item, status, cases))
    return rows


def _canary_population(tests_dir=None):
    """The canary population by the runner's own derivation.

    The run_rt311_green.sh gate scans the SKIP_311_EXECUTABLE_COMPILE()
    sites to decide what the canary leg executes; anything that claims a
    case "runs in canary" has to consume that same scan, not a committed
    manifest that merely permits a skip.
    """
    import subprocess as _sp

    script = (
        Path(runners.REPO_ROOT)
        / "ci_pipeline" / "scripts" / "run_rt311_green.sh"
    )
    cmd = ["bash", str(script), "--verify-canary-population"]
    if tests_dir is not None:
        cmd.append(str(tests_dir))
    proc = _sp.run(cmd, capture_output=True, text=True, timeout=60)
    assert proc.returncode == 0, (proc.returncode, proc.stdout[-400:])
    return {line.strip() for line in proc.stdout.splitlines() if line.strip()}


def _assert_writeoff_evidence_runs_in_canary(rows, population):
    for item, _, cases in rows:
        unrun = [case for case in cases if case not in population]
        assert not unrun, (item, unrun)


def test_mr05_lifecycle_writeoff_is_backed_by_passing_tests():
    # The audit rows this milestone owns are closed against registered
    # cases rather than against prose.  Reading the evidence out of the
    # table -- instead of restating it here -- is what stops the two from
    # drifting: a row can only be written off against a case that exists
    # and is not in the known-failure baseline.
    registered = set(_rt_data("rt311_registered_tests.txt"))
    known_fail = set(_rt_data("rt311_known_failures.txt"))
    rows = _writeoff_rows("mr05_lifecycle_writeoff.txt")
    items = [item for item, _, _ in rows]
    assert items == [
        "weakref death-watch replication",
        "co_extra minimum capacity",
        "finalize cross-period GC re-entry UAF family",
    ], items
    for item, status, cases in rows:
        assert status == "landed", (item, status)
        missing = [case for case in cases if case not in registered]
        assert not missing, (item, missing)
        failing = [case for case in cases if case in known_fail]
        assert not failing, (item, failing)
    # Every named case has to be one the canary leg actually runs, or the
    # write-off rests on something that is skipped in both modes.  The
    # population comes from the runner's macro scan -- the allowed-skip
    # manifest only proves the normal mode may skip, and a case whose gate
    # macro is removed or relocated keeps its manifest rows while dropping
    # out of the canary run.
    _assert_writeoff_evidence_runs_in_canary(rows, _canary_population())


def test_mr05_writeoff_case_leaving_the_canary_population_turns_red():
    # A write-off evidence case that falls out of the derived canary
    # population -- its mode-gate macro deleted, renamed, or moved -- must
    # turn the verifier red, whatever the committed manifests still say.
    rows = _writeoff_rows("mr05_lifecycle_writeoff.txt")
    population = _canary_population()
    victim = rows[0][2][0]
    assert victim in population
    with pytest.raises(AssertionError):
        _assert_writeoff_evidence_runs_in_canary(rows, population - {victim})


def test_canary_population_counts_only_gate_sites_inside_the_case(tmp_path):
    # The scan attributes a SKIP_311_EXECUTABLE_COMPILE() line to the
    # TEST_F body it appears in.  A macro moved into a helper still skips
    # at runtime, but it is no longer a gate site the scan can attribute --
    # such a case leaves the canary population, and with it any write-off
    # that named it (the previous test pins that side).
    src = tmp_path / "doctored_test.cpp"
    src.write_text(
        "TEST_F(DoctoredTest, GatedInBody) {\n"
        "  SKIP_311_EXECUTABLE_COMPILE();\n"
        "  helperGate();\n"
        "}\n"
        "static void helperGate() {\n"
        "  SKIP_311_EXECUTABLE_COMPILE();\n"
        "}\n"
        "TEST_F(DoctoredTest, GatedViaHelper) {\n"
        "  helperGate();\n"
        "}\n"
    )
    population = _canary_population(tmp_path)
    assert "DoctoredTest.GatedInBody" in population
    assert "DoctoredTest.GatedViaHelper" not in population


def test_registered_test_disappearance_turns_red(tmp_path):
    import subprocess as _sp

    script = (
        Path(runners.REPO_ROOT)
        / "ci_pipeline" / "scripts" / "run_rt311_green.sh"
    )
    manifest = (
        Path(runners.REPO_ROOT)
        / "ci_pipeline" / "jit311" / "data" / "rt311_registered_tests.txt"
    )
    entries = [
        line for line in manifest.read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    live = tmp_path / "live.txt"

    # Identical live list passes through the same code path the gate uses.
    live.write_text("\n".join(entries) + "\n")
    proc = _sp.run(
        ["bash", str(script), "--verify-registered", str(live)],
        capture_output=True, text=True, timeout=60,
    )
    assert proc.returncode == 0, proc.stdout[-400:]

    # One deleted green case must turn red, not stay silently green.
    live.write_text("\n".join(entries[1:]) + "\n")
    proc = _sp.run(
        ["bash", str(script), "--verify-registered", str(live)],
        capture_output=True, text=True, timeout=60,
    )
    assert proc.returncode == 1
    assert "identity drifted" in proc.stdout


def test_baseline_growth_turns_red(tmp_path):
    import subprocess as _sp

    script = (
        Path(runners.REPO_ROOT)
        / "ci_pipeline" / "scripts" / "run_rt311_green.sh"
    )
    old = tmp_path / "old.txt"
    new = tmp_path / "new.txt"
    old.write_text("Suite.A\nSuite.B\n")

    # Shrinking (a fix) passes; growing in the same change turns red.
    new.write_text("Suite.A\n")
    assert _sp.run(
        ["bash", str(script), "--verify-baseline-growth", str(old), str(new)],
        capture_output=True, timeout=60,
    ).returncode == 0
    new.write_text("Suite.A\nSuite.B\nSuite.C\n")
    proc = _sp.run(
        ["bash", str(script), "--verify-baseline-growth", str(old), str(new)],
        capture_output=True, text=True, timeout=60,
    )
    assert proc.returncode == 1
    assert "washed green" in proc.stdout


def test_skip_allowlist_growth_turns_red(tmp_path):
    import subprocess as _sp

    script = (
        Path(runners.REPO_ROOT)
        / "ci_pipeline" / "scripts" / "run_rt311_green.sh"
    )
    old = tmp_path / "old.txt"
    new = tmp_path / "new.txt"
    old.write_text("Suite.SkipA\n")
    new.write_text("Suite.SkipA\nSuite.SkipB\n")
    proc = _sp.run(
        ["bash", str(script), "--verify-skip-growth", str(old), str(new)],
        capture_output=True, text=True, timeout=60,
    )
    assert proc.returncode == 1
    assert "washed green" in proc.stdout
    new.write_text("Suite.SkipA\n")
    assert _sp.run(
        ["bash", str(script), "--verify-skip-growth", str(old), str(new)],
        capture_output=True, timeout=60,
    ).returncode == 0


def test_registered_verification_is_locale_proof(tmp_path):
    import os as _os
    import subprocess as _sp

    script = (
        Path(runners.REPO_ROOT)
        / "ci_pipeline" / "scripts" / "run_rt311_green.sh"
    )
    manifest = (
        Path(runners.REPO_ROOT)
        / "ci_pipeline" / "jit311" / "data" / "rt311_registered_tests.txt"
    )
    entries = [
        line for line in manifest.read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    live = tmp_path / "live.txt"
    live.write_text("\n".join(entries) + "\n")
    for locale_value in ("C", "en_US.UTF-8"):
        env = dict(_os.environ)
        env["LC_ALL"] = locale_value
        proc = _sp.run(
            ["bash", str(script), "--verify-registered", str(live)],
            capture_output=True, text=True, env=env, timeout=60,
        )
        assert proc.returncode == 0, (locale_value, proc.stdout[-400:])


def test_child_env_drops_external_jit_config(monkeypatch):
    # Inherited CinderX / PYTHONJIT configuration must never reach the
    # child: a leaked shadow-mode variable would change what every driver
    # measures (and dies loudly on the gated build, which is exactly how
    # a leak shows up here).
    monkeypatch.setenv("CINDERX_JIT_MODE", "shadow")
    monkeypatch.setenv("PYTHONJITAUTO", "4")
    result = runners.run(runners.cold_compile_runner())
    assert result.ok, result.errors


def test_expected_deopt_missing_turns_red():
    # The deopt counters are wired into the schema now; a stage that
    # promises deopt evidence and does not produce it must fail, never
    # skip -- same discipline as the compile triggers.
    spec = runners.cold_compile_runner(
        judges=runners.gate_holds()
        + [runners.expect("forced_deopt_hits", ">", 0)]
    )
    result = runners.run(spec)
    assert not result.ok
    assert any("forced_deopt_hits" in err for err in result.errors)


def test_unexpected_organic_deopt_turns_red():
    errors = [
        error
        for judge in runners.execute_holds()
        for error in judge({"organic_deopt_hits": 1})
    ]
    assert any("organic_deopt_hits == 0" in error for error in errors)


def test_stdlib_organic_deopt_count_drift_turns_red():
    # Each milestone re-pins the leg (MR-09's guarded attribute sites put
    # it at 333); any drift off the pinned constant must still turn red.
    errors = [
        error
        for judge in runners.stdlib_canary_runner().judges
        for error in judge({"organic_deopt_hits": 334})
    ]
    assert any("organic_deopt_hits == 333" in error for error in errors)


def test_green_gate_refuses_skips(tmp_path):
    import subprocess as _sp

    script = (
        Path(runners.REPO_ROOT)
        / "ci_pipeline" / "scripts" / "run_rt311_green.sh"
    )
    log = tmp_path / "green.log"
    # The reviewer probe: one green case downgraded to GTEST_SKIP keeps
    # exit 0 and the ran-count -- the PASSED count and the zero-skip rule
    # must catch it.
    log.write_text(
        "[==========] 134 tests from 26 test suites ran. (44 ms total)\n"
        "[  PASSED  ] 133 tests.\n"
        "[  SKIPPED ] 1 test, listed below:\n"
        "[  SKIPPED ] HIRBuilderTest.Foo\n"
    )
    proc = _sp.run(
        ["bash", str(script), "--verify-green-log", str(log), "134"],
        capture_output=True, text=True, timeout=60,
    )
    assert proc.returncode == 1
    assert "neither a pass nor a fix" in proc.stdout

    log.write_text(
        "[==========] 134 tests from 26 test suites ran. (44 ms total)\n"
        "[  PASSED  ] 134 tests.\n"
    )
    assert _sp.run(
        ["bash", str(script), "--verify-green-log", str(log), "134"],
        capture_output=True, timeout=60,
    ).returncode == 0


def _rt314_fixture_logs(tmp_path):
    base = tmp_path / "base.log"
    base.write_text(
        "[ RUN      ] CmdLineTest.OSREnabledFlag\n"
        "file.cpp:42: Failure at 0x1a2b in /home/u/src/x.cpp\n"
        "[  FAILED  ] CmdLineTest.OSREnabledFlag (12 ms)\n"
        "[ RUN      ] FooTest.Ok\n"
        "[       OK ] FooTest.Ok (1 ms)\n"
        "[==========] 2 tests from 2 test suites ran. (20 ms total)\n"
        "[  FAILED  ] 1 test, listed below:\n"
        "[  FAILED  ] CmdLineTest.OSREnabledFlag\n"
    )
    return base


def test_rt314_log_stage_normalizes_and_compares(tmp_path):
    import subprocess as _sp

    script = (
        Path(runners.REPO_ROOT)
        / "ci_pipeline" / "scripts" / "rt314_differential.sh"
    )
    base = _rt314_fixture_logs(tmp_path)

    # Same behavior, different address/path/duration: normalized equal.
    same = tmp_path / "same.log"
    same.write_text(
        base.read_text()
        .replace("0x1a2b", "0x9f8e")
        .replace("/home/u/src", "/other/place")
        .replace("(12 ms)", "(99 ms)")
    )
    assert _sp.run(
        ["bash", str(script), "--verify-logs", str(base), str(same)],
        capture_output=True, timeout=60,
    ).returncode == 0

    # A pure source-line shift (unrelated edits move the assertion) is
    # NOT a behavior change and must not red the differential.
    lineshift = tmp_path / "lineshift.log"
    lineshift.write_text(base.read_text().replace("file.cpp:42", "file.cpp:52"))
    assert _sp.run(
        ["bash", str(script), "--verify-logs", str(base), str(lineshift)],
        capture_output=True, timeout=60,
    ).returncode == 0

    # gtest AssertionHelper prints `file", LINE` rather than `file:LINE`.
    gtest_base = tmp_path / "gtest-base.log"
    gtest_base.write_text(
        "[ RUN      ] CmdLineTest.OSREnabledFlag\n"
        'cmdline_test.cpp", 698, GetBoolAssertionFailureMessage('
        'gtest_ar_, "getConfig().osr_enabled", "false", "true")\n'
        "[  FAILED  ] CmdLineTest.OSREnabledFlag (12 ms)\n"
        "[==========] 1 test from 1 test suite ran. (12 ms total)\n"
        "[  FAILED  ] 1 test, listed below:\n"
        "[  FAILED  ] CmdLineTest.OSREnabledFlag\n"
    )
    gtest_shift = tmp_path / "gtest-shift.log"
    gtest_shift.write_text(
        gtest_base.read_text().replace('cpp", 698', 'cpp", 708')
    )
    assert _sp.run(
        ["bash", str(script), "--verify-logs", str(gtest_base), str(gtest_shift)],
        capture_output=True, timeout=60,
    ).returncode == 0

    # An allowlisted failure failing DIFFERENTLY is not excusable.
    diag = tmp_path / "diag.log"
    diag.write_text(
        base.read_text().replace(
            "file.cpp:42: Failure", "other.cpp:9: DIFFERENT assertion"
        )
    )
    proc = _sp.run(
        ["bash", str(script), "--verify-logs", str(base), str(diag)],
        capture_output=True, text=True, timeout=60,
    )
    assert proc.returncode == 1 and "diverged" in proc.stdout

    # base pass -> head skip keeps failure sets equal; the skip-set
    # comparison must refuse it.
    skip = tmp_path / "skip.log"
    skip.write_text(
        base.read_text().replace(
            "[       OK ] FooTest.Ok (1 ms)",
            "[  SKIPPED ] FooTest.Ok (0 ms)",
        )
        + "[  SKIPPED ] 1 test, listed below:\n[  SKIPPED ] FooTest.Ok\n"
    )
    proc = _sp.run(
        ["bash", str(script), "--verify-logs", str(base), str(skip)],
        capture_output=True, text=True, timeout=60,
    )
    assert proc.returncode == 1 and "skipped-test sets diverged" in proc.stdout


def test_rt314_allowlist_growth_must_be_symmetric_failures(tmp_path):
    import subprocess as _sp

    script = (
        Path(runners.REPO_ROOT)
        / "ci_pipeline" / "scripts" / "rt314_differential.sh"
    )
    grown = tmp_path / "grown.txt"
    symmetric = tmp_path / "symmetric.txt"
    grown.write_text(
        "OSRDetectionTest.HotWhileLoopAtThresholdCallsTryOSRAndContinues\n"
    )
    symmetric.write_text(
        "CmdLineTest.OSREnabledFlag\n"
        "OSRDetectionTest.HotWhileLoopAtThresholdCallsTryOSRAndContinues\n"
    )
    assert _sp.run(
        ["bash", str(script), "--verify-allowlist-growth",
         str(grown), str(symmetric)],
        capture_output=True, timeout=60,
    ).returncode == 0
    grown.write_text("NotARealTest.WashesGreen\n")
    proc = _sp.run(
        ["bash", str(script), "--verify-allowlist-growth",
         str(grown), str(symmetric)],
        capture_output=True, text=True, timeout=60,
    )
    assert proc.returncode == 1
    assert "did not fail identically" in proc.stdout


def test_refcount_matrix_canary_minimal_tier(tmp_path):
    # MR-04 minimal tier: on the execute-min corpus the jit mode genuinely
    # compiles (canary), every case executes machine code, and the
    # per-case refcount drift equals the interpreted run exactly.
    import os as _os
    import subprocess as _sp

    corpus_dir = Path(runners.REPO_ROOT) / "ci_pipeline" / "jit311"
    rcm = str(corpus_dir / "refcount_matrix.py")
    interp_out = tmp_path / "interp.json"
    jit_out = tmp_path / "jit.json"

    proc = _sp.run(
        [sys.executable, rcm, str(corpus_dir), "corpus_execute_min",
         "interp", str(interp_out)],
        capture_output=True, timeout=180,
    )
    assert proc.returncode == 0, proc.stderr.decode(errors="replace")[-400:]

    env = dict(_os.environ)
    env["CINDERX_JIT_MODE"] = "canary"
    # No call threshold: the cases are force-compiled, and a threshold would
    # arm the ROI deopt backoff, which withdraws the raising cases partway
    # through the iteration loop and leaves the rest of their run
    # interpreted.
    env.pop("PYTHONJITAUTO", None)
    # The plan makes the debug allocator mandatory from MR-04, and this tier
    # is one of the places it has to hold.
    env["PYTHONMALLOC"] = "debug"
    proc = _sp.run(
        [sys.executable, rcm, str(corpus_dir), "corpus_execute_min",
         "jit", str(jit_out)],
        capture_output=True, env=env, timeout=180,
    )
    assert proc.returncode == 0, proc.stderr.decode(errors="replace")[-400:]

    proc = _sp.run(
        [sys.executable, rcm, "diff", str(interp_out), str(jit_out)],
        capture_output=True, text=True, timeout=60,
    )
    assert proc.returncode == 0, proc.stdout[-400:]

    # Every case must have run compiled for the whole loop, and the report
    # must carry the semantic outcomes the diff compares.
    report = json.loads(jit_out.read_text())
    assert set(report["outcome"]) == set(report["drift"]), report
    entries = report["machine_code_entries"]
    assert entries and all(v >= 200 for v in entries.values()), entries
