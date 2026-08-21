# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Shadow-mode execution of the frozen 440-module regrtest surface and
test_cinderx.

The 72-module import driver in runners.stdlib_shadow_runner is the MR-05
canary: it does not execute test functions. Completeness for MR-03 is this
leg: run the committed 440-module list and the full test_cinderx tree under
CINDERX_JIT_MODE=shadow, record per-module and per-case outcomes, and refuse
unknown rejects or any machine-code side effect.

Tests still run on the interpreter. Shadow compile discards the artifact;
functions the front end cannot translate (exception handlers, generators)
stay interpreted. Completeness is differential against a JIT-off run of
the same frozen surface: pytest/regrtest failures that already exist on
that baseline are recorded, but a new failure, collection error, empty
suite, crash, missing report, unknown_reject or non-zero executable
counter is red. compile_success counts only when a compiled function's
co_filename+qualname belongs to the target, not a process-global counter.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

from ci_pipeline.jit311 import report as _report
from ci_pipeline.jit311.runners import (
    LIBTEST_TARGET_MANIFEST,
    REPO_ROOT,
    load_libtest_target_manifest,
)
from ci_pipeline.libtest_diff_311 import (
    arm_environment,
    arm_run_completed,
    parse_junit,
    parse_regrtest_modules,
)

MACHINE_CODE_FIELDS = (
    "executable_alloc_calls",
    "executable_alloc_bytes",
    "compiled_function_creations",
    "machine_code_installed",
    "machine_code_entries",
)

SHADOW_ENV = {
    "CINDERX_PLUGIN_ENABLE": "1",
    "CINDERX_EVAL_MODE": "cinder",
    "CINDERX_JIT_MODE": "shadow",
    "PYTHONJITAUTO": "1",
}

JIT_OFF_ENV = {
    "CINDERX_PLUGIN_ENABLE": "1",
    "CINDERX_EVAL_MODE": "cinder",
    "CINDERX_JIT_MODE": "off",
}

# Sitecustomize extras are popped before report.validate_schema.
SURFACE_EXTRA_KEYS = ("surface_module", "surface_kind", "compiled_functions")

EXECUTED_CASE_STATES = ("pass", "failure", "error")

KNOWN_SKIP_PLUGIN_DIR = (
    REPO_ROOT / "cinderx" / "TestScripts" / "TestScriptsKunpeng"
)
TEST_CINDERX_RUNNER = KNOWN_SKIP_PLUGIN_DIR / "test_cinderx_runner.py"

# Loaded by every interpreter the surface spawns, including regrtest
# workers and pytest children. Interpreters without _cinderx (fresh venvs
# created by tests) must keep running; only processes that actually loaded
# the extension write a snapshot.
SURFACE_SITECUSTOMIZE = """\
import atexit
import json
import os
import sys

_cinderx_site = os.environ.get("JIT311_CINDERX_SITE")
if _cinderx_site:
    sys.path.insert(0, _cinderx_site)
_repo = os.environ.get("JIT311_REPO_ROOT")
if _repo:
    sys.path.insert(0, _repo)

try:
    import _cinderx
    import cinderx
except ModuleNotFoundError:
    pass
else:
    try:
        cinderx.init()
        if not _cinderx.is_frame_evaluator_installed():
            _cinderx.install_frame_evaluator()
        installed = bool(_cinderx.is_frame_evaluator_installed())
    except Exception:
        import traceback
        traceback.print_exc()
        os._exit(78)
    if not installed:
        print("cinderx evaluator not installed after install call",
              file=sys.stderr)
        os._exit(78)

    _evaluator_installed_at_start = installed

    def _jit311_surface_emit():
        still = _cinderx.is_frame_evaluator_installed()
        if still:
            _cinderx.remove_frame_evaluator()
        from ci_pipeline.jit311 import report as _jit311_report
        snap = _jit311_report.snapshot()
        # Tests such as test_capi may uninstall the evaluator before
        # process exit. Completeness cares that shadow compile ran with
        # the evaluator live, which is recorded at install time.
        snap["evaluator_installed"] = _evaluator_installed_at_start
        snap["surface_module"] = os.environ.get("JIT311_SURFACE_MODULE")
        snap["surface_kind"] = os.environ.get("JIT311_SURFACE_KIND")
        compiled = []
        try:
            observe = _cinderx._get_observe_stats() or {}
        except Exception:
            observe = {}
        for event in observe.get("events") or []:
            if event.get("result") == "compiled":
                compiled.append({
                    "filename": event.get("filename"),
                    "qualname": event.get("qualname"),
                })
        snap["compiled_functions"] = compiled
        directory = os.environ.get("JIT311_WORKER_REPORT_DIR")
        if not directory:
            return
        path = os.path.join(directory, "%d.json" % os.getpid())
        with open(path, "w", encoding="utf8") as fp:
            json.dump(snap, fp)

    atexit.register(_jit311_surface_emit)
"""


def load_frozen_modules() -> list[str]:
    modules = load_libtest_target_manifest()
    if len(modules) != 440:
        raise SystemExit(
            f"frozen libtest surface must be 440 modules, got {len(modules)} "
            f"from {LIBTEST_TARGET_MANIFEST}"
        )
    return modules


def libtest_empty_ok() -> frozenset[str]:
    global _FROZEN_EMPTY_OK
    if _FROZEN_EMPTY_OK is None:
        names: list[str] = []
        for line in LIBTEST_EMPTY_OK_FILE.read_text(encoding="utf8").splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                names.append(line)
        _FROZEN_EMPTY_OK = frozenset(names)
    return _FROZEN_EMPTY_OK


def strip_surface_extras(snap: dict) -> dict:
    cleaned = dict(snap)
    for key in SURFACE_EXTRA_KEYS:
        cleaned.pop(key, None)
    return cleaned


def snapshot_errors(snap: dict) -> list[str]:
    """Judge one process snapshot. Missing cinderx children write nothing."""
    errors: list[str] = []
    cleaned = strip_surface_extras(snap)
    errors.extend(
        f"report schema: {err}" for err in _report.validate_schema(cleaned)
    )
    if snap.get("evaluator_installed") is not True:
        errors.append(
            "expected evaluator_installed is True, got "
            f"{snap.get('evaluator_installed')!r}"
        )
    if snap.get("events_dropped") != 0:
        errors.append(f"events_dropped={snap.get('events_dropped')!r}")
    if snap.get("supported_opcode_failures") != 0:
        errors.append(
            "supported_opcode_failures="
            f"{snap.get('supported_opcode_failures')!r}"
        )
    if snap.get("unknown_rejects") != 0:
        errors.append(f"unknown_rejects={snap.get('unknown_rejects')!r}")
    requested = snap.get("compile_requests")
    accounted = snap.get("compile_success", 0) + snap.get("compile_rejected", 0)
    if requested != accounted:
        errors.append(
            "compile requests are not fully accounted: "
            f"requests={requested!r}, success+rejected={accounted!r}"
        )
    for field in MACHINE_CODE_FIELDS:
        if snap.get(field) != 0:
            errors.append(f"{field}={snap.get(field)!r}")
    return errors


def _executed_case_count(cases: dict[str, str]) -> int:
    return sum(1 for state in cases.values() if state in EXECUTED_CASE_STATES)


def _norm_path(filename: object) -> str:
    return str(filename or "").replace("\\", "/")


def compiled_functions_of(record: dict) -> list[dict]:
    funcs: list[dict] = []
    for snap in record.get("snapshots") or []:
        funcs.extend(snap.get("compiled_functions") or [])
    return funcs


# Regrtest names whose executable Python is not test/{name}.py.
# Values are path suffixes (files) or directory prefixes (trailing /).
LIBTEST_EXTRA_COMPILE_PATHS: dict[str, tuple[str, ...]] = {
    "test_ctypes": ("/ctypes/test/",),
    "test_lib2to3": ("/lib2to3/tests/",),
    **{
        name: ("/test/multibytecodec_support.py",)
        for name in (
            "test_codecencodings_cn",
            "test_codecencodings_hk",
            "test_codecencodings_iso2022",
            "test_codecencodings_jp",
            "test_codecencodings_kr",
            "test_codecencodings_tw",
        )
    },
}

# 3.11 dual-run compiled 0 functions from these files: C-API one-shot
# (test_fileutils) and eval of a giant literal (test_longexp). Dual-run
# case identity is the completeness bar; argparse hits stay out.
LIBTEST_NO_OWN_PYTHON = frozenset({"test_fileutils", "test_longexp"})
LIBTEST_REQUIRED_CASES = {
    "test_fileutils": frozenset(
        {"test.test_fileutils.PathTests.test_capi_normalize_path"}
    ),
    "test_longexp": frozenset({"test.test_longexp.LongExpText.test_longexp"}),
}

# skip_unless_jit uses passAlways when is_enabled() is false. Shadow is
# kShadow, is_enabled() is only true for kRunning, so this suite's 20
# "pass" results are wrappers on both arms. Do not copy instrumentation
# env; mark the suite N/A for completeness.
SHADOW_N_A_SUITES = frozenset({"test_jit_support_instrumentation"})

LIBTEST_EMPTY_OK_FILE = (
    REPO_ROOT / "ci_pipeline" / "jit311" / "data" / "libtest_empty_ok.txt"
)
_FROZEN_EMPTY_OK: frozenset[str] | None = None


def _path_matches_extra(path: str, extra: str) -> bool:
    if extra.endswith("/"):
        return extra in path
    return path.endswith(extra)


def _path_is_libtest_target(path: str, name: str) -> bool:
    """Match a libtest target to its file or package, including dotted names.

    ``test_int`` lives at ``.../test/test_int.py``. Nested regrtest names
    such as ``test.test_asyncio.test_locks`` live at
    ``.../test/test_asyncio/test_locks.py``. Package tests such as
    ``test_json`` compile files under ``.../test/test_json/``. Basename
    equality would treat those as sitecustomize noise.
    """
    if not path or not name:
        return False
    rel = name.replace(".", "/")
    file_suffix = "/" + rel + ".py"
    if path.endswith(file_suffix) or path == rel + ".py":
        return True
    package = "/" + rel + "/"
    if package in path or path.endswith("/" + rel):
        return True
    return any(
        _path_matches_extra(path, extra)
        for extra in LIBTEST_EXTRA_COMPILE_PATHS.get(name, ())
    )


def compiled_belongs_to_target(
    filename: object, qualname: object, record: dict
) -> bool:
    """True when this compiled function is from the judged target.

    Process-global compile_success is not enough: sitecustomize or another
    module in the same interpreter can increment the counter. Identity is
    co_filename plus qualname belonging to the libtest module or test_cinderx
    suite under test.
    """
    del qualname  # reserved for a tighter qualname match if filenames collide
    path = _norm_path(filename)
    if not path:
        return False
    name = str(record.get("name") or "")
    kind = str(record.get("kind") or "")
    if kind == "libtest":
        return _path_is_libtest_target(path, name)
    if kind == "test_cinderx" or name == "test_cinderx":
        if "test_cinderx" not in path:
            return False
        if name in ("test_cinderx", "all_test_cinderx"):
            return True
        if path.endswith(f"/{name}.py") or f"/{name}/" in f"/{path}":
            return True
        return name in path.rsplit("/", 1)[-1]
    return False


def attributed_compile_success(record: dict) -> int:
    return sum(
        1
        for func in compiled_functions_of(record)
        if compiled_belongs_to_target(
            func.get("filename"), func.get("qualname"), record
        )
    )


def new_case_errors(record: dict) -> list[str]:
    """Reject any shadow/JIT-off mismatch on case set, status, verdict, rc.

    Crash records are already red from ``record_errors``. Extra shadow-only
    passes, skip->pass, and equal-rank verdict changes are completeness
    failures even when junit is non-empty.
    """
    name = record.get("name") or "<unnamed>"
    kind = record.get("kind") or "target"
    prefix = f"{kind} {name}"
    if record.get("verdict") == "crash":
        return []
    if "baseline_cases" not in record:
        return [f"{prefix}: missing JIT-off baseline_cases"]
    errors: list[str] = []
    baseline = record.get("baseline_cases") or {}
    shadow_cases = record.get("cases") or {}
    for key, prior in baseline.items():
        if key not in shadow_cases:
            errors.append(
                f"{prefix}: baseline case disappeared: {key} "
                f"(baseline={prior!r})"
            )
            continue
        state = shadow_cases[key]
        if state != prior:
            errors.append(f"{prefix}: case {key} {prior} -> {state}")
    for key, state in shadow_cases.items():
        if key not in baseline:
            errors.append(
                f"{prefix}: extra case {key} ({state!r}, baseline=None)"
            )
    base_verdict = record.get("baseline_verdict")
    verdict = record.get("verdict")
    if base_verdict is not None and verdict != base_verdict:
        errors.append(f"{prefix}: verdict {base_verdict} -> {verdict}")
    rc = record.get("returncode")
    base_rc = record.get("baseline_returncode")
    if base_rc is not None and rc != base_rc:
        errors.append(f"{prefix}: returncode {base_rc!r} -> {rc!r}")
    return errors


_FROZEN_CINDERX_SUITE_NAMES: tuple[str, ...] | None = None


def frozen_cinderx_suite_names() -> tuple[str, ...]:
    global _FROZEN_CINDERX_SUITE_NAMES
    if _FROZEN_CINDERX_SUITE_NAMES is None:
        _FROZEN_CINDERX_SUITE_NAMES = tuple(
            str(suite["name"]) for suite in load_test_cinderx_suites()
        )
    return _FROZEN_CINDERX_SUITE_NAMES


def apply_suite_env(env: dict[str, str], suite: dict) -> dict[str, str]:
    """Do not copy official runner env (LWF/OSR/instrumentation). 3.11
    cannot enable lightweight frames; keep allow_oss only.
    """
    merged = dict(env)
    if suite.get("allow_oss"):
        merged["CINDERX_TEST_ALLOW_OSS_IMPORTS"] = "1"
    return merged


def record_errors(record: dict) -> list[str]:
    """Completeness errors for one target (libtest module or test_cinderx)."""
    errors: list[str] = []
    name = record.get("name") or "<unnamed>"
    kind = record.get("kind") or "target"
    prefix = f"{kind} {name}"
    verdict = record.get("verdict")
    if verdict in ("crash", "no_result"):
        errors.append(f"{prefix}: verdict={verdict}")
    snaps = record.get("snapshots") or []
    if verdict not in ("skip",) and not snaps:
        errors.append(f"{prefix}: no worker snapshots")
    for snap in snaps:
        for err in snapshot_errors(snap):
            errors.append(f"{prefix}: {err}")
    compile_success = sum(int(s.get("compile_success") or 0) for s in snaps)
    executed = _executed_case_count(record.get("cases") or {})
    # Skip-only and collection-empty targets may never enter the target
    # file. Dual-run already flags a new empty junit against a passing
    # baseline. Require an attributed compile only when this target
    # actually ran tests; process-global argparse/typing hits stay out.
    needs_compile = executed > 0 and name not in LIBTEST_NO_OWN_PYTHON
    attributed = attributed_compile_success(record)
    if needs_compile and attributed <= 0:
        if compile_success <= 0:
            errors.append(
                f"{prefix}: compile_success={compile_success} "
                f"verdict={verdict} executed_cases={executed}"
            )
        else:
            errors.append(
                f"{prefix}: compile_success={compile_success} is not "
                "attributed to this target by co_filename+qualname "
                f"verdict={verdict} executed_cases={executed}"
            )
    cases = record.get("cases") or {}
    required = LIBTEST_REQUIRED_CASES.get(name, ())
    for case_id in required:
        if case_id not in cases:
            errors.append(f"{prefix}: missing required case {case_id}")
    if (
        not cases
        and not required
        and name not in libtest_empty_ok()
        and verdict not in ("skip", "crash")
    ):
        errors.append(f"{prefix}: empty junit")
    errors.extend(new_case_errors(record))
    return errors


def _cinderx_suite_errors(cinderx: dict) -> list[str]:
    errors: list[str] = []
    expected = list(frozen_cinderx_suite_names())
    suites = list(cinderx.get("suites") or [])
    got = [str(rec.get("name") or "") for rec in suites]
    if got != expected:
        errors.append(
            "test_cinderx: suite names "
            f"{got} != frozen {expected}"
        )
    for rec in suites:
        name = rec.get("name") or "<unnamed>"
        if name in SHADOW_N_A_SUITES:
            continue
        verdict = rec.get("verdict")
        if verdict in ("crash", "no_result"):
            errors.append(f"test_cinderx suite {name}: verdict={verdict}")
        case_count = int(rec.get("case_count") or 0)
        if case_count <= 0:
            errors.append(
                f"test_cinderx suite {name}: case_count={case_count}"
            )
        suite_rec = dict(rec)
        suite_rec["kind"] = "test_cinderx suite"
        suite_rec["name"] = name
        errors.extend(new_case_errors(suite_rec))
    return errors


def judge_completeness(
    report: dict,
    *,
    require_frozen: bool = True,
    require_cinderx: bool = True,
) -> list[str]:
    errors: list[str] = []
    modules = report.get("libtest_modules") or []
    attempted = [m["name"] for m in modules]
    if require_frozen:
        frozen = report.get("frozen_module_count")
        if frozen != 440:
            errors.append(f"frozen_module_count must be 440, got {frozen!r}")
        if len(attempted) != 440 or len(set(attempted)) != 440:
            errors.append(
                "libtest_modules must be the 440 unique frozen names, got "
                f"{len(attempted)}"
            )
    if report.get("libtest_worker_crashes"):
        errors.append(
            f"libtest_worker_crashes={report.get('libtest_worker_crashes')!r}"
        )
    for record in modules:
        errors.extend(record_errors(record))
    cinderx = report.get("test_cinderx")
    if require_cinderx and not cinderx:
        errors.append("test_cinderx completeness record is missing")
    elif cinderx:
        errors.extend(record_errors(cinderx))
        if require_cinderx:
            errors.extend(_cinderx_suite_errors(cinderx))
        rc = cinderx.get("returncode")
        if (
            rc not in (0, 1, 2, 5)
            and cinderx.get("verdict") not in ("skip", "fail", "crash")
        ):
            errors.append(f"test_cinderx: abnormal pytest exit {rc!r}")
    return errors


def _write_sitecustomize(out: Path) -> Path:
    startup = out / "startup"
    startup.mkdir(parents=True, exist_ok=True)
    path = startup / "sitecustomize.py"
    path.write_text(SURFACE_SITECUSTOMIZE, encoding="utf8")
    return startup


def _child_env(
    *,
    base: dict[str, str],
    startup: Path,
    reports: Path,
    module: str,
    kind: str,
    pythonpath_extra: list[str] | None = None,
    mode: str = "shadow",
) -> dict[str, str]:
    env = arm_environment(base)
    env.update(SHADOW_ENV if mode == "shadow" else JIT_OFF_ENV)
    env["PYTHONHASHSEED"] = "0"
    env["JIT311_REPO_ROOT"] = str(REPO_ROOT)
    env["JIT311_WORKER_REPORT_DIR"] = str(reports)
    env["JIT311_SURFACE_MODULE"] = module
    env["JIT311_SURFACE_KIND"] = kind
    try:
        import _cinderx
        env["JIT311_CINDERX_SITE"] = str(Path(_cinderx.__file__).resolve().parent)
    except ImportError:
        pass
    prepend = [str(startup), str(REPO_ROOT)]
    if pythonpath_extra:
        prepend.extend(pythonpath_extra)
    prev = env.get("PYTHONPATH", "")
    env["PYTHONPATH"] = os.pathsep.join(prepend + ([prev] if prev else []))
    return env


def _load_snapshots(reports: Path) -> list[dict]:
    snaps = []
    if not reports.is_dir():
        return snaps
    for path in sorted(reports.glob("*.json")):
        snaps.append(json.loads(path.read_text(encoding="utf8")))
    return snaps


def _fresh_dir(path: Path) -> Path:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)
    return path


def _module_work_dir(root: Path, name: str) -> Path:
    safe = name.replace("/", "_")
    work = _fresh_dir(root / safe)
    (work / "reports").mkdir()
    return work


def infer_libtest_verdict(
    name: str,
    log_text: str,
    returncode: int,
    cases: dict[str, str],
    timed_out: bool,
) -> str:
    """Module verdict for a one-module regrtest child.

    Multiprocess runs print ``[n/m] test_int passed``. Sequential 3.11.6
    prints only ``[1/1] test_int`` and puts the outcome in the epilogue, so
    falling back to that epilogue plus junit keeps a successful run from
    being recorded as no_result.
    """
    if timed_out:
        return "crash"
    parsed = parse_regrtest_modules(log_text, [name])
    if name in parsed:
        return parsed[name]
    completion = arm_run_completed(returncode, log_text)
    if completion:
        return "crash" if returncode not in (0, 2, 3) else "no_result"
    failed = sum(1 for state in cases.values() if state in ("failure", "error"))
    executed = _executed_case_count(cases)
    skipped = sum(1 for state in cases.values() if state == "skipped")
    if failed:
        return "fail"
    if executed:
        return "pass"
    if skipped and not executed:
        return "skip"
    if returncode == 0:
        return "pass"
    if returncode in (2, 3):
        return "fail"
    return "no_result"


def run_libtest_module(
    name: str,
    *,
    python: str,
    out: Path,
    startup: Path,
    timeout: int,
    base_env: dict[str, str],
    mode: str = "shadow",
) -> dict:
    work = _module_work_dir(out / f"libtest_{mode}", name)
    reports = work / "reports"
    junit = work / "junit.xml"
    log_path = work / "regrtest.log"
    env = _child_env(
        base=base_env,
        startup=startup,
        reports=reports,
        module=name,
        kind="libtest",
        mode=mode,
    )
    cmd = [
        python, "-u", "-m", "test",
        "--timeout", str(timeout),
        "--junit-xml", str(junit),
        name,
    ]
    started = time.time()
    try:
        with log_path.open("w", encoding="utf-8") as sink:
            proc = subprocess.run(
                cmd,
                env=env,
                text=True,
                stdout=sink,
                stderr=subprocess.STDOUT,
                timeout=timeout + 60,
            )
        returncode = proc.returncode
        timed_out = False
    except subprocess.TimeoutExpired:
        returncode = -1
        timed_out = True
        log_path.write_text(
            log_path.read_text(encoding="utf-8", errors="replace")
            + f"\n[shadow-surface] timeout after {timeout + 60}s\n",
            encoding="utf-8",
        )
    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    cases, _ = parse_junit(junit) if junit.is_file() else ({}, {})
    verdict = infer_libtest_verdict(
        name, log_text, returncode, cases, timed_out
    )
    snaps = _load_snapshots(reports)
    return {
        "kind": "libtest",
        "name": name,
        "verdict": verdict,
        "returncode": returncode,
        "timed_out": timed_out,
        "duration_s": round(time.time() - started, 1),
        "cases": cases,
        "snapshots": snaps,
        "compile_success": sum(int(s.get("compile_success") or 0) for s in snaps),
        "compile_requests": sum(int(s.get("compile_requests") or 0) for s in snaps),
        "unknown_rejects": sum(int(s.get("unknown_rejects") or 0) for s in snaps),
        "log": str(log_path),
    }


def load_test_cinderx_suites() -> list[dict]:
    """The official Kunpeng test_cinderx runner's suite list.

    Raw pytest over the whole tree collects compiler/static files that
    need `_static` and fail the process during collection. The runner's
    suites are the frozen execution surface for this completeness leg.
    """
    import importlib.util

    spec = importlib.util.spec_from_file_location(
        "jit311_test_cinderx_runner", TEST_CINDERX_RUNNER
    )
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot load {TEST_CINDERX_RUNNER}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    suites = list(getattr(mod, "SUITES", ()))
    if not suites:
        raise SystemExit("test_cinderx_runner exported no suites")
    return suites


def pytest_process_verdict(
    returncode: int, cases: dict[str, str], timed_out: bool
) -> str:
    if timed_out:
        return "crash"
    if returncode not in (0, 1, 2, 5):
        return "crash"
    if returncode == 5 and not cases:
        return "no_result"
    if any(state in ("failure", "error") for state in cases.values()):
        return "fail"
    if returncode in (1, 2):
        return "fail"
    if cases and all(state == "skipped" for state in cases.values()):
        return "skip"
    return "pass"


def _attach_baseline(shadow: dict, baseline: dict) -> dict:
    shadow["baseline_cases"] = baseline.get("cases") or {}
    shadow["baseline_returncode"] = baseline.get("returncode")
    shadow["baseline_verdict"] = baseline.get("verdict")
    base_suites = {
        rec.get("name"): rec for rec in (baseline.get("suites") or [])
    }
    for rec in shadow.get("suites") or []:
        prior = base_suites.get(rec.get("name")) or {}
        rec["baseline_case_count"] = prior.get("case_count")
        rec["baseline_returncode"] = prior.get("returncode")
        rec["baseline_verdict"] = prior.get("verdict")
        rec["baseline_cases"] = prior.get("cases") or {}
    return shadow


def _run_test_cinderx_pass(
    *,
    python: str,
    out: Path,
    startup: Path,
    timeout: int,
    base_env: dict[str, str],
    mode: str,
) -> dict:
    work = _fresh_dir(out / f"test_cinderx_{mode}")
    started = time.time()
    suites = load_test_cinderx_suites()
    all_cases: dict[str, str] = {}
    all_snaps: list[dict] = []
    suite_records: list[dict] = []
    remaining = max(60, timeout)
    timed_out_any = False
    worst_rc = 0

    for suite in suites:
        name = str(suite["name"])
        slice_timeout = min(1800, remaining)
        suite_dir = _fresh_dir(work / name)
        reports = suite_dir / "reports"
        reports.mkdir()
        junit = suite_dir / "junit.xml"
        log_path = suite_dir / "pytest.log"
        env = apply_suite_env(
            _child_env(
                base=base_env,
                startup=startup,
                reports=reports,
                module=f"test_cinderx.{name}",
                kind="test_cinderx",
                pythonpath_extra=[str(KNOWN_SKIP_PLUGIN_DIR)],
                mode=mode,
            ),
            suite,
        )
        cmd = [python, *suite["args"], "--junit-xml", str(junit)]
        print(
            f"shadow-surface: test_cinderx {mode} suite {name}",
            flush=True,
        )
        slice_started = time.time()
        try:
            with log_path.open("w", encoding="utf-8") as sink:
                sink.write("$ " + " ".join(cmd) + "\n\n")
                sink.flush()
                proc = subprocess.run(
                    cmd,
                    cwd=str(REPO_ROOT),
                    env=env,
                    text=True,
                    stdout=sink,
                    stderr=subprocess.STDOUT,
                    timeout=slice_timeout,
                )
            returncode = proc.returncode
            timed_out = False
        except subprocess.TimeoutExpired:
            returncode = -1
            timed_out = True
            timed_out_any = True
            with log_path.open("a", encoding="utf-8") as sink:
                sink.write(
                    f"\n[shadow-surface] timeout after {slice_timeout}s\n"
                )
        cases, _ = parse_junit(junit) if junit.is_file() else ({}, {})
        prefixed = {f"{name}::{key}": state for key, state in cases.items()}
        snaps = _load_snapshots(reports) if mode == "shadow" else []
        verdict = pytest_process_verdict(returncode, cases, timed_out)
        record = {
            "name": name,
            "verdict": verdict,
            "returncode": returncode,
            "timed_out": timed_out,
            "duration_s": round(time.time() - slice_started, 1),
            "case_count": len(cases),
            "cases": cases,
            "compile_success": sum(
                int(s.get("compile_success") or 0) for s in snaps
            ),
            "log": str(log_path),
        }
        suite_records.append(record)
        all_cases.update(prefixed)
        all_snaps.extend(snaps)
        if abs(returncode) > abs(worst_rc):
            worst_rc = returncode
        remaining = max(30, remaining - int(time.time() - slice_started))
        print(
            f"  {name}: {verdict} compile_success={record['compile_success']} "
            f"cases={len(cases)} rc={returncode}",
            flush=True,
        )

    if timed_out_any:
        overall = "crash"
    else:
        overall = pytest_process_verdict(worst_rc, all_cases, False)
        if any(rec["verdict"] == "crash" for rec in suite_records):
            overall = "crash"
        elif any(rec["verdict"] == "fail" for rec in suite_records):
            overall = "fail"
    return {
        "kind": "test_cinderx",
        "name": "test_cinderx",
        "verdict": overall,
        "returncode": worst_rc,
        "timed_out": timed_out_any,
        "duration_s": round(time.time() - started, 1),
        "cases": all_cases,
        "case_counts": {
            state: sum(1 for v in all_cases.values() if v == state)
            for state in ("pass", "failure", "error", "skipped")
        },
        "suites": suite_records,
        "snapshots": all_snaps,
        "compile_success": sum(
            int(s.get("compile_success") or 0) for s in all_snaps
        ),
        "compile_requests": sum(
            int(s.get("compile_requests") or 0) for s in all_snaps
        ),
        "unknown_rejects": sum(
            int(s.get("unknown_rejects") or 0) for s in all_snaps
        ),
        "log": str(work),
    }


def run_test_cinderx(
    *,
    python: str,
    out: Path,
    startup: Path,
    timeout: int,
    base_env: dict[str, str],
) -> dict:
    baseline = _run_test_cinderx_pass(
        python=python,
        out=out,
        startup=startup,
        timeout=timeout,
        base_env=base_env,
        mode="off",
    )
    shadow = _run_test_cinderx_pass(
        python=python,
        out=out,
        startup=startup,
        timeout=timeout,
        base_env=base_env,
        mode="shadow",
    )
    return _attach_baseline(shadow, baseline)


def run_surface(
    *,
    python: str | None = None,
    out: Path,
    jobs: int = 8,
    timeout: int = 1200,
    tests: list[str] | None = None,
    skip_libtest: bool = False,
    skip_cinderx: bool = False,
    cinderx_timeout: int = 7200,
) -> dict:
    python = python or sys.executable
    out.mkdir(parents=True, exist_ok=True)
    startup = _write_sitecustomize(out)
    base_env = dict(os.environ)
    subset = bool(tests)
    if tests:
        allowed = set(load_libtest_target_manifest())
        unknown = [name for name in tests if name not in allowed]
        if unknown:
            raise SystemExit(
                f"--tests names are not on the frozen surface: {unknown}"
            )
        targets = list(tests)
    else:
        targets = [] if skip_libtest else load_frozen_modules()

    modules: list[dict] = []
    started = time.time()
    if not skip_libtest:
        workers = max(1, jobs)

        def _run_libtest_pass(mode: str) -> list[dict]:
            print(
                f"shadow-surface: libtest {mode} {len(targets)} modules "
                f"jobs={workers} timeout={timeout}s",
                flush=True,
            )
            records: list[dict] = []
            with ThreadPoolExecutor(max_workers=workers) as pool:
                futs = {
                    pool.submit(
                        run_libtest_module,
                        name,
                        python=python,
                        out=out,
                        startup=startup,
                        timeout=timeout,
                        base_env=base_env,
                        mode=mode,
                    ): name
                    for name in targets
                }
                for fut in as_completed(futs):
                    record = fut.result()
                    records.append(record)
                    print(
                        f"  {record['name']}: {record['verdict']} "
                        f"compile_success={record['compile_success']} "
                        f"cases={len(record['cases'])} "
                        f"rc={record['returncode']}",
                        flush=True,
                    )
            records.sort(key=lambda rec: rec["name"])
            return records

        baseline_modules = _run_libtest_pass("off")
        modules = _run_libtest_pass("shadow")
        baseline_by_name = {rec["name"]: rec for rec in baseline_modules}
        for rec in modules:
            _attach_baseline(rec, baseline_by_name.get(rec["name"]) or {})

    cinderx_record = None
    if not skip_cinderx:
        print("shadow-surface: test_cinderx", flush=True)
        cinderx_record = run_test_cinderx(
            python=python,
            out=out,
            startup=startup,
            timeout=cinderx_timeout,
            base_env=base_env,
        )
        print(
            f"  test_cinderx: {cinderx_record['verdict']} "
            f"compile_success={cinderx_record['compile_success']} "
            f"cases={len(cinderx_record['cases'])} "
            f"rc={cinderx_record['returncode']}",
            flush=True,
        )

    report = {
        "frozen_module_count": 440 if not subset and not skip_libtest else len(modules),
        "libtest_modules": modules,
        "libtest_worker_crashes": sum(
            1 for rec in modules if rec["verdict"] in ("crash", "no_result")
        ),
        "test_cinderx": cinderx_record,
        "duration_s": round(time.time() - started, 1),
        "python": python,
        "jobs": jobs,
        "subset": subset,
        "skip_libtest": skip_libtest,
        "skip_cinderx": skip_cinderx,
    }
    return report


def write_report(report: dict, out: Path) -> Path:
    # Snapshots can be large; keep them in per-module files and emit a
    # compact completeness index plus the full document.
    compact_modules = []
    for rec in report.get("libtest_modules") or []:
        compact_modules.append(
            {
                "name": rec["name"],
                "verdict": rec["verdict"],
                "returncode": rec["returncode"],
                "compile_success": rec["compile_success"],
                "compile_requests": rec["compile_requests"],
                "unknown_rejects": rec["unknown_rejects"],
                "executed_cases": _executed_case_count(rec.get("cases") or {}),
                "case_count": len(rec.get("cases") or {}),
                "snapshot_count": len(rec.get("snapshots") or []),
            }
        )
    cinderx = report.get("test_cinderx")
    compact_cinderx = None
    if cinderx:
        compact_cinderx = {
            "name": cinderx["name"],
            "verdict": cinderx["verdict"],
            "returncode": cinderx["returncode"],
            "compile_success": cinderx["compile_success"],
            "compile_requests": cinderx["compile_requests"],
            "unknown_rejects": cinderx["unknown_rejects"],
            "executed_cases": _executed_case_count(cinderx.get("cases") or {}),
            "case_count": len(cinderx.get("cases") or {}),
            "case_counts": cinderx.get("case_counts"),
            "snapshot_count": len(cinderx.get("snapshots") or []),
            "suites": [
                {
                    "name": rec.get("name"),
                    "verdict": rec.get("verdict"),
                    "returncode": rec.get("returncode"),
                    "compile_success": rec.get("compile_success"),
                    "case_count": rec.get("case_count"),
                }
                for rec in (cinderx.get("suites") or [])
            ],
        }
    index = {
        "frozen_module_count": report.get("frozen_module_count"),
        "libtest_worker_crashes": report.get("libtest_worker_crashes"),
        "duration_s": report.get("duration_s"),
        "subset": report.get("subset"),
        "libtest_modules": compact_modules,
        "test_cinderx": compact_cinderx,
    }
    index_path = out / "completeness.json"
    full_path = out / "completeness.full.json"
    index_path.write_text(json.dumps(index, indent=1, sort_keys=True) + "\n")
    full_path.write_text(json.dumps(report, indent=1, sort_keys=True) + "\n")
    return index_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--out", required=True)
    parser.add_argument("--jobs", type=int, default=min(48, os.cpu_count() or 8))
    parser.add_argument("--timeout", type=int, default=1200)
    parser.add_argument("--cinderx-timeout", type=int, default=7200)
    parser.add_argument("--tests", nargs="*", help="libtest subset; not the frozen bar")
    parser.add_argument("--skip-libtest", action="store_true")
    parser.add_argument("--skip-cinderx", action="store_true")
    args = parser.parse_args(argv)
    out = Path(args.out)
    report = run_surface(
        python=args.python,
        out=out,
        jobs=args.jobs,
        timeout=args.timeout,
        tests=args.tests,
        skip_libtest=args.skip_libtest,
        skip_cinderx=args.skip_cinderx,
        cinderx_timeout=args.cinderx_timeout,
    )
    write_report(report, out)
    errors = judge_completeness(
        report,
        require_frozen=not args.tests and not args.skip_libtest,
        require_cinderx=not args.skip_cinderx,
    )
    for err in errors:
        print(f"shadow-surface FAIL: {err}", flush=True)
    if errors:
        print(
            f"shadow-surface: FAIL ({len(errors)} errors) -> {out / 'completeness.json'}",
            flush=True,
        )
        return 1
    print(f"shadow-surface: ok -> {out / 'completeness.json'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
