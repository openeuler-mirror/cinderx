"""A3-Core: the long-term formal acceptance profile (simplified plan v1.1).

Three formal cases plus one conditional hook, assembled from the discovery
framework's existing probes:

    LIFECYCLE  L1 function death x100 (C1 core scale)
               L2 __code__ swap x100 (C5 core scale)
               L3 observer/code smoke x1000 (C2 core scale)
               L4 suspended-generator ownership anchor (the canonical
                  Python regression)
               L5 ownership invariants at every checkpoint (inside the
                  churn probes)
    SHUTDOWN   S1 park/die/re-enable x100 (C4 core scale)
               S2 multithread compile/teardown (C8 core scale)
               S3 six-state real-process exit matrix, poisoned + layout
                  entropy (a3_shutdown core quota)
    MEMSAFE    seven targeted ASAN legs against an instrumented extension
    REALWORLD-PENETRATION-HOOK
               the A2 threshold=50 arm over the 72 frozen stdlib modules,
               required mechanically when lifecycle-sensitive paths change
               and always for a final acceptance

Two policies the plan pins down are implemented here rather than in the
probes: capacity/high-water readings are demoted from verdict to
RESOURCE_STABILITY_DIAGNOSTIC (v1.1 §27), and every lifecycle scenario
must prove it was not vacuous -- the populations rose before they
returned (v1.1 §10).
"""

from __future__ import annotations

import argparse
from datetime import datetime
import fnmatch
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import textwrap

from ci_pipeline.jit311.a3_census import CAPACITY_PATHS, value_at
from ci_pipeline.jit311.a3_runner import A3Runner


CASES = ("LIFECYCLE", "SHUTDOWN", "MEMSAFE")

SHUTDOWN_CORE_QUOTA = (
    "installed=20,parked=20,function-death=20,code-death=20,"
    "failure-unwind=20,multithread-completed=100"
)

# v1.1 §10: which populations must provably rise (live gauge at the
# live_1 checkpoint) and which cumulative counters must provably move for
# the scenario's return-to-baseline to mean anything.
NONVACUITY = {
    "C1": {
        "live": ("jit.installed_functions", "jit.watched_functions"),
        "cumulative": {"runtime.function_destroyed_notifications": "cycles"},
    },
    "C5": {
        "live": (),
        "cumulative": {"runtime.compiled_function_creations": 2},
    },
    "C2": {
        "live": ("observer.watched_codes",),
        "cumulative": {"runtime.code_destroyed_notifications": "cycles"},
    },
    "C4": {
        "live": (),
        "cumulative": {"runtime.function_destroyed_notifications": "cycles"},
    },
    "C8": {
        "live": (),
        "cumulative": {"runtime.compiled_function_creations": "cycles"},
    },
}

GENERATOR_ANCHOR_TEST = (
    "test_cinderx.test_kunpeng.test_canary_execute_311."
    "CanaryExecute311Test.test_suspended_generator_pins_its_artifact"
)

# The suspended-generator pin probe, shared by the LIFECYCLE anchor's ASAN
# twin (MEMSAFE target 3).  Same contract as the canonical Python test,
# runnable directly under an instrumented extension.
GENERATOR_PIN_PROBE = textwrap.dedent(
    """
    import gc
    import _cinderx, cinderx
    cinderx.init()
    _cinderx.install_frame_evaluator()
    import cinderjit

    namespace = {}
    exec(
        "def wave(base):\\n    yield base\\n    yield base + 1\\n",
        namespace,
        namespace,
    )
    wave = namespace.pop("wave")
    assert cinderjit.force_compile(wave) is True
    generator = wave(10)
    assert next(generator) == 10
    held = _cinderx._get_trigger_stats()["resident_code_buffers"]
    assert cinderjit.force_uncompile(wave) is True
    del wave, namespace
    gc.collect(); gc.collect()
    assert _cinderx._get_trigger_stats()["resident_code_buffers"] == held
    assert generator.send(None) == 11
    try:
        next(generator)
    except StopIteration:
        pass
    else:
        raise AssertionError("generator did not finish")
    del generator
    gc.collect(); gc.collect()
    assert _cinderx._get_trigger_stats()["resident_code_buffers"] == held - 1
    assert cinderjit._jit311_lifecycle_invariants()["ok"] is True
    print("generator-pin-probe: ok")
    """
)


def judge_penetration_path(classification: dict | None) -> list[str]:
    """The hook's JIT path proof (review P0-1).

    Semantic results alone can go green with the JIT silently absent, so
    the hook consumes the penetration classifier's worker evidence: every
    worker journal present, the JIT active in each, the scheduler at the
    configured threshold, no dropped ledger/scheduler evidence, no unknown
    refusals, and real own-code machine entries somewhere in the run.
    """
    if not classification:
        return ["penetration classifier produced no output"]
    errors = []
    totals = classification.get("totals") or {}
    targets = classification.get("target_modules")
    if targets != 72:
        errors.append(f"target module population is {targets}, not 72")
    for note in classification.get("errors") or []:
        if "missing worker summaries" in note:
            errors.append(note)
    active = totals.get("worker_jit_active")
    if active != 72:
        errors.append(f"JIT active in {active}/72 workers")
    thresholds = {
        row.get("scheduler_threshold")
        for row in (classification.get("modules") or {}).values()
        if isinstance(row, dict) and not row.get("missing")
    }
    if thresholds != {50}:
        errors.append(
            f"scheduler threshold not uniformly 50: {sorted(map(str, thresholds))}"
        )
    if totals.get("ledger_dropped", -1) != 0:
        errors.append(f"entry ledger dropped {totals.get('ledger_dropped')}")
    if totals.get("events_dropped", -1) != 0:
        errors.append(f"scheduler events dropped {totals.get('events_dropped')}")
    if classification.get("unknown_refusals"):
        errors.append(
            f"unknown refusals: {classification.get('unknown_refusals')[:5]}"
        )
    if totals.get("machine_entries", 0) <= 0:
        errors.append("no machine-code entries in any worker")
    if totals.get("actual_own_code_machine_entry_modules", 0) <= 0:
        errors.append("no module entered its own compiled code")
    return errors


def judge_semantic_results(statuses: dict, *, expected: int) -> list[str]:
    # A run that silently lost module results must not be judged on the
    # survivors: PASS needs every target present AND passing.
    errors = []
    if len(statuses) != expected:
        errors.append(
            f"penetration arm returned {len(statuses)}/{expected} module results"
        )
    non_pass = {name: state for name, state in statuses.items() if state != "pass"}
    if non_pass:
        errors.append(f"penetration arm non-pass modules: {non_pass}")
    return errors


def final_status(
    *,
    selected: set[str],
    cases: dict,
    hook_required: bool,
    hook_skipped: bool,
    hook_run: dict | None,
    infra: list[str],
) -> str:
    """The formal verdict (review P0-2).

    A subset invocation can succeed, but only the complete surface --
    all three cases plus the hook whenever it is required -- may call
    itself an A3-Core PASS; anything less green caps at NOT_FULLY_RUN.
    An operator skip of the hook is a subset by definition.
    """
    if infra or any(
        case.get("result") == "INFRA_FAIL" for case in cases.values()
    ):
        return "INFRA_FAIL"
    if any(case.get("result") != "PASS" for case in cases.values()):
        return "FAIL"
    if hook_run is not None and hook_run.get("result") != "PASS":
        return "FAIL"
    fully_run = (
        selected == set(CASES)
        and not hook_skipped
        and (hook_run is not None or not hook_required)
    )
    return "PASS" if fully_run else "NOT_FULLY_RUN"


def match_sensitive(changed: list[str], patterns: list[str]) -> list[str]:
    hits = []
    for path in changed:
        for pattern in patterns:
            if pattern.endswith("/**"):
                if path.startswith(pattern[:-2]):
                    hits.append(path)
                    break
            elif fnmatch.fnmatch(path, pattern):
                hits.append(path)
                break
    return hits


def apply_core_policy(result: dict) -> dict:
    """Demote capacity errors to diagnostics and add non-vacuity checks."""
    scenario = result.get("scenario")
    errors = []
    diagnostics = []
    for error in result.get("errors", []):
        if error.startswith("capacity "):
            diagnostics.append(error)
        else:
            errors.append(error)
    capacity_readings = {}
    plateau = result.get("plateau") or {}
    for path, reading in (plateau.get("capacity") or {}).items():
        capacity_readings[path] = reading

    contract = NONVACUITY.get(scenario, {})
    samples = result.get("samples") or []
    by_label = {sample["label"]: sample for sample in samples}
    baseline = by_label.get("baseline")
    if scenario == "C4":
        # Review P1-1: the recorded phase populations must show the park,
        # the partial death and the re-attach -- relative transitions, not
        # absolute set assertions.
        rows = (result.get("evidence") or {}).get("phase_evidence") or []

        def phases_ok(row: dict) -> bool:
            # The population must be visible while disabled -- under either
            # pause model: a flag-level pause keeps it installed, a
            # per-function park moves it to parked.
            populated = (
                row.get("disabled_installed", 0) + row.get("disabled_parked", 0)
                >= 4
            )
            # The partial death must have moved whichever population held
            # the subjects.
            death_moved = row.get("enabled_installed", 0) < row.get(
                "disabled_installed", 0
            ) or row.get("half_dead_parked", 0) < row.get("disabled_parked", 0)
            # And the survivors must still be installed after enable();
            # their execution is separately proven by the scenario's
            # semantic checks and machine-entry delta.
            survivors = row.get("enabled_installed", 0) > 0
            return populated and death_moved and survivors

        if not rows or not all(phases_ok(row) for row in rows):
            errors.append(
                "non-vacuity: C4 phase evidence lacks the park/death/"
                "re-attach transitions"
            )
    if contract and baseline is None:
        errors.append("non-vacuity: baseline sample is missing")
    if contract and baseline is not None:
        live = by_label.get("live_1")
        for path in contract.get("live", ()):
            if live is None:
                errors.append("non-vacuity: live_1 sample is missing")
                break
            if value_at(live["snapshot"], path) <= value_at(
                baseline["snapshot"], path
            ):
                errors.append(
                    f"non-vacuity: {path} never rose above baseline"
                )
        final = by_label.get("after_gc_2")
        cycles = (result.get("cycles") or [0])[-1]
        for path, minimum in contract.get("cumulative", {}).items():
            floor = cycles if minimum == "cycles" else int(minimum)
            if final is None:
                errors.append("non-vacuity: after_gc_2 sample is missing")
                break
            moved = value_at(final["snapshot"], path) - value_at(
                baseline["snapshot"], path
            )
            if moved < floor:
                errors.append(
                    f"non-vacuity: {path} moved {moved} (< {floor})"
                )

    result = dict(result)
    result["errors"] = errors
    result["resource_stability_diagnostic"] = {
        "errors_demoted": diagnostics,
        "capacity_readings": capacity_readings,
    }
    if result.get("result") != "INFRA_FAIL":
        result["result"] = "PASS" if not errors else "FAIL"
    return result


class A3CoreRunner(A3Runner):
    def __init__(
        self,
        *,
        asan_build: Path | None,
        penetration_hook: str,
        hook_base: str | None,
        cases: set[str],
        **kwargs,
    ) -> None:
        super().__init__(lanes=set(), **kwargs)
        self.asan_build = asan_build.resolve() if asan_build else None
        self.penetration_hook = penetration_hook
        self.hook_base = hook_base
        self.cases = cases

    # -- probes ----------------------------------------------------------

    def _churn(self, name: str, scenario: str, directory: Path, *, scale: str, env=None) -> dict:
        out = directory / f"{scenario}.json"
        rc = self.base._run(
            name,
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.a3_churn",
                "--scenario",
                scenario,
                "--scale",
                scale,
                "--out",
                str(out),
            ],
            env=env or self._c_env(scenario),
        )
        result = self._json(out)
        if result is None:
            result = {
                "scenario": scenario,
                "result": "FAIL" if rc == 124 or rc < 0 else "INFRA_FAIL",
                "errors": [f"scenario process exited {rc} without JSON evidence"],
            }
        return apply_core_policy(result)

    def run_lifecycle(self) -> dict:
        directory = self.output / "LIFECYCLE"
        directory.mkdir()
        results = {
            "L1_function_death": self._churn("50-CORE-L1", "C1", directory, scale="core"),
            "L2_code_swap": self._churn("51-CORE-L2", "C5", directory, scale="core"),
            "L3_observer_smoke": self._churn("52-CORE-L3", "C2", directory, scale="core"),
        }
        anchor_rc = self.base._run(
            "53-CORE-L4-generator-anchor",
            [str(self.base.python), "-m", "unittest", "-v", GENERATOR_ANCHOR_TEST],
            env=self.base._product_env(threshold="1000000"),
        )
        results["L4_generator_anchor"] = {
            "result": "PASS" if anchor_rc == 0 else "FAIL",
            "test": GENERATOR_ANCHOR_TEST,
            "returncode": anchor_rc,
        }
        errors = [
            f"{key}: {error}"
            for key, value in results.items()
            for error in value.get("errors", [])
        ]
        case = {
            "result": (
                "PASS"
                if all(value.get("result") == "PASS" for value in results.values())
                else "FAIL"
            ),
            "sections": results,
            "errors": errors,
        }
        (directory / "result.json").write_text(
            json.dumps(case, indent=2, sort_keys=True) + "\n"
        )
        return case

    def run_shutdown(self) -> dict:
        directory = self.output / "SHUTDOWN"
        directory.mkdir()
        results = {
            "S1_park_die_reenable": self._churn("60-CORE-S1", "C4", directory, scale="core"),
            "S2_multithread_teardown": self._churn("61-CORE-S2", "C8", directory, scale="core"),
        }
        out = directory / "shutdown.json"
        self.base._run(
            "62-CORE-S3-shutdown-matrix",
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.a3_shutdown",
                "--quota",
                SHUTDOWN_CORE_QUOTA,
                "--child-timeout",
                "60",
                "--out",
                str(out),
            ],
            env={**self.base._product_env(threshold="1000000"), "PYTHONMALLOC": "debug"},
        )
        matrix = self._json(out)
        if matrix is None:
            matrix = {"result": "INFRA_FAIL", "errors": ["shutdown matrix produced no result"]}
        # v1.1 §17: multithread-completed must pass on its own, not by
        # dilution in the total.
        multithread = (matrix.get("per_state") or {}).get("multithread-completed") or {}
        multithread_clean = (
            multithread.get("attempts", 0) > 0
            and multithread.get("attempts") == multithread.get("successes")
        )
        results["S3_process_exit_matrix"] = {
            "result": (
                "PASS"
                if matrix.get("result") == "PASS" and multithread_clean
                else matrix.get("result", "INFRA_FAIL")
            ),
            "successful_exits": matrix.get("successful_exits"),
            "repetitions": matrix.get("repetitions"),
            "per_state": matrix.get("per_state"),
            "detector": matrix.get("detector"),
            "errors": (
                []
                if multithread_clean and matrix.get("result") == "PASS"
                else ["multithread-completed did not pass on its own"]
                + [str(row.get("errors")) for row in (matrix.get("failures") or [])[:5]]
            ),
        }
        errors = [
            f"{key}: {error}"
            for key, value in results.items()
            for error in value.get("errors", [])
        ]
        case = {
            "result": (
                "PASS"
                if all(value.get("result") == "PASS" for value in results.values())
                else "FAIL"
            ),
            "sections": results,
            "errors": errors,
        }
        (directory / "result.json").write_text(
            json.dumps(case, indent=2, sort_keys=True) + "\n"
        )
        return case

    # -- MEMSAFE ---------------------------------------------------------

    def _asan_env(self, extension_dir: Path, runtime: Path, extra=None) -> dict:
        env = self.base._base_env()
        env.update(
            LD_PRELOAD=str(runtime),
            ASAN_OPTIONS="detect_leaks=0:alloc_dealloc_mismatch=0",
            CINDERX_JIT_MODE="canary",
            PYTHONJITAUTO="1000000",
            PYTHONJITGENERATOR="1",
            PYTHONPATH=os.pathsep.join(
                [
                    str(extension_dir),
                    str(self.base.source / "cinderx/PythonLib"),
                    str(self.base.stage),
                ]
            ),
        )
        env.update(extra or {})
        return env

    def run_memsafe(self) -> dict:
        directory = self.output / "MEMSAFE"
        directory.mkdir()
        if self.asan_build is None or not self.asan_build.is_dir():
            case = {
                "result": "INFRA_FAIL",
                "errors": [
                    "MEMSAFE needs --asan-build pointing at a "
                    "run_asan_build_311.sh output; refusing to pass silently"
                ],
            }
            (directory / "result.json").write_text(json.dumps(case, indent=2) + "\n")
            return case
        extensions = sorted(self.asan_build.glob("**/_cinderx*.so"))
        gcc = shutil.which("gcc")
        runtime = None
        if gcc:
            probe = subprocess.run(
                [gcc, "-print-file-name=libasan.so"], capture_output=True, text=True
            )
            candidate = Path(probe.stdout.strip())
            runtime = candidate if candidate.is_file() else None
        errors = []
        if not extensions:
            errors.append("no instrumented _cinderx.so under the ASAN build")
        if runtime is None:
            errors.append("libasan.so not found via gcc")
        instrumented = False
        if extensions:
            symbols = subprocess.run(
                ["nm", "-D", str(extensions[0])], capture_output=True, text=True
            )
            instrumented = symbols.stdout.count("__asan") > 0
            if not instrumented:
                errors.append(
                    "the extension under test carries no __asan symbols; the "
                    "leg would sanitize nothing"
                )
        if errors:
            case = {"result": "INFRA_FAIL", "errors": errors}
            (directory / "result.json").write_text(json.dumps(case, indent=2) + "\n")
            return case

        extension_dir = extensions[0].parent
        env = self._asan_env(extension_dir, runtime)
        results = {}
        commands = []
        for label, scenario, extra in (
            ("T1_function_death", "C1", None),
            ("T2_code_swap", "C5", None),
            ("T4_parked_death", "C4", None),
            ("T5_publication_failure", "C7", None),
            (
                "T6_multithread_teardown",
                "C8",
                {
                    "PYTHONJITMULTITHREADEDCOMPILETEST": "1",
                    "PYTHONJITBATCHCOMPILEWORKERS": "4",
                },
            ),
        ):
            name = f"70-CORE-ASAN-{label}"
            commands.append(name)
            out = directory / f"{label}.json"
            self.base._run(
                name,
                [
                    str(self.base.python),
                    "-m",
                    "ci_pipeline.jit311.a3_churn",
                    "--scenario",
                    scenario,
                    "--quick",
                    "--out",
                    str(out),
                ],
                env={**env, **(extra or {})},
            )
            payload = self._json(out) or {
                "result": "FAIL",
                "errors": ["no JSON evidence (crashed under ASAN?)"],
            }
            results[label] = apply_core_policy(payload)
        commands.append("71-CORE-ASAN-T3-generator-pin")
        pin_rc = self.base._run(
            "71-CORE-ASAN-T3-generator-pin",
            [str(self.base.python), "-c", GENERATOR_PIN_PROBE],
            env=env,
        )
        results["T3_generator_pin"] = {
            "result": "PASS" if pin_rc == 0 else "FAIL",
            "returncode": pin_rc,
        }
        commands.append("72-CORE-ASAN-T7-shutdown")
        out = directory / "T7_shutdown.json"
        self.base._run(
            "72-CORE-ASAN-T7-shutdown",
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.a3_shutdown",
                "--only-state",
                "multithread-completed",
                "--repetitions",
                "30",
                # ASAN owns malloc; glibc poisoning is inert under it and
                # the shadow poisoning is stronger.
                "--malloc-perturb",
                "0",
                "--child-timeout",
                "120",
                "--out",
                str(out),
            ],
            env=env,
        )
        shutdown = self._json(out) or {"result": "FAIL", "errors": ["no result"]}
        results["T7_multithread_shutdown"] = {
            "result": shutdown.get("result", "FAIL"),
            "successful_exits": shutdown.get("successful_exits"),
            "repetitions": shutdown.get("repetitions"),
            "errors": [str(row.get("errors")) for row in (shutdown.get("failures") or [])[:5]],
        }

        # Fail closed on any sanitizer report in any leg's log, whether or
        # not the process managed to exit zero.
        sanitizer_hits = []
        for name in commands:
            log = self.base.logs / f"{name}.log"
            if log.is_file() and "AddressSanitizer" in log.read_text(errors="replace"):
                sanitizer_hits.append(name)
        errors = [
            f"{key}: {error}"
            for key, value in results.items()
            for error in value.get("errors", [])
        ]
        errors.extend(f"sanitizer report in {name}" for name in sanitizer_hits)
        case = {
            "result": (
                "PASS"
                if not sanitizer_hits
                and all(value.get("result") == "PASS" for value in results.values())
                else "FAIL"
            ),
            "extension": str(extensions[0]),
            "instrumented": instrumented,
            "sections": results,
            "sanitizer_reports": sanitizer_hits,
            "errors": errors,
        }
        (directory / "result.json").write_text(
            json.dumps(case, indent=2, sort_keys=True) + "\n"
        )
        return case

    # -- REALWORLD-PENETRATION-HOOK -------------------------------------

    def _hook_required(self) -> tuple[bool, dict]:
        if self.penetration_hook == "required":
            return True, {"mode": "required"}
        if self.penetration_hook == "skip":
            return False, {"mode": "skip", "note": "operator declined; recorded"}
        patterns = [
            line.strip()
            for line in (
                self.base.stage
                / "ci_pipeline/jit311/data/a3_lifecycle_sensitive_paths.txt"
            )
            .read_text()
            .splitlines()
            if line.strip() and not line.startswith("#")
        ]
        if not self.hook_base:
            # No base to diff against: the plan's final-release rule wins,
            # run rather than guess (v1.1 §26).
            return True, {"mode": "auto", "note": "no --hook-base; defaulting to required"}
        diff = subprocess.run(
            ["git", "diff", "--name-only", f"{self.hook_base}...HEAD"],
            cwd=self.base.source,
            capture_output=True,
            text=True,
        )
        if diff.returncode != 0:
            return True, {"mode": "auto", "note": "diff against --hook-base failed; defaulting to required"}
        changed = [line for line in diff.stdout.splitlines() if line]
        hits = match_sensitive(changed, patterns)
        return bool(hits), {
            "mode": "auto",
            "base": self.hook_base,
            "changed_files": len(changed),
            "sensitive_hits": hits[:20],
        }

    def run_hook(self) -> dict:
        directory = self.output / "PENETRATION"
        directory.mkdir()
        startup = directory / "startup"
        journal = directory / "journal"
        startup.mkdir()
        journal.mkdir()
        (startup / "sitecustomize.py").write_text(
            "from ci_pipeline.jit311 import a2_penetration\n"
        )
        modules = [
            line.strip()
            for line in (
                self.base.stage / "ci_pipeline/jit311/data/a1_compile_all_modules.txt"
            )
            .read_text()
            .splitlines()
            if line.strip() and not line.startswith("#")
        ]
        arm = directory / "arm"
        self.base._run(
            "80-CORE-PENETRATION-threshold50",
            [
                str(self.base.python),
                str(self.base.stage / "ci_pipeline/libtest_diff_311.py"),
                "run",
                "--python",
                str(self.base.python),
                "--jobs",
                "16",
                "--timeout",
                "900",
                "--out",
                str(arm),
                "--pythonpath-prepend",
                str(startup),
                "--pythonpath-prepend",
                str(self.base.stage),
                "--env",
                f"A2_PENETRATION_JOURNAL={journal}",
                "--env",
                "CINDERX_JIT_MODE=canary",
                "--env",
                "PYTHONJITGENERATOR=1",
                "--env",
                "PYTHONJITAUTO=50",
                "--env",
                "MALLOC_PERTURB_=165",
                "--tests",
                *modules,
            ],
            env=self.base._base_env(),
        )
        payload = self._json(arm / "result.json") or {}
        statuses = payload.get("modules") or {}
        non_pass = {name: state for name, state in statuses.items() if state != "pass"}
        errors = judge_semantic_results(statuses, expected=len(modules))

        # Review P0-1: the semantic result alone can go green with the JIT
        # silently absent; consume the worker evidence through the same
        # classifier A2 uses rather than a second implementation.
        classification_path = directory / "classification.json"
        self.base._run(
            "81-CORE-PENETRATION-classify",
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.a2_penetration",
                "--journal",
                str(journal),
                "--targets",
                str(
                    self.base.stage
                    / "ci_pipeline/jit311/data/a1_compile_all_modules.txt"
                ),
                "--test-result",
                str(arm / "result.json"),
                "--out",
                str(classification_path),
            ],
            env={**self.base._base_env(), "PYTHONPATH": str(self.base.stage)},
        )
        classification = self._json(classification_path)
        path_errors = judge_penetration_path(classification)
        errors.extend(f"path proof: {error}" for error in path_errors)

        totals = (classification or {}).get("totals") or {}
        result = {
            "result": "PASS" if not errors else "FAIL",
            "modules": len(statuses),
            "non_pass": non_pass,
            "poisoned": True,
            "path_proof": {
                "worker_jit_active": totals.get("worker_jit_active"),
                "machine_entries": totals.get("machine_entries"),
                "own_code_modules": totals.get(
                    "actual_own_code_machine_entry_modules"
                ),
                "ledger_dropped": totals.get("ledger_dropped"),
                "events_dropped": totals.get("events_dropped"),
                "unknown_refusals": (classification or {}).get("unknown_refusals"),
                "errors": path_errors,
            },
            "errors": errors,
        }
        (directory / "result.json").write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n"
        )
        return result

    # -- orchestration ---------------------------------------------------

    def run(self) -> str:  # type: ignore[override]
        provenance = self.base.preflight()
        frozen = self._frozen_a2()
        prerequisite = self._run_prerequisite()
        if frozen["result"] != "PASS":
            self.command_failures.extend(frozen["errors"])
        if prerequisite is None or prerequisite.get("result") != "PASS":
            self.command_failures.append("minimal A2 prerequisite did not pass")

        cases = {}
        if "LIFECYCLE" in self.cases:
            cases["LIFECYCLE"] = self.run_lifecycle()
        if "SHUTDOWN" in self.cases:
            cases["SHUTDOWN"] = self.run_shutdown()
        if "MEMSAFE" in self.cases:
            cases["MEMSAFE"] = self.run_memsafe()

        hook_required, hook_meta = self._hook_required()
        hook = self.run_hook() if hook_required else None

        status = final_status(
            selected=self.cases,
            cases=cases,
            hook_required=hook_required,
            hook_skipped=hook_meta.get("mode") == "skip",
            hook_run=hook,
            infra=self.command_failures,
        )

        diagnostics = {
            name: {
                section: value.get("resource_stability_diagnostic")
                for section, value in (case.get("sections") or {}).items()
                if value.get("resource_stability_diagnostic")
            }
            for name, case in cases.items()
        }
        document = {
            "format": "cp311-jit-a3-core-v1",
            "profile": "core",
            "status": status,
            "provenance": provenance,
            "a2_frozen": frozen,
            "prerequisite": prerequisite,
            "cases": cases,
            "penetration_hook": {"required": hook_required, **hook_meta, "run": hook},
            "resource_stability_diagnostic": diagnostics,
            "command_failures": self.command_failures,
            "commands": self.base.command_results,
        }
        (self.output / "a3_core_result.json").write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n"
        )
        self._write_reports(document)
        return status

    def _write_reports(self, document: dict) -> None:
        def case_report(name: str, title: str) -> None:
            case = document["cases"].get(name)
            lines = [f"# {title}", "", f"Result: **{case['result'] if case else 'NOT_RUN'}**", ""]
            if case:
                lines.append("| Section | Result | Notes |")
                lines.append("|---|---|---|")
                for section, value in (case.get("sections") or {}).items():
                    note = "; ".join(value.get("errors", [])) or "-"
                    lines.append(f"| {section} | {value.get('result')} | {note} |")
                diagnostics = {
                    section: value.get("resource_stability_diagnostic")
                    for section, value in (case.get("sections") or {}).items()
                    if value.get("resource_stability_diagnostic", {}).get("errors_demoted")
                    or value.get("resource_stability_diagnostic", {}).get("capacity_readings")
                }
                if diagnostics:
                    lines += [
                        "",
                        "## RESOURCE_STABILITY_DIAGNOSTIC (recorded, not judged)",
                        "",
                        "```json",
                        json.dumps(diagnostics, indent=1, sort_keys=True),
                        "```",
                    ]
            (self.output / f"CP311_JIT_{name}_REPORT.md").write_text(
                "\n".join(lines) + "\n"
            )

        case_report("LIFECYCLE", "CPython 3.11 CinderX JIT 函数、代码对象与编译产物生命周期所有权一致性验证")
        case_report("SHUTDOWN", "CPython 3.11 CinderX JIT 暂停恢复、多线程编译清理与进程退出稳定性验证")
        case_report("MEMSAFE", "CPython 3.11 CinderX JIT 生命周期与销毁路径 Native 内存安全验证")

        hook = document["penetration_hook"]
        if hook.get("run") is not None:
            run = hook["run"]
            (self.output / "CP311_JIT_REALWORLD_PENETRATION_REPORT.md").write_text(
                "\n".join(
                    [
                        "# CPython 3.11 CinderX JIT 真实程序面渗透回归",
                        "",
                        f"Result: **{run['result']}**",
                        "",
                        f"- 模块: {run.get('modules')}/72（threshold=50 Auto-JIT，MALLOC_PERTURB_ 在开）",
                        f"- 非 pass: `{json.dumps(run.get('non_pass'))}`",
                        f"- JIT 路径证明: `{json.dumps(run.get('path_proof'), ensure_ascii=False)}`",
                        f"- 触发方式: `{json.dumps({k: v for k, v in hook.items() if k != 'run'}, ensure_ascii=False)}`",
                        "",
                    ]
                )
                + "\n"
            )

        status_note = (
            " (subset execution — a single-case result, not a full "
            "A3-Core acceptance)"
            if document["status"] == "NOT_FULLY_RUN"
            else ""
        )
        summary = [
            "# CP311 JIT A3-Core Report",
            "",
            f"Final: **{document['status']}**{status_note}",
            "",
            f"- Source SHA: `{document['provenance'].get('source_git_sha')}`",
            f"- Wheel SHA256: `{document['provenance'].get('wheel_sha256')}`",
            f"- A2 frozen evidence: `{document['a2_frozen'].get('result')}`",
            "",
            "| Case | Result |",
            "|---|---|",
        ]
        for name in CASES:
            case = document["cases"].get(name)
            summary.append(f"| {name} | {case['result'] if case else 'NOT_RUN'} |")
        hook_result = (
            (document["penetration_hook"].get("run") or {}).get("result", "NOT_TRIGGERED")
            if document["penetration_hook"]["required"]
            else "NOT_TRIGGERED"
        )
        summary.append(f"| REALWORLD-PENETRATION-HOOK | {hook_result} |")
        summary += [
            "",
            "Resource Stability Follow-ups (recorded, not judged): "
            "executable allocator retention, CodeRuntime slab high-water — "
            "see the per-case RESOURCE_STABILITY_DIAGNOSTIC sections.",
            "",
        ]
        (self.output / "CP311_JIT_A3_CORE_REPORT.md").write_text("\n".join(summary) + "\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wheel", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--case", choices=CASES, action="append")
    parser.add_argument("--asan-build", type=Path)
    parser.add_argument(
        "--penetration-hook",
        choices=("auto", "required", "skip"),
        default="auto",
    )
    parser.add_argument("--hook-base")
    parser.add_argument("--jobs", type=int, default=min(16, os.cpu_count() or 8))
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--a2-report", type=Path)
    parser.add_argument("--a2-result", type=Path)
    args = parser.parse_args(argv)
    output = args.out or Path.cwd() / f"cp311-a3-core-{datetime.now():%Y%m%d-%H%M%S}"
    runner = A3CoreRunner(
        wheel=args.wheel,
        source=args.source,
        output=output,
        jobs=args.jobs,
        timeout=args.timeout,
        a2_report=args.a2_report,
        a2_result=args.a2_result,
        asan_build=args.asan_build,
        penetration_hook=args.penetration_hook,
        hook_base=args.hook_base,
        cases=set(args.case or CASES),
    )
    try:
        status = runner.run()
    except Exception as exc:
        print(
            f"A3-Core runner failed before judgment: {type(exc).__name__}: {exc}",
            file=sys.stderr,
        )
        return 1
    print(f"A3-Core {status}: {runner.output / 'CP311_JIT_A3_CORE_REPORT.md'}")
    if status in ("PASS", "NOT_FULLY_RUN"):
        # A green subset exits zero but its report says NOT_FULLY_RUN --
        # it never claims a full A3-Core acceptance (review P0-2).
        return 0
    return 2 if status == "FAIL" else 1


if __name__ == "__main__":
    raise SystemExit(main())
