"""Unified wheel-first CPython 3.11 CinderX JIT execution acceptance runner."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import zipfile


PASS_STATES = {"PASS", "PASS_WITH_APPROVED_DEVIATIONS"}


def require_matching_source_sha(embedded_sha: object, source_sha: str) -> None:
    if embedded_sha != source_sha:
        raise RuntimeError(
            "wheel/source provenance mismatch: "
            f"wheel={embedded_sha!r}, source={source_sha!r}"
        )


class ExecutionAcceptanceRunner:
    def __init__(
        self,
        *,
        wheel: Path,
        source: Path,
        output: Path,
        lanes: set[str],
        jobs: int,
        timeout: int,
    ) -> None:
        self.wheel = wheel.resolve()
        self.source = source.resolve()
        self.output = output.resolve()
        self.lanes = lanes
        self.jobs = jobs
        self.timeout = timeout
        self.stage = self.output / "harness"
        self.venv = self.output / "venv"
        self.python = self.venv / "bin" / "python"
        self.logs = self.output / "logs"
        self.results: dict[str, dict] = {}
        self.command_results: dict[str, dict] = {}

    def _base_env(self) -> dict[str, str]:
        env = {
            key: value
            for key, value in os.environ.items()
            if key not in ("PYTHONPATH", "PYTHONHOME", "PYTHONHASHSEED")
            and not key.startswith(("CINDERX_", "PYTHONJIT", "PARALLEL_GC_"))
        }
        env.update(PYTHONHASHSEED="0", PYTHONUNBUFFERED="1")
        return env

    def _product_env(self, *, threshold: str = "1000000") -> dict[str, str]:
        env = self._base_env()
        env.update(
            CINDERX_JIT_MODE="canary",
            PYTHONJITAUTO=threshold,
            PYTHONJITGENERATOR="1",
            PYTHONPATH=str(self.stage),
        )
        return env

    def _run(
        self,
        name: str,
        command: list[str],
        *,
        env: dict[str, str] | None = None,
        cwd: Path | None = None,
    ) -> int:
        log_path = self.logs / f"{name}.log"
        started = datetime.now(timezone.utc)
        with log_path.open("w", encoding="utf-8") as stream:
            stream.write("COMMAND " + json.dumps(command) + "\n")
            stream.flush()
            try:
                process = subprocess.run(
                    command,
                    cwd=cwd,
                    env=env or self._base_env(),
                    text=True,
                    stdout=stream,
                    stderr=subprocess.STDOUT,
                    timeout=self.timeout,
                )
                returncode = process.returncode
            except subprocess.TimeoutExpired:
                stream.write(f"TIMEOUT after {self.timeout}s\n")
                returncode = 124
        self.command_results[name] = {
            "command": command,
            "returncode": returncode,
            "log": str(log_path),
            "started_utc": started.isoformat(),
            "duration_s": round((datetime.now(timezone.utc) - started).total_seconds(), 3),
        }
        return returncode

    @staticmethod
    def _json(path: Path) -> dict | None:
        if not path.is_file():
            return None
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return None

    def preflight(self) -> dict:
        if not self.wheel.is_file() or self.wheel.suffix != ".whl":
            raise ValueError(f"wheel does not exist: {self.wheel}")
        if not (self.source / ".git").exists() and not (
            self.source / "pyproject.toml"
        ).exists():
            raise ValueError(f"source does not look like a CinderX checkout: {self.source}")
        if sys.version_info[:3] != (3, 11, 6):
            raise RuntimeError(
                f"the execution acceptance requires CPython 3.11.6, got {sys.version.split()[0]}"
            )

        if self.output.exists():
            if any(self.output.iterdir()):
                raise FileExistsError(f"the output directory is not empty: {self.output}")
        else:
            self.output.mkdir(parents=True)
        self.logs.mkdir()
        self.stage.mkdir()
        manifest_path = self.source / "ci_pipeline/jit311/data/execution_harness_manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        for entry in manifest["entries"]:
            source_path = self.source / entry["source"]
            destination = self.stage / entry["destination"]
            destination.parent.mkdir(parents=True, exist_ok=True)
            if entry["kind"] == "tree":
                shutil.copytree(
                    source_path,
                    destination,
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo"),
                )
            else:
                shutil.copy2(source_path, destination)

        self._run(
            "00-create-venv",
            [sys.executable, "-m", "venv", "--system-site-packages", str(self.venv)],
        )
        if not self.python.is_file():
            raise RuntimeError("venv creation failed; see 00-create-venv.log")
        if self._run(
            "01-install-wheel",
            [str(self.python), "-m", "pip", "install", "--no-index", "--no-deps", str(self.wheel)],
        ) != 0:
            raise RuntimeError("wheel installation failed; see 01-install-wheel.log")

        wheel_sha = hashlib.sha256(self.wheel.read_bytes()).hexdigest()
        embedded = None
        with zipfile.ZipFile(self.wheel) as archive:
            embedded = json.loads(
                archive.read("cinderx/_native/build_info_311.json").decode("utf-8")
            )
        source_sha = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=self.source,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        for command in (
            ["git", "diff", "--quiet"],
            ["git", "diff", "--cached", "--quiet"],
        ):
            if subprocess.run(command, cwd=self.source).returncode != 0:
                raise RuntimeError(
                    "the official execution acceptance requires a clean tracked source tree"
                )
        embedded_sha = embedded.get("git_sha")
        require_matching_source_sha(embedded_sha, source_sha)
        runtime_probe = subprocess.run(
            [
                str(self.python),
                "-c",
                "import json,sys,_cinderx,cinderx; print(json.dumps({"
                "'python':sys.version,'cinderx':cinderx.__file__,"
                "'_cinderx':_cinderx.__file__}, sort_keys=True))",
            ],
            check=True,
            capture_output=True,
            text=True,
            env=self._base_env(),
        )
        runtime = json.loads(runtime_probe.stdout.strip().splitlines()[-1])
        for module_path in (runtime["cinderx"], runtime["_cinderx"]):
            resolved = Path(module_path).resolve()
            if str(resolved).startswith(str(self.source)) or "site-packages" not in str(resolved):
                raise RuntimeError(f"product module did not load from wheel site-packages: {resolved}")

        provenance = {
            "python": runtime["python"],
            "python_executable": str(self.python),
            "architecture": platform.machine(),
            "wheel": self.wheel.name,
            "wheel_sha256": wheel_sha,
            "wheel_embedded": embedded,
            "source_git_sha": source_sha,
            "cinderx_file": runtime["cinderx"],
            "_cinderx_file": runtime["_cinderx"],
            "harness_manifest": manifest,
            "wheel_source_sha_match": True,
        }
        (self.output / "provenance.json").write_text(
            json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        return provenance

    def run_execution_smoke(self) -> dict:
        directory = self.output / "EXECUTION_SMOKE"
        directory.mkdir()
        module = "ci_pipeline.jit311.execution_smoke"
        negative = directory / "negative.json"
        positive = directory / "threshold50.json"
        rc_negative = self._run(
            "10-S-negative",
            [str(self.python), "-m", module, "--expect-mode", "off", "--out", str(negative)],
            env={**self._base_env(), "PYTHONPATH": str(self.stage)},
        )
        rc_positive = self._run(
            "11-S-threshold50",
            [str(self.python), "-m", module, "--expect-mode", "execute", "--out", str(positive)],
            env=self._product_env(threshold="50"),
        )
        result = {
            "result": "PASS"
            if rc_negative == rc_positive == 0
            and (self._json(negative) or {}).get("result") == "PASS"
            and (self._json(positive) or {}).get("result") == "PASS"
            else "FAIL",
            "negative": self._json(negative),
            "threshold50": self._json(positive),
        }
        (directory / "result.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
        return result

    def _arm_command(
        self,
        *,
        out: Path,
        startup: Path | None = None,
        mode: str | None = None,
        journal: Path | None = None,
    ) -> list[str]:
        targets = [
            line.strip()
            for line in (
                self.stage / "ci_pipeline/jit311/data/frozen_stdlib_modules.txt"
            ).read_text().splitlines()
            if line.strip() and not line.startswith("#")
        ]
        command = [
            str(self.python),
            str(self.stage / "ci_pipeline/libtest_diff_311.py"),
            "run",
            "--python",
            str(self.python),
            "--jobs",
            str(self.jobs),
            "--timeout",
            str(self.timeout),
            "--out",
            str(out),
        ]
        if startup is not None:
            command += ["--pythonpath-prepend", str(startup), "--pythonpath-prepend", str(self.stage)]
        if mode is not None:
            command += ["--env", f"EXECUTION_COMPILE_ALL_MODE={mode}"]
        if journal is not None:
            command += ["--env", f"EXECUTION_COMPILE_ALL_LOG_DIR={journal}"]
        if mode == "jit":
            command += [
                "--env", "CINDERX_JIT_MODE=canary",
                "--env", "PYTHONJITAUTO=1000000",
                "--env", "PYTHONJITGENERATOR=1",
            ]
        command += ["--tests", *targets]
        return command

    def run_semantic_conformance(self) -> dict:
        directory = self.output / "SEMANTIC_CONFORMANCE"
        directory.mkdir()
        c0, c1, c2 = directory / "c0", directory / "c1", directory / "c2"
        c1_journal, c2_journal = directory / "c1-journal", directory / "c2-journal"
        c1_journal.mkdir()
        c2_journal.mkdir()
        c1_startup, c2_startup = directory / "c1-startup", directory / "c2-startup"
        c1_startup.mkdir()
        c2_startup.mkdir()
        (c1_startup / "sitecustomize.py").write_text(
            "import os,sys\n"
            "assert os.environ.get('EXECUTION_COMPILE_ALL_MODE') == 'instrumented'\n"
            "assert 'cinderx' not in sys.modules and '_cinderx' not in sys.modules\n"
            "from ci_pipeline.jit311 import semantic_conformance_hook\n",
            encoding="utf-8",
        )
        (c2_startup / "sitecustomize.py").write_text(
            "import os\n"
            "assert os.environ.get('EXECUTION_COMPILE_ALL_MODE') == 'jit'\n"
            "import _cinderx,cinderx\n"
            "cinderx.init()\n"
            "_cinderx.install_frame_evaluator()\n"
            "from ci_pipeline.jit311 import semantic_conformance_hook\n",
            encoding="utf-8",
        )
        rc0 = self._run("20-C0-stock", self._arm_command(out=c0))
        rc1 = self._run(
            "21-C1-instrumented",
            self._arm_command(out=c1, startup=c1_startup, mode="instrumented", journal=c1_journal),
        )
        rc2 = self._run(
            "22-C2-compile-all",
            self._arm_command(out=c2, startup=c2_startup, mode="jit", journal=c2_journal),
        )

        empty_deviations = directory / "empty-deviations.json"
        empty_deviations.write_text('{"format":"cp311-jit-execution-deviations-v1","deviations":[]}\n')
        c0c1 = directory / "c0-vs-c1.json"
        c1c2 = directory / "c1-vs-c2.json"
        report_module = "ci_pipeline.jit311.execution_report"
        common_env = {**self._base_env(), "PYTHONPATH": str(self.stage)}
        rc_c0c1 = self._run(
            "23-C0-vs-C1",
            [str(self.python), "-m", report_module, "compare", "--stock", str(c0 / "result.json"), "--execute", str(c1 / "result.json"), "--deviations", str(empty_deviations), "--out", str(c0c1)],
            env=common_env,
        )
        rc_c1c2 = self._run(
            "24-C1-vs-C2",
            [str(self.python), "-m", report_module, "compare", "--stock", str(c1 / "result.json"), "--execute", str(c2 / "result.json"), "--deviations", str(self.stage / "ci_pipeline/jit311/data/execution_compatibility_deviations.json"), "--out", str(c1c2)],
            env=common_env,
        )
        classification = directory / "classification.json"
        rc_classify = self._run(
            "25-C-classification",
            [str(self.python), "-m", report_module, "classify", "--journal", str(c2_journal), "--targets", str(self.stage / "ci_pipeline/jit311/data/frozen_stdlib_modules.txt"), "--capabilities", str(self.stage / "ci_pipeline/jit311/data/semantic_conformance_capabilities.toml"), "--out", str(classification)],
            env=common_env,
        )
        tracing = directory / "tracing.json"
        rc_tracing = self._run(
            "26-C-tracing-T1-T8",
            [str(self.python), "-m", "ci_pipeline.jit311.tracing_conformance_probe", "--out", str(tracing)],
            env=self._product_env(),
        )
        generator = directory / "generator.json"
        rc_generator = self._run(
            "27-C-generator",
            [str(self.python), "-m", "ci_pipeline.jit311.generator_execution_probe", "--out", str(generator)],
            env=self._product_env(),
        )
        dis_probe = directory / "dis-deviation-probe.json"
        rc_dis_probe = self._run(
            "28-C-dis-deviation-probe",
            [str(self.python), "-m", "ci_pipeline.jit311.specialization_disassembly_probe", "--out", str(dis_probe)],
            env=self._product_env(),
        )
        c0c1_report = self._json(c0c1)
        c1c2_report = self._json(c1c2)
        classification_report = self._json(classification)
        tracing_report = self._json(tracing)
        generator_report = self._json(generator)
        dis_probe_report = self._json(dis_probe)
        good = (
            all(code == 0 for code in (rc0, rc1, rc2, rc_c0c1, rc_c1c2, rc_classify, rc_tracing, rc_generator, rc_dis_probe))
            and (c0c1_report or {}).get("result") == "PASS"
            and (c1c2_report or {}).get("result") in PASS_STATES
            and (classification_report or {}).get("result") == "PASS"
            and (tracing_report or {}).get("result") == "PASS"
            and (generator_report or {}).get("result") == "PASS"
            and (dis_probe_report or {}).get("result") == "PASS"
        )
        result = {
            "result": (
                "PASS_WITH_APPROVED_DEVIATIONS"
                if good and (c1c2_report or {}).get("differences")
                else "PASS" if good else "FAIL"
            ),
            "c0_vs_c1": c0c1_report,
            "c1_vs_c2": c1c2_report,
            "classification": classification_report,
            "tracing": tracing_report,
            "generator": generator_report,
            "dis_deviation_probe": dis_probe_report,
        }
        (directory / "result.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
        return result

    def run_specialization_conformance(self) -> dict:
        directory = self.output / "SPECIALIZATION_CONFORMANCE"
        directory.mkdir()
        result_path = directory / "result.json"
        rc = self._run(
            "30-W-specialization",
            [str(self.python), "-m", "ci_pipeline.jit311.specialization_conformance", "--manifest", str(self.stage / "ci_pipeline/jit311/data/specialization_conformance_manifest.toml"), "--out", str(result_path)],
            env=self._product_env(),
        )
        result = self._json(result_path) or {"result": "FAIL"}
        if rc != 0:
            result["result"] = "FAIL"
        return result

    def finalize(self, provenance: dict) -> str:
        states = [self.results[lane]["result"] for lane in sorted(self.results)]
        if any(state == "FAIL" for state in states):
            final = "FAIL"
        elif any(state == "REVIEW_REQUIRED" for state in states):
            final = "REVIEW_REQUIRED"
        elif any(state == "PASS_WITH_APPROVED_DEVIATIONS" for state in states):
            final = "PASS_WITH_APPROVED_DEVIATIONS"
        else:
            final = "PASS"

        convergence = {
            "final": final,
            "cases": self.results,
            "commands": self.command_results,
            "provenance": provenance,
        }
        (self.output / "execution_result.json").write_text(
            json.dumps(convergence, indent=2, sort_keys=True) + "\n"
        )
        return final

    def run(self) -> str:
        provenance = self.preflight()
        if "execution_smoke" in self.lanes:
            self.results["execution_smoke"] = self.run_execution_smoke()
        if "semantic_conformance" in self.lanes:
            self.results["semantic_conformance"] = self.run_semantic_conformance()
        if "specialization_conformance" in self.lanes:
            self.results["specialization_conformance"] = self.run_specialization_conformance()
        return self.finalize(provenance)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wheel", type=Path, default=None)
    parser.add_argument("--source", type=Path, default=None)
    parser.add_argument("--out", type=Path)
    parser.add_argument(
        "--case",
        type=lambda value: value.replace("-", "_").lower(),
        choices=("execution_smoke", "semantic_conformance", "specialization_conformance"),
        action="append",
    )
    parser.add_argument("--jobs", type=int, default=min(16, os.cpu_count() or 8))
    parser.add_argument("--timeout", type=int, default=1200)
    args = parser.parse_args(argv)
    if args.wheel is None:
        print("no wheel: pass --wheel or mount it at /wheels", file=sys.stderr)
        return 2
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    output = args.out or Path.cwd() / f"cp311-execution-{timestamp}"
    runner = ExecutionAcceptanceRunner(
        wheel=args.wheel,
        source=args.source,
        output=output,
        lanes=set(args.case or ("execution_smoke", "semantic_conformance", "specialization_conformance")),
        jobs=args.jobs,
        timeout=args.timeout,
    )
    try:
        final = runner.run()
    except Exception as exc:
        print(f"execution acceptance failed before final judgment: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1
    # The last line is the whole verdict: approved deviations are a pass.
    print("PASS" if final in PASS_STATES else final)
    print(f"evidence: {runner.output / 'execution_result.json'}", file=sys.stderr)
    if final in PASS_STATES:
        return 0
    return 2 if final == "REVIEW_REQUIRED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
