"""Independent wheel-first CPython 3.11 CinderX JIT A3 discovery runner."""

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

from ci_pipeline.jit311.a1_runner import A1Runner
from ci_pipeline.jit311.a3_report import PRIMARY_CLUSTERS, judge, write_reports


class A3Runner:
    def __init__(
        self,
        *,
        wheel: Path,
        source: Path,
        output: Path,
        lanes: set[str],
        jobs: int,
        timeout: int,
        a2_report: Path | None,
        a2_result: Path | None,
    ) -> None:
        self.base = A1Runner(
            wheel=wheel,
            source=source,
            output=output,
            lanes=set(),
            jobs=jobs,
            timeout=timeout,
        )
        self.lanes = lanes
        self.a2_report = a2_report.resolve() if a2_report else None
        self.a2_result = a2_result.resolve() if a2_result else None
        self.command_failures: list[str] = []

    @property
    def output(self) -> Path:
        return self.base.output

    def _json(self, path: Path) -> dict | None:
        return self.base._json(path)

    def _frozen_a2(self) -> dict:
        policy_path = self.base.stage / "ci_pipeline/jit311/data/a3_a2_frozen_prerequisite.json"
        policy = json.loads(policy_path.read_text())
        frozen = policy["a2_frozen_commit"]
        ancestry = subprocess.run(
            ["git", "merge-base", "--is-ancestor", frozen, "HEAD"],
            cwd=self.base.source,
        ).returncode == 0
        source_hashes = {}
        source_errors = []
        for relative, expected in policy["frozen_source_files"].items():
            actual = hashlib.sha256((self.base.source / relative).read_bytes()).hexdigest()
            source_hashes[relative] = actual
            if actual != expected:
                source_errors.append(f"frozen A2 source changed: {relative}")
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
            evidence["errors"].append("A2 frozen commit is not an ancestor of source HEAD")
        for supplied, key, expected_key in (
            (self.a2_report, "external_report", "canonical_report_sha256"),
            (self.a2_result, "external_result", "canonical_result_sha256"),
        ):
            if supplied is None:
                continue
            if not supplied.is_file():
                evidence["errors"].append(f"A2 evidence file is missing: {supplied}")
                continue
            digest = hashlib.sha256(supplied.read_bytes()).hexdigest()
            evidence[key] = {"path": str(supplied), "sha256": digest}
            if digest != policy[expected_key]:
                evidence["errors"].append(f"A2 evidence digest mismatch: {supplied}")
        if self.a2_report is not None:
            text = self.a2_report.read_text(errors="replace")
            if "A2 FROZEN" not in text or "PASS_WITH_APPROVED_DEVIATIONS" not in text:
                evidence["errors"].append("external A2 report lacks exact frozen/result markers")
        if self.a2_result is not None:
            result_doc = self._json(self.a2_result)
            if not result_doc or result_doc.get("a2_freeze") != "A2 FROZEN":
                evidence["errors"].append("external A2 result lacks A2 FROZEN")
        if evidence["errors"]:
            evidence["result"] = "FAIL"
        (self.output / "a2_frozen_prerequisite.json").write_text(
            json.dumps(evidence, indent=2, sort_keys=True) + "\n"
        )
        return evidence

    def _run_prerequisite(self) -> dict | None:
        directory = self.output / "prerequisite"
        directory.mkdir()
        out = directory / "result.json"
        rc = self.base._run(
            "10-A3-minimal-A2-prerequisite",
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.a3_prerequisite",
                "--out",
                str(out),
            ],
            env=self.base._product_env(threshold="1000000"),
        )
        result = self._json(out)
        if rc != 0 and result is None:
            self.command_failures.append("minimal A2 prerequisite produced no result")
        return result

    def _c_env(self, scenario: str) -> dict[str, str]:
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

    def run_c(self) -> dict[str, dict]:
        directory = self.output / "C"
        directory.mkdir()
        results = {}
        for scenario in sorted(PRIMARY_CLUSTERS):
            out = directory / f"{scenario}.json"
            rc = self.base._run(
                f"20-A3-{scenario}",
                [
                    str(self.base.python),
                    "-m",
                    "ci_pipeline.jit311.a3_churn",
                    "--scenario",
                    scenario,
                    "--out",
                    str(out),
                ],
                env=self._c_env(scenario),
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

    def run_o(self) -> dict:
        directory = self.output / "O"
        directory.mkdir()
        corpus_dir = self.base.stage / "ci_pipeline/jit311"
        rows = []
        base_env = {
            **self.base._base_env(),
            "PYTHONPATH": str(self.base.stage),
            "PYTHONMALLOC": "debug",
        }
        jit_env = self.base._product_env(threshold="1000000")
        # O3 uses explicit force_compile.  Do not arm the ROI threshold: a
        # raising case could otherwise back off halfway through its measured
        # window, mixing interpreted and compiled reference behaviour.
        jit_env.pop("PYTHONJITAUTO", None)
        jit_env["PYTHONMALLOC"] = "debug"
        for module in ("corpus_execute_min", "corpus_calls", "corpus_generators"):
            interp = directory / f"{module}-interp.json"
            jit = directory / f"{module}-jit.json"
            script = self.base.stage / "ci_pipeline/jit311/refcount_matrix.py"
            rc_interp = self.base._run(
                f"30-A3-O-{module}-interp",
                [str(self.base.python), str(script), str(corpus_dir), module, "interp", str(interp)],
                env=base_env,
            )
            rc_jit = self.base._run(
                f"31-A3-O-{module}-jit",
                [str(self.base.python), str(script), str(corpus_dir), module, "jit", str(jit)],
                env=jit_env,
            )
            rc_diff = self.base._run(
                f"32-A3-O-{module}-diff",
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
    # on a3_shutdown's MALLOC_PERTURB_ poisoning and per-child layout
    # entropy rather than on repetition count alone.
    SHUTDOWN_QUOTA = (
        "installed=200,parked=200,function-death=200,code-death=200,"
        "failure-unwind=200,multithread-completed=2000"
    )

    def run_f(self) -> dict | None:
        directory = self.output / "F"
        directory.mkdir()
        out = directory / "shutdown.json"
        rc = self.base._run(
            "40-A3-F-shutdown",
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.a3_shutdown",
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
            self.command_failures.append(f"A3-F returned {rc} without result")
        return result

    def run(self) -> str:
        provenance = self.base.preflight()
        shutil.copy2(
            self.base.source / "docs/design/cp311-a3-lifecycle-inventory.md",
            self.output / "A3_LIFECYCLE_INVENTORY.md",
        )
        frozen = self._frozen_a2()
        prerequisite = self._run_prerequisite()
        if frozen["result"] != "PASS":
            self.command_failures.extend(frozen["errors"])
        if "S" in self.lanes:
            self.command_failures.append(
                "A3-S is intentionally deferred until Product discovery blockers converge"
            )
        c_results = self.run_c() if "C" in self.lanes else {}
        ownership = self.run_o() if "O" in self.lanes else None
        finalize = self.run_f() if "F" in self.lanes else None
        status, blockers = judge(
            prerequisite=prerequisite,
            c_results=c_results,
            ownership=ownership,
            finalize=finalize,
            command_failures=self.command_failures,
            require_c="C" in self.lanes,
        )
        lane_results = {
            "C": (
                "PASS" if c_results and all(result.get("result") == "PASS" for result in c_results.values()) else "FAIL" if c_results else "NOT_RUN"
            ),
            "O": ownership.get("result") if ownership else "NOT_RUN",
            "F": finalize.get("result") if finalize else "NOT_RUN",
            "S": "DEFERRED_UNTIL_DISCOVERY_BLOCKERS_CONVERGE",
        }
        document = {
            "format": "cp311-jit-a3-discovery-v1",
            "status": status,
            "provenance": provenance,
            "a2_frozen": frozen,
            "prerequisite": prerequisite,
            "selected_lanes": sorted(self.lanes),
            "lane_results": lane_results,
            "lanes": {"C": c_results, "O": ownership, "F": finalize, "S": None},
            "blockers": blockers,
            "command_failures": self.command_failures,
            "commands": self.base.command_results,
        }
        write_reports(document, self.output)
        return status


def main(argv: list[str] | None = None) -> int:
    # --profile core delegates to the simplified-plan v1.1 acceptance
    # profile with its own argument surface (--case/--asan-build/
    # --penetration-hook/--hook-base); --profile full stays here.
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--profile" in argv:
        index = argv.index("--profile")
        profile = argv[index + 1] if index + 1 < len(argv) else ""
        remainder = argv[:index] + argv[index + 2 :]
        if profile == "core":
            from ci_pipeline.jit311.a3_core import main as core_main

            return core_main(remainder)
        if profile != "full":
            print(f"unknown --profile {profile!r}", file=sys.stderr)
            return 2
        argv = remainder

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wheel", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--lane", choices=("C", "O", "F", "S"), action="append")
    parser.add_argument("--jobs", type=int, default=min(16, os.cpu_count() or 8))
    parser.add_argument("--timeout", type=int, default=1200)
    parser.add_argument("--a2-report", type=Path)
    parser.add_argument("--a2-result", type=Path)
    args = parser.parse_args(argv)
    output = args.out or Path.cwd() / f"cp311-a3-{datetime.now():%Y%m%d-%H%M%S}"
    runner = A3Runner(
        wheel=args.wheel,
        source=args.source,
        output=output,
        lanes=set(args.lane or ("C", "O", "F")),
        jobs=args.jobs,
        timeout=args.timeout,
        a2_report=args.a2_report,
        a2_result=args.a2_result,
    )
    try:
        status = runner.run()
    except Exception as exc:
        print(f"A3 runner failed before discovery judgment: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1
    print(f"A3 {status}: {runner.output / 'A3_DISCOVERY_REPORT.md'}")
    return 0 if status == "DISCOVERY_PASS" else 2 if status == "DISCOVERY_FAIL" else 1


if __name__ == "__main__":
    raise SystemExit(main())
