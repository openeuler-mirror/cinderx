"""Independent wheel-first CPython 3.11 CinderX JIT lifecycle discovery runner."""

from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from ci_pipeline.jit311.execution_acceptance import ExecutionAcceptanceRunner
from ci_pipeline.jit311.lifecycle_discovery_report import PRIMARY_CLUSTERS, judge, write_reports

# The frozen runtime-transition evidence was recorded under its
# campaign-era schema; these literals validate that historical material
# and are exempt from the semantic-naming lint.
HISTORICAL_TRANSITION_FREEZE_MARKER = "A2 FROZEN"  # naming-lint: allow
HISTORICAL_TRANSITION_FREEZE_KEY = "a2_freeze"  # naming-lint: allow


class LifecycleDiscoveryRunner:
    def __init__(
        self,
        *,
        wheel: Path,
        source: Path,
        output: Path,
        lanes: set[str],
        jobs: int,
        timeout: int,
        transition_report: Path | None,
        transition_result: Path | None,
    ) -> None:
        self.base = ExecutionAcceptanceRunner(
            wheel=wheel,
            source=source,
            output=output,
            lanes=set(),
            jobs=jobs,
            timeout=timeout,
        )
        self.lanes = lanes
        self.transition_report = transition_report.resolve() if transition_report else None
        self.transition_result = transition_result.resolve() if transition_result else None
        self.command_failures: list[str] = []

    @property
    def output(self) -> Path:
        return self.base.output

    def _json(self, path: Path) -> dict | None:
        return self.base._json(path)

    def _frozen_transition(self) -> dict:
        policy_path = self.base.stage / "ci_pipeline/jit311/data/lifecycle_transition_prerequisite.json"
        policy = json.loads(policy_path.read_text())
        frozen = policy["transition_frozen_commit"]
        ancestry = subprocess.run(
            ["git", "merge-base", "--is-ancestor", frozen, "HEAD"],
            cwd=self.base.source,
        ).returncode == 0
        # The freeze pins historical evidence, not present-day file names:
        # every frozen source is read back from the frozen commit itself, so
        # renaming a file later never counts as touching the frozen surface.
        source_hashes = {}
        source_errors = []
        for relative, expected in policy["frozen_source_files"].items():
            shown = subprocess.run(
                ["git", "show", f"{frozen}:{relative}"],
                cwd=self.base.source,
                capture_output=True,
            )
            if shown.returncode != 0:
                source_hashes[relative] = None
                source_errors.append(
                    f"frozen transition source missing from the frozen commit: {relative}"
                )
                continue
            actual = hashlib.sha256(shown.stdout).hexdigest()
            source_hashes[relative] = actual
            if actual != expected:
                source_errors.append(
                    f"frozen transition source changed at the frozen commit: {relative}"
                )
        evidence = {
            "result": "PASS",
            "policy": policy,
            "frozen_commit_is_ancestor": ancestry,
            "frozen_source_sha256": source_hashes,
            "external_report": None,
            "external_result": None,
            "errors": source_errors,
        }
        if not ancestry:
            evidence["errors"].append(
                "transition frozen commit is not an ancestor of source HEAD"
            )
        for supplied, key, expected_key in (
            (self.transition_report, "external_report", "canonical_report_sha256"),
            (self.transition_result, "external_result", "canonical_result_sha256"),
        ):
            if supplied is None:
                continue
            if not supplied.is_file():
                evidence["errors"].append(f"transition evidence file is missing: {supplied}")
                continue
            digest = hashlib.sha256(supplied.read_bytes()).hexdigest()
            evidence[key] = {"path": str(supplied), "sha256": digest}
            if digest != policy[expected_key]:
                evidence["errors"].append(f"transition evidence digest mismatch: {supplied}")
        if self.transition_report is not None:
            text = self.transition_report.read_text(errors="replace")
            if (
                HISTORICAL_TRANSITION_FREEZE_MARKER not in text
                or "PASS_WITH_APPROVED_DEVIATIONS" not in text
            ):
                evidence["errors"].append(
                    "external transition report lacks exact frozen/result markers"
                )
        if self.transition_result is not None:
            result_doc = self._json(self.transition_result)
            if (
                not result_doc
                or result_doc.get(HISTORICAL_TRANSITION_FREEZE_KEY)
                != HISTORICAL_TRANSITION_FREEZE_MARKER
            ):
                evidence["errors"].append(
                    "external transition result lacks the frozen marker"
                )
        if evidence["errors"]:
            evidence["result"] = "FAIL"
        (self.output / "transition_frozen_prerequisite.json").write_text(
            json.dumps(evidence, indent=2, sort_keys=True) + "\n"
        )
        return evidence

    def _run_prerequisite(self) -> dict | None:
        directory = self.output / "prerequisite"
        directory.mkdir()
        out = directory / "result.json"
        rc = self.base._run(
            "10-lifecycle-prerequisite",
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.lifecycle_prerequisite",
                "--out",
                str(out),
            ],
            env=self.base._product_env(threshold="1000000"),
        )
        result = self._json(out)
        if rc != 0 and result is None:
            self.command_failures.append("lifecycle prerequisite probe produced no result")
        return result

    def _churn_env(self, scenario: str) -> dict[str, str]:
        env = self.base._product_env(threshold="1000000")
        env["PYTHONMALLOC"] = "debug"
        if scenario == "C2":
            env.pop("PYTHONJITAUTO", None)
            env["PYTHONJITALL"] = "1"
        if scenario == "C8":
            env.update(
                PYTHONJITMULTITHREADEDCOMPILETEST="1",
                PYTHONJITBATCHCOMPILEWORKERS="4",
            )
        return env

    def run_churn(self) -> dict[str, dict]:
        directory = self.output / "churn"
        directory.mkdir()
        results = {}
        for scenario in sorted(PRIMARY_CLUSTERS):
            out = directory / f"{scenario}.json"
            rc = self.base._run(
                f"20-churn-{scenario}",
                [
                    str(self.base.python),
                    "-m",
                    "ci_pipeline.jit311.lifecycle_churn",
                    "--scenario",
                    scenario,
                    "--out",
                    str(out),
                ],
                env=self._churn_env(scenario),
            )
            result = self._json(out)
            if result is None:
                product_failure = rc == 124 or rc < 0
                result = {
                    "scenario": scenario,
                    "result": "FAIL" if product_failure else "INFRA_FAIL",
                    "errors": [f"scenario process exited {rc} without JSON evidence"],
                }
                out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
            results[scenario] = result
        return results

    def run_ownership(self) -> dict:
        directory = self.output / "ownership"
        directory.mkdir()
        corpus_dir = self.base.stage / "ci_pipeline/jit311"
        rows = []
        base_env = {
            **self.base._base_env(),
            "PYTHONPATH": str(self.base.stage),
            "PYTHONMALLOC": "debug",
        }
        jit_env = self.base._product_env(threshold="1000000")
        # The refcount matrix uses explicit force_compile.  Do not arm the
        # ROI threshold: a raising case could otherwise back off halfway
        # through its measured window, mixing interpreted and compiled
        # reference behaviour.
        jit_env.pop("PYTHONJITAUTO", None)
        jit_env["PYTHONMALLOC"] = "debug"
        for module in ("corpus_execute_min", "corpus_calls", "corpus_generators"):
            interp = directory / f"{module}-interp.json"
            jit = directory / f"{module}-jit.json"
            script = self.base.stage / "ci_pipeline/jit311/refcount_matrix.py"
            rc_interp = self.base._run(
                f"30-ownership-{module}-interp",
                [str(self.base.python), str(script), str(corpus_dir), module, "interp", str(interp)],
                env=base_env,
            )
            rc_jit = self.base._run(
                f"31-ownership-{module}-jit",
                [str(self.base.python), str(script), str(corpus_dir), module, "jit", str(jit)],
                env=jit_env,
            )
            rc_diff = self.base._run(
                f"32-ownership-{module}-diff",
                [str(self.base.python), str(script), "diff", str(interp), str(jit)],
                env=base_env,
            ) if interp.is_file() and jit.is_file() else 2
            rows.append(
                {
                    "module": module,
                    "interp_returncode": rc_interp,
                    "jit_returncode": rc_jit,
                    "diff_returncode": rc_diff,
                    "interp": self._json(interp),
                    "jit": self._json(jit),
                }
            )
        result = {
            "result": "PASS" if all(row["interp_returncode"] == row["jit_returncode"] == row["diff_returncode"] == 0 for row in rows) else "FAIL",
            "layers": {
                "O1": "C1-C6 weakref/gc census",
                "O2": "snapshot invariant checker at every checkpoint",
                "O3": "stock-carrier refcount drift and semantic differential",
            },
            "rows": rows,
            "errors": [f"{row['module']} refcount matrix failed" for row in rows if row["diff_returncode"] != 0 or row["interp_returncode"] != 0 or row["jit_returncode"] != 0],
        }
        (directory / "result.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
        return result

    # Per-state quotas, not a total: 100 round-robin exits gave the
    # multithread-completed state ~16 attempts.  The teardown class of
    # crash is deterministic per memory layout, so the lane also relies
    # on shutdown_stability's MALLOC_PERTURB_ poisoning and per-child layout
    # entropy rather than on repetition count alone.
    SHUTDOWN_QUOTA = (
        "installed=200,parked=200,function-death=200,code-death=200,"
        "failure-unwind=200,multithread-completed=2000"
    )

    def run_shutdown(self) -> dict | None:
        directory = self.output / "shutdown"
        directory.mkdir()
        out = directory / "shutdown.json"
        rc = self.base._run(
            "40-shutdown-stability",
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.shutdown_stability",
                "--quota",
                self.SHUTDOWN_QUOTA,
                "--child-timeout",
                "30",
                "--out",
                str(out),
            ],
            env={**self.base._product_env(threshold="1000000"), "PYTHONMALLOC": "debug"},
        )
        result = self._json(out)
        if result is None:
            self.command_failures.append(f"the shutdown lane returned {rc} without result")
        return result

    def run(self) -> str:
        provenance = self.base.preflight()
        shutil.copy2(
            self.base.source / "docs/design/cp311-a3-lifecycle-inventory.md",
            self.output / "CP311_JIT_LIFECYCLE_INVENTORY.md",
        )
        frozen = self._frozen_transition()
        prerequisite = self._run_prerequisite()
        if frozen["result"] != "PASS":
            self.command_failures.extend(frozen["errors"])
        if "fullsuite" in self.lanes:
            self.command_failures.append(
                "the full-suite lane is intentionally deferred until product "
                "discovery blockers converge"
            )
        churn_results = self.run_churn() if "churn" in self.lanes else {}
        ownership = self.run_ownership() if "ownership" in self.lanes else None
        finalize = self.run_shutdown() if "shutdown" in self.lanes else None
        status, blockers = judge(
            prerequisite=prerequisite,
            c_results=churn_results,
            ownership=ownership,
            finalize=finalize,
            command_failures=self.command_failures,
            require_c="churn" in self.lanes,
        )
        lane_results = {
            "churn": (
                "PASS" if churn_results and all(result.get("result") == "PASS" for result in churn_results.values()) else "FAIL" if churn_results else "NOT_RUN"
            ),
            "ownership": ownership.get("result") if ownership else "NOT_RUN",
            "shutdown": finalize.get("result") if finalize else "NOT_RUN",
            "fullsuite": "DEFERRED_UNTIL_DISCOVERY_BLOCKERS_CONVERGE",
        }
        document = {
            "format": "cp311-jit-lifecycle-discovery-v1",
            "status": status,
            "provenance": provenance,
            "transition_frozen": frozen,
            "prerequisite": prerequisite,
            "selected_lanes": sorted(self.lanes),
            "lane_results": lane_results,
            "lanes": {
                "churn": churn_results,
                "ownership": ownership,
                "shutdown": finalize,
                "fullsuite": None,
            },
            "blockers": blockers,
            "command_failures": self.command_failures,
            "commands": self.base.command_results,
        }
        write_reports(document, self.output)
        return status


def main(argv: list[str] | None = None) -> int:
    # --profile acceptance delegates to the formal lifecycle acceptance
    # profile with its own argument surface (--case/--asan-build/
    # --stdlib-regression/--regression-base); --profile discovery stays here.
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--profile" in argv:
        index = argv.index("--profile")
        profile = argv[index + 1] if index + 1 < len(argv) else ""
        remainder = argv[:index] + argv[index + 2 :]
        if profile == "acceptance":
            from ci_pipeline.jit311.lifecycle_acceptance import main as acceptance_main

            return acceptance_main(remainder)
        if profile != "discovery":
            print(f"unknown --profile {profile!r}", file=sys.stderr)
            return 2
        argv = remainder

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wheel", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    parser.add_argument(
        "--lane",
        choices=("churn", "ownership", "shutdown", "fullsuite"),
        action="append",
    )
    parser.add_argument("--jobs", type=int, default=min(16, os.cpu_count() or 8))
    parser.add_argument("--timeout", type=int, default=1200)
    parser.add_argument("--transition-report", type=Path)
    parser.add_argument("--transition-result", type=Path)
    args = parser.parse_args(argv)
    output = args.out or Path.cwd() / f"cp311-lifecycle-{datetime.now():%Y%m%d-%H%M%S}"
    runner = LifecycleDiscoveryRunner(
        wheel=args.wheel,
        source=args.source,
        output=output,
        lanes=set(args.lane or ("churn", "ownership", "shutdown")),
        jobs=args.jobs,
        timeout=args.timeout,
        transition_report=args.transition_report,
        transition_result=args.transition_result,
    )
    try:
        status = runner.run()
    except Exception as exc:
        print(
            f"lifecycle discovery failed before judgment: {type(exc).__name__}: {exc}",
            file=sys.stderr,
        )
        return 1
    print(f"lifecycle discovery {status}: {runner.output / 'CP311_JIT_LIFECYCLE_DISCOVERY_REPORT.md'}")
    return 0 if status == "DISCOVERY_PASS" else 2 if status == "DISCOVERY_FAIL" else 1


if __name__ == "__main__":
    raise SystemExit(main())
