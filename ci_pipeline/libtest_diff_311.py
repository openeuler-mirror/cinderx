#!/usr/bin/env python3
"""CPython 3.11 Lib/test dual-mode differential gate.

Runs the stdlib test suite twice on the same interpreter -- arm A ("stock",
clean environment) and arm B (environment overrides, e.g. CinderX loaded with
JIT off) -- and compares per-module and per-case outcomes.  Only regressions
count: failures already present in arm A are absorbed as the environmental
baseline, so no skip list needs to be maintained.

The execution engine is regrtest itself (``python -m test -j N --junit-xml``),
which provides multiprocess scheduling and crash isolation natively on 3.11.

Subcommands:
  run   run one arm, write a normalized result JSON
  diff  compare two result JSONs, exit 1 on regressions
  gate  run arm A, run arm B, diff (the PR-gate entry point)

Examples:
  # Harness self-certification: stock vs stock must be empty.
  python3.11 libtest_diff_311.py gate --out /tmp/selfcert

  # CinderX JIT-off arm (env values owned by the runtime MRs).
  python3.11 libtest_diff_311.py gate --out /tmp/jitoff \\
      --env CINDERX_PLUGIN_ENABLE=1 --env CINDERX_EVAL_MODE=cinder \\
      --env CINDERX_JIT_DISABLE=1 --pythonpath-prepend /path/to/startup

  # Quick subset while iterating.
  python3.11 libtest_diff_311.py gate --out /tmp/q --tests test_int test_dict
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

CASE_STATES = ("pass", "failure", "error", "skipped")


def list_tests(python: str) -> list[str]:
    out = subprocess.run(
        [python, "-m", "test", "--list-tests"],
        check=True, capture_output=True, text=True,
    ).stdout
    return [line.strip() for line in out.splitlines() if line.strip()]


# regrtest's per-module result line, e.g.
#   0:00:01 load avg: 7.78 [131/440/19] test_extcall passed
#   0:00:00 load avg: 7.78 [  6/440/1] test.test_asyncio.test_base_events failed (uncaught exception)
RESULT_LINE = re.compile(
    r"\[ *\d+/\d+(?:/\d+)?\] (\S+)(?: process)? "
    r"(passed|failed|skipped|crashed)\b"
)


def parse_regrtest_modules(log_text: str, requested: list[str]) -> dict[str, str]:
    """Module verdicts from regrtest's own per-module result lines.

    junit is case-granular and silently loses doctest-only modules and
    modules whose test classes live in alias packages (datetimetester,
    ctypes.test, unittest.test, ...), so the module verdict comes from the
    runner itself: passed / failed (any qualifier, env changed included) /
    crashed / skipped (resource denied included).  A requested module with
    no result line is no_result -- its worker died before reporting -- and
    the diff treats that as a regression whenever it is not symmetric.
    """
    wanted = set(requested)
    verdicts: dict[str, str] = {}
    for match in RESULT_LINE.finditer(log_text):
        name, word = match.group(1), match.group(2)
        if name not in wanted:
            continue
        if word == "passed":
            verdicts[name] = "pass"
        elif word == "skipped":
            verdicts[name] = "skip"
        elif word == "crashed":
            # A crashed worker is never comparable evidence: the verdict
            # stays distinct so it can neither launder into "fail" (where a
            # symmetric crash would diff as baseline) nor count as a result.
            verdicts[name] = "crash"
        else:
            verdicts[name] = "fail"
    return verdicts


def crash_count(modules: dict[str, str]) -> int:
    return sum(1 for verdict in modules.values() if verdict == "crash")


# regrtest exit codes that still denote a COMPLETED run: 0 (all passed),
# 2 (some tests failed), 3 (environment changed).  Anything else --
# signals (negative), interrupt (130), internal error, no-tests-ran --
# means the main process did not finish normally, and per-module verdict
# lines cannot certify such a run.
ACCEPTED_REGRTEST_EXITS = (0, 2, 3)

# Harness-internal failures that regrtest reports WITHOUT killing the main
# process: a worker thread that dies after its module already printed a
# verdict still ends in a normal FAILURE epilogue with exit code 2, and
# per-module verdicts alone would certify the run (3.11.6
# libregrtest/runtest_mp.py).
FATAL_HARNESS_MARKERS = ("regrtest worker thread failed",)


def arm_run_completed(returncode: int, log_text: str) -> str | None:
    if returncode not in ACCEPTED_REGRTEST_EXITS:
        return (
            f"regrtest ended abnormally (exit {returncode}); per-module "
            f"verdicts cannot certify a run whose main process died"
        )
    if "== Tests result: " not in log_text:
        return (
            "regrtest log has no completion epilogue; the run did not "
            "finish normally"
        )
    for marker in FATAL_HARNESS_MARKERS:
        if marker in log_text:
            return (
                f"regrtest reported a harness-internal failure "
                f"({marker!r}); the run cannot be certified"
            )
    return None


# Startup file the gate provisions for the CinderX arm.  Every interpreter
# in the arm installs the evaluator and appends an attestation line, so
# "the evaluator was live during the differential" is recorded evidence
# rather than an assumption.
STARTUP_SITECUSTOMIZE = '''\
"""CinderX diffgate arm startup: install the evaluator, attest, fail loud."""
import os

try:
    import _cinderx
    import cinderx
except ModuleNotFoundError:
    # Interpreters spawned by tests (fresh venvs) have no cinderx and must
    # behave exactly like stock.
    pass
else:
    try:
        cinderx.init()
        _cinderx.install_frame_evaluator()
        installed = bool(_cinderx.is_frame_evaluator_installed())
    except Exception:
        # site.py would print this and keep going, silently degrading the
        # arm to stock and turning the differential into a false neutral.
        # Dying here makes the module no_result on one arm only -- red.
        import traceback

        traceback.print_exc()
        os._exit(78)
    if not installed:
        import sys

        print("cinderx evaluator not installed after install call",
              file=sys.stderr)
        os._exit(78)
    ledger = os.environ.get("CINDERX_EXECUTE_TRIGGER_LEDGER")
    if ledger:
        # The execute differential needs proof that this arm actually ran
        # machine code, not merely that the evaluator was installed.  Each
        # process records its own counter at exit; the gate sums them.
        import atexit

        def _process_role():
            # regrtest runs each test module in a worker spawned with
            # --worker-args '<json [ns, test_name]>'.  Summing the whole
            # process tree would let the scheduler process's own JIT stand
            # in for the workers that actually run the tests, so the role
            # and the module under test are recorded, not just the pid.
            import sys as _sys

            argv = _sys.argv
            for index, arg in enumerate(argv):
                if arg == "--worker-args" and index + 1 < len(argv):
                    try:
                        import json as _json

                        _ns, test_name = _json.loads(argv[index + 1])
                        return "worker", str(test_name)
                    except Exception:
                        return "worker", "?"
                if arg.startswith("--worker-args="):
                    return "worker", "?"
            # Everything else -- the regrtest scheduler, a test's own
            # subprocess -- is simply "not a worker".  The gate only needs
            # that distinction, and guessing finer labels from argv would
            # put a wrong one in the diagnostic.
            return "other", "-"

        def _own_compiled(test_name):
            # How many compiled functions came from the module this worker
            # was told to run.
            #
            # The process counters cannot answer that: they are totals, so
            # sitecustomize, the import machinery and regrtest's own
            # harness all count towards them, and a worker that compiled
            # nothing but harness code would still report "entered machine
            # code".  Matching the code objects' filename against the test
            # module's own file is what makes the claim about the module.
            try:
                import cinderjit as _jit
                import sys as _sys

                module = _sys.modules.get(test_name) or _sys.modules.get(
                    "test." + test_name
                )
                path = getattr(module, "__file__", None)
                if path is None:
                    return -1
                return sum(
                    1
                    for fn in _jit.get_compiled_functions()
                    if getattr(getattr(fn, "__code__", None), "co_filename", None)
                    == path
                )
            except Exception:
                return -1

        def _record_trigger_stats():
            try:
                stats = _cinderx._get_trigger_stats()
                role, test_name = _process_role()
                own = _own_compiled(test_name) if role == "worker" else -1
                with open(ledger, "a", encoding="utf-8") as fh:
                    fh.write(
                        "%d %s %s %d %d %d\\n"
                        % (
                            os.getpid(),
                            role,
                            test_name,
                            stats["machine_code_entries"],
                            stats["compiled_function_creations"],
                            own,
                        )
                    )
            except Exception:
                pass

        atexit.register(_record_trigger_stats)
    attest = os.environ.get("CINDERX_DIFF_ATTEST")
    if attest:
        try:
            with open(attest, "a", encoding="utf-8") as fh:
                fh.write(f"{os.getpid()} {installed}\\n")
        except OSError:
            # A test child running under a dropped-privilege user may not be
            # able to sign the ledger.  The evaluator *is* installed -- the
            # only thing worth dying for -- and the parent processes carry
            # the attestation proof.
            pass
'''


def read_attest(path: Path) -> tuple[int, bool]:
    """Number of attested processes and whether every one was installed."""
    if not path.is_file():
        return 0, True
    rows = [line.split() for line in path.read_text().splitlines() if line.strip()]
    return len(rows), all(len(row) == 2 and row[1] == "True" for row in rows)


def missing_verdicts(modules: dict[str, str]) -> int:
    """Requested modules that produced no verdict at all.

    A healthy arm reports a verdict for every requested module -- pass,
    fail or skip.  Anything without one means a worker died or regrtest
    crashed mid-flight, and the arm must fail loudly: if both arms crashed
    the same way, the per-module diff would compare no_result against
    no_result and wave a broken run through as a false green.  regrtest's
    exit code cannot serve here, because baseline failures already make it
    non-zero on a perfectly healthy run.
    """
    return sum(1 for verdict in modules.values() if verdict == "no_result")


def case_state(tc: ET.Element) -> str:
    for child in tc:
        tag = child.tag.rsplit("}", 1)[-1]
        if tag in ("failure", "error", "skipped"):
            return tag
    return "pass"


# Everything in a diagnostic that legitimately differs between two runs of
# the same interpreter.  Addresses, temporary paths, pids and ports say
# nothing about behaviour, and leaving them in would make every failing
# case an asymmetry.
_DIAGNOSTIC_NOISE = (
    (re.compile(r"0x[0-9a-fA-F]+"), "0xADDR"),
    (re.compile(r"/(?:tmp|var/folders)/[^\s'\"]*"), "<tmp>"),
    (re.compile(r"\b(?:pid|port)[= ]\d+", re.IGNORECASE), "<num>"),
    (re.compile(r"\bat 0xADDR\b"), "at 0xADDR"),
)


def normalize_diagnostic(text: str) -> str:
    for pattern, replacement in _DIAGNOSTIC_NOISE:
        text = pattern.sub(replacement, text)
    return " ".join(text.split())


def case_diagnostic(tc: ET.Element) -> str | None:
    """How a case failed, normalized, or None when it did not.

    The state tag alone answers "did it fail", not "did it fail the same
    way".  Two runs that both raise TypeError for entirely different
    reasons are both `failure`, and a comparator that stops at the tag
    calls them identical -- while the correctness contract this gate
    exists for names the exception type and message explicitly.

    The junit `message` attribute carries the exception's own text; the
    element body carries the traceback, whose file paths and line numbers
    are not part of the contract and are deliberately left out.
    """
    for child in tc:
        tag = child.tag.rsplit("}", 1)[-1]
        if tag in ("failure", "error"):
            kind = child.get("type") or tag
            message = child.get("message") or ""
            return normalize_diagnostic(f"{kind}: {message}")
        if tag == "skipped":
            # A skip reason is a behavioural fact too: skipping for a
            # different reason is a different outcome.
            return normalize_diagnostic(f"skipped: {child.get('message') or ''}")
    return None


def _normalize(name: str) -> str:
    return name[5:] if name.startswith("test.") else name


JUNIT_MODULE_ALIASES = (
    ("unittest.test.testmock", "test_unittest"),
    ("ctypes.test", "test_ctypes"),
    ("datetimetester", "test_datetime"),
    ("test_profile", "test_cprofile"),
    ("builtins", "test_builtin"),
    ("enum", "test_enum"),
)


def make_module_resolver(requested: list[str]):
    """Map a junit classname to the requested test name it belongs to.

    ``--list-tests`` mixes top-level names (test_grammar) with package paths
    (test.test_asyncio.test_events); junit classnames are full dotted paths.
    Longest-prefix match on dot boundaries after stripping the "test." prefix
    keys ordinary cases back to their requested name.  That match must run
    before the alias table: both test_profile and test_cprofile are frozen
    targets, so an unconditional alias would steal test_profile's own cases.
    CPython also reuses test classes from alias modules (for example
    test_cprofile runs classes defined in test_profile), so aliases remain a
    fallback when the source module was not requested independently.
    """
    normalized = sorted((_normalize(r), r) for r in requested)
    by_length = sorted(normalized, key=lambda nr: len(nr[0]), reverse=True)
    cache: dict[str, str] = {}

    def resolve(classname: str) -> str:
        cls = _normalize(classname)
        if cls in cache:
            return cache[cls]
        for norm, req in by_length:
            if cls == norm or cls.startswith(norm + "."):
                cache[cls] = req
                return req
        for prefix, req in JUNIT_MODULE_ALIASES:
            if req in requested and (
                cls == prefix or cls.startswith(prefix + ".")
            ):
                cache[cls] = req
                return req
        for part in cls.split("."):
            if part.startswith("test_"):
                cache[cls] = part
                return part
        cache[cls] = cls or "unknown"
        return cache[cls]

    return resolve


def parse_junit(path: Path) -> tuple[dict[str, str], dict[str, str]]:
    """Per-case states, and per-case diagnostics for the ones that did not pass.

    Two maps rather than one record: the JIT-off differential compares
    states and only states, and its contract does not change because the
    execute differential wants more.
    """
    cases: dict[str, str] = {}
    diagnostics: dict[str, str] = {}
    root = ET.parse(path).getroot()
    for ts in root.iter():
        if ts.tag.rsplit("}", 1)[-1] != "testcase":
            continue
        classname = ts.get("classname") or ""
        name = ts.get("name") or ""
        # regrtest's junit output puts the full dotted path in `name` and
        # leaves `classname` empty; joining blindly would poison keys with a
        # leading dot.
        key = f"{classname}.{name}" if classname else name
        cases[key] = case_state(ts)
        diagnostic = case_diagnostic(ts)
        if diagnostic is not None:
            diagnostics[key] = diagnostic
    return cases, diagnostics


SANITIZED_ENV_KEYS = (
    "PYTHONPATH", "PYTHONSTARTUP", "PYTHONHOME", "PYTHONHASHSEED",
)
SANITIZED_ENV_PREFIXES = ("CINDERX_", "PYTHONJIT", "PARALLEL_GC_")


def arm_environment(base: dict) -> dict:
    """Arms start from a sanitized environment: an inherited PYTHONPATH,
    sitecustomize hook, or stray CINDERX_* variable could activate the
    evaluator (or anything else) in BOTH arms and manufacture a false
    neutral.  Each arm re-adds exactly what it declares."""
    env = {
        key: value
        for key, value in base.items()
        if key not in SANITIZED_ENV_KEYS
        and not key.startswith(SANITIZED_ENV_PREFIXES)
    }
    # Unconditional: an inherited PYTHONHASHSEED=random would make the two
    # arms diverge for reasons that have nothing to do with the evaluator.
    env["PYTHONHASHSEED"] = "0"
    # Some minimal CPython installations omit Lib/test while the matching
    # test support tree lives beside the explicitly selected interpreter.
    # These two scheduler-owned paths are control-plane inputs, not CinderX
    # activation knobs, and both differential arms must see the same tree.
    test_support = [
        base.get("CINDERX_TEST_PYTHON_STDLIB_DIR", "").strip(),
        base.get("CINDERX_TEST_PYTHON_EXTENSIONS_DIR", "").strip(),
    ]
    configured = os.pathsep.join(path for path in test_support if path)
    if configured:
        env["PYTHONPATH"] = configured
    return env


def run_arm(args: argparse.Namespace) -> int:
    python = args.python
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    junit = out / "junit.xml"

    env = arm_environment(dict(os.environ))
    for kv in args.env or []:
        key, _, value = kv.partition("=")
        env[key] = value
    if args.pythonpath_prepend:
        prev = env.get("PYTHONPATH", "")
        env["PYTHONPATH"] = os.pathsep.join(args.pythonpath_prepend + ([prev] if prev else []))

    attest_path = (
        Path(args.attest_file) if getattr(args, "attest_file", None) else None
    )
    if attest_path is not None:
        attest_path.parent.mkdir(parents=True, exist_ok=True)
        attest_path.unlink(missing_ok=True)
        # World-writable so even test children that drop privileges can
        # sign the ledger.
        attest_path.touch()
        os.chmod(attest_path, 0o666)
        env["CINDERX_DIFF_ATTEST"] = str(attest_path)

    tests = args.tests or load_target_manifest()
    excluded = set(args.exclude or [])
    tests = [t for t in tests if t not in excluded]
    cmd = [
        python, "-u", "-m", "test",
        "-j", str(args.jobs),
        "--timeout", str(args.timeout),
        "--junit-xml", str(junit),
        *tests,
    ]
    started = time.time()
    # Stream regrtest's output straight to disk so a running arm can be
    # watched with tail -f -- which is exactly what diagnosing a hanging
    # test module needs.  The child runs with -u, so lines land live.
    with (out / "regrtest.log").open("w", encoding="utf-8") as sink:
        proc = subprocess.run(
            cmd, env=env, text=True, stdout=sink, stderr=subprocess.STDOUT
        )

    cases, diagnostics = parse_junit(junit) if junit.is_file() else ({}, {})
    log_text = (out / "regrtest.log").read_text(errors="replace")
    verdicts = parse_regrtest_modules(log_text, tests)
    modules: dict[str, str] = {name: verdicts.get(name, "no_result") for name in tests}

    attest_count, attest_ok = (
        read_attest(attest_path) if attest_path is not None else (0, True)
    )

    result = {
        "meta": {
            "python": python,
            "argv_tests": len(tests),
            "jobs": args.jobs,
            "env_overrides": args.env or [],
            "duration_s": round(time.time() - started, 1),
            "regrtest_exit": proc.returncode,
            "pythonpath_prepend": list(args.pythonpath_prepend or []),
            "attest_processes": attest_count,
            "attest_all_installed": attest_ok,
        },
        "modules": modules,
        "cases": cases,
        "diagnostics": diagnostics,
    }
    (out / "result.json").write_text(json.dumps(result, indent=1, sort_keys=True))
    completion_error = arm_run_completed(proc.returncode, log_text)
    if completion_error:
        print(f"arm FAILED: {completion_error}")
        return 7
    counts = {
        s: sum(1 for v in modules.values() if v == s)
        for s in ("pass", "fail", "skip", "crash", "no_result")
    }
    print(f"arm done: {counts} duration={result['meta']['duration_s']}s -> {out / 'result.json'}")
    crashes = crash_count(modules)
    if crashes:
        print(
            f"arm FAILED: {crashes} module worker(s) crashed; a crash is "
            f"never comparable evidence, symmetric or not"
        )
        return 6
    gaps = missing_verdicts(modules)
    if gaps:
        print(
            f"arm FAILED: {gaps} modules produced no verdict (worker death "
            f"or a harness crash); regrtest exit {proc.returncode}"
        )
        return 5
    if attest_path is not None:
        print(f"arm attest: {attest_count} processes, all installed: {attest_ok}")
        # Fewer than two records means not even the regrtest main process
        # plus one worker attested: the arm cannot have been live.
        if attest_count < 2 or not attest_ok:
            print("arm attest FAILED: the evaluator was not verifiably live")
            return 4
    return 0


def load(path: str) -> dict:
    return json.loads(Path(path).read_text())


TARGET_MANIFEST = (
    Path(__file__).resolve().parent
    / "jit311" / "data" / "libtest_target_modules.txt"
)


def load_target_manifest() -> list[str]:
    """The frozen stdlib target surface.  The gate runs exactly this list:
    a dynamically discovered surface would shrink symmetrically with the
    environment and no one would notice."""
    if not TARGET_MANIFEST.is_file():
        raise SystemExit(f"target-module manifest missing: {TARGET_MANIFEST}")
    modules = [
        line.strip()
        for line in TARGET_MANIFEST.read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if len(modules) != len(set(modules)):
        raise SystemExit("target-module manifest contains duplicates")
    return modules


def diff_results(a: dict, b: dict) -> dict:
    regressions: dict[str, dict[str, str]] = {}
    warnings: dict[str, dict[str, str]] = {}

    for mod, averdict in a["modules"].items():
        bverdict = b["modules"].get(mod, "missing")
        if averdict == bverdict:
            continue
        entry = {"stock": averdict, "cinderx": bverdict}
        # Regression: anything that was passing and no longer is -- a module
        # that starts *skipping* under the evaluator changed behavior too.
        if averdict == "pass" and bverdict in (
            "fail", "skip", "crash", "no_result", "missing"
        ):
            regressions[mod] = entry
        else:
            warnings[mod] = entry

    case_regressions: dict[str, dict[str, str]] = {}
    bcases = b.get("cases", {})
    bmodules = b.get("modules", {})
    resolve = make_module_resolver(list(a.get("modules", {})))
    for key, astate in a.get("cases", {}).items():
        if astate != "pass":
            continue
        bstate = bcases.get(key, "missing")
        if bstate in ("failure", "error", "skipped"):
            # A case that starts skipping under the evaluator shrank
            # coverage while staying green -- that is a regression here,
            # same as the module-level pass->skip rule above.
            case_regressions[key] = {"stock": astate, "cinderx": bstate}
        elif bstate == "missing":
            # A case that vanished from a module which still reported is a
            # regression in its own right.  When the whole module produced no
            # result, the module-level entry already carries that signal and
            # repeating it for every case would bury it.
            if bmodules.get(resolve(key)) not in ("no_result", "missing", "skip"):
                case_regressions[key] = {"stock": astate, "cinderx": bstate}

    return {
        "module_regressions": regressions,
        "module_warnings": warnings,
        "case_regressions": case_regressions,
    }


EXECUTE_JIT_COVERAGE = (
    Path(__file__).parent / "jit311" / "data" / "execute_jit_coverage.txt"
)


def load_execute_jit_coverage(path: Path) -> set[str]:
    """Target modules that must each compile a function of their own."""
    if not path.is_file():
        return set()
    return {
        line.strip()
        for line in path.read_text().splitlines()
        if line.strip() and not line.startswith("#")
    }


EXECUTE_BASELINE = (
    Path(__file__).parent / "jit311" / "data" / "execute_diff_baseline.json"
)


def load_execute_baseline(path: Path) -> dict:
    if not path.exists():
        return {}
    doc = json.loads(path.read_text())
    return doc.get("asymmetries", {})


def diff_results_symmetric(a: dict, b: dict, allowed: dict) -> dict:
    """Compare two arms for isomorphism, not for regressions.

    diff_results() answers "did anything that used to pass stop passing".
    That is the right question for the JIT-off gate, where the two arms are
    the same interpreter and an improvement is not a defect.  It is the
    wrong question here: under Auto-JIT a case that FAILS in stock and
    PASSES under execute, a case that changes how it fails, and a case that
    only one arm ran at all are each a semantic difference the execute mode
    is supposed not to produce, and each is invisible to a comparator that
    only walks stock's passing cases.

    So this one requires the two result sets to be identical -- same case
    identities, same state for each -- and carries a frozen baseline for
    the asymmetries that are genuinely environmental.  A baseline entry
    that no longer reproduces is reported too: an allowance nobody can
    justify any more is how a gate stops gating.
    """
    differences: dict[str, dict[str, str]] = {}

    for mod in sorted(set(a.get("modules", {})) | set(b.get("modules", {}))):
        averdict = a.get("modules", {}).get(mod, "missing")
        bverdict = b.get("modules", {}).get(mod, "missing")
        if averdict != bverdict:
            differences[f"<module> {mod}"] = {
                "stock": averdict,
                "execute": bverdict,
            }

    acases, bcases = a.get("cases", {}), b.get("cases", {})
    for key in sorted(set(acases) | set(bcases)):
        astate = acases.get(key, "missing")
        bstate = bcases.get(key, "missing")
        if astate != bstate:
            differences[key] = {"stock": astate, "execute": bstate}
            # The states already differ; also reporting the diagnostic
            # would say the same thing twice.
            continue
        if astate == "pass":
            continue
        adiag = a.get("diagnostics", {}).get(key, "")
        bdiag = b.get("diagnostics", {}).get(key, "")
        if adiag != bdiag:
            # Same verdict, different reason.  Two TypeErrors raised for
            # unrelated reasons are both `failure`; the contract names the
            # exception type and message, so the tag alone is not enough.
            differences[f"<diagnostic> {key}"] = {
                "stock": adiag,
                "execute": bdiag,
            }

    unexpected = {
        key: value
        for key, value in differences.items()
        if allowed.get(key) != value
    }
    stale = {
        key: value
        for key, value in allowed.items()
        if differences.get(key) != value
    }
    return {
        "differences": differences,
        "unexpected": unexpected,
        "stale_baseline": stale,
        "allowed": len(allowed),
    }


def cmd_diff(args: argparse.Namespace) -> int:
    report = diff_results(load(args.a), load(args.b))
    out = Path(args.report) if args.report else None
    text = json.dumps(report, indent=1, sort_keys=True)
    if out:
        out.write_text(text)
    print(text if len(text) < 8000 else text[:8000] + "\n... (truncated, see report file)")
    red = report["module_regressions"] or report["case_regressions"]
    print(f"DIFF: {len(report['module_regressions'])} module regressions, "
          f"{len(report['case_regressions'])} case regressions, "
          f"{len(report['module_warnings'])} warnings")
    return 1 if red else 0


STOCK_SITECUSTOMIZE = '''\
"""Stock control arm startup: prove cinderx never activated here."""
import atexit
import os
import sys


def _attest_stock_purity():
    path = os.environ.get("CINDERX_STOCK_ATTEST")
    if not path:
        return
    polluted = [
        name for name in ("cinderx", "_cinderx", "cinderjit")
        if name in sys.modules
    ]
    with open(path, "a", encoding="utf8") as fp:
        fp.write(("POLLUTED:" + ",".join(polluted)) if polluted else "clean")
        fp.write("\\n")


atexit.register(_attest_stock_purity)
'''


def read_stock_attest(path: Path) -> tuple[int, bool]:
    if not path.is_file():
        return 0, False
    lines = [ln for ln in path.read_text().splitlines() if ln]
    return len(lines), bool(lines) and all(ln == "clean" for ln in lines)


def provision_dual_arms(
    args,
    out: Path,
    *,
    label: str,
    arm_dir: str,
    shared: dict,
    cinderx_env: list[str],
    precreate: list[Path] = (),
):
    """Provision both arms' startups and attestation files, run them, and
    enforce stock purity.

    The stock arm is a CONTROL with its own startup whose only job is to
    attest, per process, that no cinderx module ever loaded; a polluted
    control (external sitecustomize, inherited environment) would fake
    neutrality symmetrically.  Returns (rc, attest_path); rc is nonzero
    when an arm failed or the control was not pure.
    """
    startup = out / "startup"
    startup.mkdir(parents=True, exist_ok=True)
    (startup / "sitecustomize.py").write_text(STARTUP_SITECUSTOMIZE)
    attest = out / arm_dir / "attest.log"
    attest.parent.mkdir(parents=True, exist_ok=True)
    for path in precreate:
        path.unlink(missing_ok=True)
        path.touch()
        os.chmod(path, 0o666)

    stock_startup = out / "startup-stock"
    stock_startup.mkdir(parents=True, exist_ok=True)
    (stock_startup / "sitecustomize.py").write_text(STOCK_SITECUSTOMIZE)
    stock_attest = out / "stock" / "attest-stock.log"
    stock_attest.parent.mkdir(parents=True, exist_ok=True)
    stock_attest.unlink(missing_ok=True)
    stock_attest.touch()
    os.chmod(stock_attest, 0o666)

    a = argparse.Namespace(**{
        **shared,
        "out": str(out / "stock"),
        "env": [f"CINDERX_STOCK_ATTEST={stock_attest}"],
        "pythonpath_prepend": [str(stock_startup)],
        "attest_file": None,
    })
    b = argparse.Namespace(**{
        **shared,
        "out": str(out / arm_dir),
        **({} if cinderx_env is None else {"env": cinderx_env}),
        "pythonpath_prepend": [str(startup)] + list(args.pythonpath_prepend or []),
        "attest_file": str(attest),
    })
    if run_arm(a) or run_arm(b):
        return 2, attest

    # Structural stock-arm purity: it has no evaluator startup, so an
    # evaluator attestation there means the arms were misrouted.
    stray = out / "stock" / "attest.log"
    if stray.exists():
        print(f"{label}: stock arm unexpectedly attested an evaluator: {stray}")
        return 3, attest
    # Positive purity: fewer than two records means not even the regrtest
    # main process plus one worker reported.
    stock_count, stock_clean = read_stock_attest(stock_attest)
    if stock_count < 2 or not stock_clean:
        print(
            f"{label}: stock arm purity not attested "
            f"({stock_count} records, clean={stock_clean}): {stock_attest}"
        )
        return 3, attest
    return 0, attest


def cmd_gate(args: argparse.Namespace) -> int:
    out = Path(args.out)
    if not args.tests and args.exclude:
        # On the frozen surface the committed manifest is the only way to
        # shrink coverage; an --exclude here would mint a strictly valid
        # but smaller report.
        print("GATE: --exclude is not allowed on the frozen surface; "
              "edit the target-module manifest deliberately")
        return 2
    out.mkdir(parents=True, exist_ok=True)

    rc, _attest = provision_dual_arms(
        args,
        out,
        label="GATE",
        arm_dir="cinderx",
        shared=vars(args),
        cinderx_env=None,
    )
    if rc:
        return rc

    args2 = argparse.Namespace(a=str(out / "stock" / "result.json"),
                               b=str(out / "cinderx" / "result.json"),
                               report=str(out / "diff.json"))
    rc = cmd_diff(args2)

    # Trigger-proof bookkeeping: this leg IS the target-module surface of
    # the unified report (dev plan MR-01).  On the frozen surface it must
    # publish the attempted count, the count must equal the committed
    # manifest, and a publication failure is red -- a silently absent
    # field would let the aggregator downstream go hungry unnoticed.
    # Explicit --tests slices are a development tool and do not publish.
    if not args.tests:
        try:
            cinderx_result = json.loads(
                (out / "cinderx" / "result.json").read_text()
            )
            attempted = len(cinderx_result["modules"])
            frozen = len(load_target_manifest())
            if attempted != frozen:
                print(
                    f"GATE: attempted {attempted} modules but the frozen "
                    f"surface is {frozen}; the target surface shrank"
                )
                return 4
            (out / "trigger_report_fields.json").write_text(
                json.dumps(
                    {
                        "target_modules_attempted": attempted,
                        # Both arms returned: any crashed worker already
                        # failed the arm (verdict "crash", return 6), so
                        # this is an attested zero over the frozen
                        # surface, not a default.
                        "worker_crashes": 0,
                    }
                )
                + "\n"
            )
        except SystemExit:
            raise
        except Exception as exc:  # noqa: BLE001
            print(f"GATE: could not publish target_modules_attempted: {exc}")
            return 4
    return rc


def load_stdlib72_modules() -> list[str]:
    """The 72-module confirmed surface, from its single definition."""
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from ci_pipeline.jit311.runners import STDLIB_SHADOW_MODULES

    modules = list(STDLIB_SHADOW_MODULES)
    if len(modules) != 72:
        raise SystemExit(
            f"the confirmed stdlib surface is {len(modules)} modules, not 72"
        )
    return modules


def reuse_stock_result(
    stock_dir: Path, out: Path, modules: list[str], python: str
) -> dict:
    """Extract the execute surface from a completed frozen stock arm."""
    result_path = stock_dir / "result.json"
    attest_path = stock_dir / "attest-stock.log"
    try:
        result = load(str(result_path))
    except (OSError, ValueError) as exc:
        raise ValueError(f"cannot load reusable stock result {result_path}: {exc}")

    expected = set(load_target_manifest())
    actual = set(result.get("modules", {}))
    if actual != expected:
        raise ValueError(
            "reusable stock result is not the frozen surface: "
            f"expected {len(expected)} modules, got {len(actual)}"
        )
    missing = set(modules) - actual
    if missing:
        raise ValueError(
            f"reusable stock result is missing execute modules: {sorted(missing)}"
        )
    recorded_python = result.get("meta", {}).get("python")
    if recorded_python != python:
        raise ValueError(
            f"reusable stock result used {recorded_python!r}, expected {python!r}"
        )
    attest_count, attest_clean = read_stock_attest(attest_path)
    if attest_count < 2 or not attest_clean:
        raise ValueError(
            "reusable stock arm purity not attested "
            f"({attest_count} records, clean={attest_clean}): {attest_path}"
        )

    resolve = make_module_resolver(list(result["modules"]))
    wanted = set(modules)
    subset = {
        "meta": {
            **result["meta"],
            "original_argv_tests": result["meta"].get("argv_tests"),
            "argv_tests": len(modules),
            "reused_from": str(result_path),
            "stock_attest_processes": attest_count,
        },
        "modules": {name: result["modules"][name] for name in modules},
        "cases": {
            key: state
            for key, state in result.get("cases", {}).items()
            if resolve(key) in wanted
        },
        "diagnostics": {
            key: diagnostic
            for key, diagnostic in result.get("diagnostics", {}).items()
            if resolve(key) in wanted
        },
    }
    target = out / "stock"
    target.mkdir(parents=True, exist_ok=True)
    (target / "result.json").write_text(
        json.dumps(subset, indent=1, sort_keys=True) + "\n"
    )
    print(
        f"EXECUTE-GATE: reused {len(modules)}-module stock baseline from "
        f"{result_path}; no second stock arm was run"
    )
    return subset


def read_trigger_ledger(path: Path) -> dict:
    """Per-role trigger evidence for one arm.

    Rows are "pid role test entries creations".  The roles are kept apart
    on purpose: regrtest's scheduler process runs plenty of Python of its
    own, so a tree-wide sum would let its JIT activity stand in for the
    workers that actually execute the test modules -- exactly the "identical
    for the wrong reason" pass this leg exists to refuse.
    """
    empty = {
        "rows": 0,
        "workers": 0,
        "worker_entries": 0,
        "worker_creations": 0,
        "worker_tests": set(),
        "executing_tests": set(),
        "compiling_tests": set(),
        "unattributed_tests": set(),
    }
    if not path.is_file():
        return empty
    rows = [line.split() for line in path.read_text().splitlines() if line.strip()]
    rows = [row for row in rows if len(row) == 6]
    workers = [row for row in rows if row[1] == "worker"]
    return {
        "rows": len(rows),
        "workers": len(workers),
        "worker_entries": sum(int(row[3]) for row in workers),
        "worker_creations": sum(int(row[4]) for row in workers),
        "worker_tests": {row[2] for row in workers},
        # Workers whose process counters moved.  Process-wide, so this
        # says the worker ran machine code, not that the module did.
        "executing_tests": {row[2] for row in workers if int(row[3]) > 0},
        # Workers that compiled a function defined in the module they were
        # told to run.  This is the attributed claim.
        "compiling_tests": {row[2] for row in workers if int(row[5]) > 0},
        # Workers that could not be attributed at all -- the module was
        # not importable by name at exit, so the count is unknown rather
        # than zero.  Kept separate so an unknown never reads as evidence.
        "unattributed_tests": {row[2] for row in workers if int(row[5]) < 0},
    }


def cmd_off_gate(args: argparse.Namespace) -> int:
    """Run stock and evaluator-off once each over the frozen 440."""
    out = Path(args.out)
    if not args.tests and args.exclude:
        print(
            "OFF-GATE: --exclude is not allowed on the frozen surface; "
            "edit the target-module manifest deliberately"
        )
        return 2
    out.mkdir(parents=True, exist_ok=True)

    rc, _ = provision_dual_arms(
        args,
        out,
        label="OFF-GATE evaluator-off",
        arm_dir="evaluator-off",
        shared=vars(args),
        cinderx_env=[
            "CINDERX_PLUGIN_ENABLE=1",
            "CINDERX_EVAL_MODE=cinder",
            "CINDERX_JIT_MODE=off",
        ],
    )
    if rc:
        return rc

    stock = load(str(out / "stock" / "result.json"))
    evaluator_off = load(str(out / "evaluator-off" / "result.json"))
    stock_off_diff = diff_results(stock, evaluator_off)
    (out / "stock-vs-evaluator-off.json").write_text(
        json.dumps(stock_off_diff, indent=1, sort_keys=True) + "\n"
    )

    expected = set(args.tests or load_target_manifest())
    diff_errors = bool(
        stock_off_diff["module_regressions"]
        or stock_off_diff["case_regressions"]
    )
    report = {
        "arms": {
            "stock": stock["meta"],
            "evaluator_off": evaluator_off["meta"],
        },
        "frozen_module_count": len(expected),
        "worker_crashes": 0,
        "diffs": {
            "stock_vs_evaluator_off": "stock-vs-evaluator-off.json",
        },
    }
    (out / "off-gate-report.json").write_text(
        json.dumps(report, indent=1, sort_keys=True) + "\n"
    )
    (out / "trigger_report_fields.json").write_text(
        json.dumps(
            {
                "target_modules_attempted": len(expected),
                "worker_crashes": 0,
            },
            sort_keys=True,
        )
        + "\n"
    )
    print("OFF-GATE: stock/evaluator-off each ran once")
    return 1 if diff_errors else 0


def cmd_execute_gate(args: argparse.Namespace) -> int:
    """Run the 72-module confirmed surface under stock and under
    CINDERX_JIT_MODE=execute, and require the per-testcase results to be
    identical.

    The import canary that preceded this leg only proved the modules could
    be imported with the JIT executing.  Importing test_call is not running
    test_call: a testcase that behaves differently under Auto-JIT would
    sail through it.  This leg runs the tests themselves and compares
    outcomes case by case, and it demands trigger proof so a silently
    interpreted arm cannot pass by being identical for the wrong reason.
    """
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    modules = load_stdlib72_modules()
    ledger = out / "execute" / "trigger.log"

    execute_env = [
        "CINDERX_JIT_MODE=execute",
        f"PYTHONJITAUTO={args.threshold}",
        f"CINDERX_EXECUTE_TRIGGER_LEDGER={ledger}",
    ]
    stock_dir = getattr(args, "stock_dir", None)
    if stock_dir:
        try:
            reuse_stock_result(Path(stock_dir), out, modules, args.python)
        except ValueError as exc:
            print(f"EXECUTE-GATE: {exc}")
            return 3
        startup = out / "startup"
        startup.mkdir(parents=True, exist_ok=True)
        (startup / "sitecustomize.py").write_text(STARTUP_SITECUSTOMIZE)
        attest = out / "execute" / "attest.log"
        ledger.parent.mkdir(parents=True, exist_ok=True)
        for path in (attest, ledger):
            path.unlink(missing_ok=True)
            path.touch()
            os.chmod(path, 0o666)
        execute = argparse.Namespace(
            **{
                **vars(args),
                "out": str(out / "execute"),
                "tests": modules,
                "exclude": [],
                "env": execute_env,
                "pythonpath_prepend": [str(startup)]
                + list(args.pythonpath_prepend or []),
                "attest_file": str(attest),
            }
        )
        if run_arm(execute):
            return 2
    else:
        rc, attest = provision_dual_arms(
            args,
            out,
            label="EXECUTE-GATE",
            arm_dir="execute",
            shared={**vars(args), "tests": modules, "exclude": []},
            cinderx_env=execute_env,
            precreate=[out / "execute" / "attest.log", ledger],
        )
        if rc:
            return rc
    attest_count, attest_clean = read_attest(attest)
    if attest_count < 2 or not attest_clean:
        print(
            f"EXECUTE-GATE: execute arm evaluator not attested "
            f"({attest_count} records, clean={attest_clean})"
        )
        return 3

    # Trigger proof, attributed to the processes that ran the tests.
    proof = read_trigger_ledger(ledger)
    executing = proof["executing_tests"] & set(modules)
    compiling = proof["compiling_tests"] & set(modules)
    unattributed = proof["unattributed_tests"] & set(modules)
    print(
        f"EXECUTE-GATE: execute arm {proof['rows']} process(es), "
        f"{proof['workers']} test worker(s), "
        f"{proof['worker_entries']} worker machine-code entries, "
        f"{proof['worker_creations']} worker compiled function(s), "
        f"{len(executing)}/{len(modules)} target module(s) in a worker that "
        f"entered machine code, "
        f"{len(compiling)}/{len(modules)} with a compiled function of their "
        f"own"
        + (
            f", {len(unattributed)} unattributed"
            if unattributed
            else ""
        )
    )
    if proof["workers"] <= 0:
        print(
            "EXECUTE-GATE: no regrtest worker reported; the arm cannot "
            "attest anything about the modules it was supposed to run"
        )
        return 5
    if proof["worker_entries"] <= 0 or proof["worker_creations"] <= 0:
        print(
            "EXECUTE-GATE: the test workers never entered machine code. "
            "The scheduler process's own JIT activity does not count: an "
            "arm whose workers all interpreted would agree with stock for "
            "the wrong reason"
        )
        return 5
    if not executing:
        print(
            "EXECUTE-GATE: machine code was entered, but not by any worker "
            f"running a target module (workers seen: {sorted(proof['worker_tests'])[:5]})"
        )
        return 5
    if not compiling:
        # Every worker's counters are process-wide, so "this worker ran
        # machine code" can be satisfied entirely by sitecustomize, the
        # import machinery and regrtest's harness.  At least one target
        # module has to have had a function of its own compiled, or the
        # arm proves only that CinderX ran, not that it ran the surface.
        print(
            "EXECUTE-GATE: no target module had a function of its own "
            "compiled; the arm's machine-code entries are all harness and "
            "import machinery"
        )
        return 5
    # Per-module, not "at least one anywhere": the modules that are known
    # to reach the JIT have to keep reaching it, each of them.  A module
    # that stops is a coverage regression even while the differential
    # stays green, because the arm goes on proving equivalence for a
    # surface the JIT no longer touches.
    expected_jit = load_execute_jit_coverage(
        Path(args.jit_coverage) if args.jit_coverage else EXECUTE_JIT_COVERAGE
    ) & set(modules)
    lost = sorted(expected_jit - compiling)
    if lost:
        print(
            f"EXECUTE-GATE: {len(lost)} module(s) that used to compile a "
            f"function of their own no longer do: {lost}"
        )
        return 5
    gained = sorted(compiling - expected_jit)
    if gained:
        # Not a failure -- more coverage is the direction we want -- but
        # it has to be recorded, or the pinned list decays into a subset
        # nobody notices is stale.
        print(
            f"EXECUTE-GATE: {len(gained)} module(s) now compile a function "
            f"of their own and are not in the pinned list: {gained}"
        )

    baseline_path = Path(args.baseline) if args.baseline else EXECUTE_BASELINE
    report = diff_results_symmetric(
        load(str(out / "stock" / "result.json")),
        load(str(out / "execute" / "result.json")),
        load_execute_baseline(baseline_path),
    )
    report["trigger_proof"] = {
        "workers": proof["workers"],
        "worker_modules": len(proof["worker_tests"] & set(modules)),
        "executing_modules": len(executing),
        "target_attributed_compile_modules": len(compiling),
        "unattributed_modules": len(unattributed),
        "worker_machine_code_entries": proof["worker_entries"],
        "worker_compiled_function_creations": proof["worker_creations"],
    }
    text = json.dumps(report, indent=1, sort_keys=True)
    (out / "diff.json").write_text(text)
    if args.write_baseline:
        Path(args.write_baseline).write_text(
            json.dumps(
                {
                    "comment": (
                        "Frozen stock-vs-execute asymmetries for the "
                        "confirmed 72-module surface.  Every entry needs a "
                        "reason that is about the environment, not about "
                        "the JIT; an entry that stops reproducing is "
                        "reported by the gate and must be deleted."
                    ),
                    "asymmetries": report["differences"],
                },
                indent=1,
                sort_keys=True,
            )
            + "\n"
        )
        print(f"EXECUTE-GATE: wrote baseline to {args.write_baseline}")
        return 0
    print(text if len(text) < 8000
          else text[:8000] + "\n... (truncated, see report file)")
    print(
        f"EXECUTE-GATE: {len(report['differences'])} asymmetry(ies), "
        f"{report['allowed']} allowed by baseline, "
        f"{len(report['unexpected'])} unexpected, "
        f"{len(report['stale_baseline'])} stale baseline entry(ies)"
    )
    return 1 if report["unexpected"] or report["stale_baseline"] else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)

    def common(p: argparse.ArgumentParser) -> None:
        p.add_argument("--python", default=sys.executable)
        p.add_argument("--jobs", type=int, default=min(48, os.cpu_count() or 8))
        p.add_argument("--timeout", type=int, default=1200)
        p.add_argument("--env", action="append",
                       help="KEY=VALUE override for the (cinderx) arm; repeatable")
        p.add_argument("--pythonpath-prepend", action="append", default=[])
        p.add_argument("--tests", nargs="*", help="subset; default = full --list-tests")
        p.add_argument("--exclude", action="append", default=[],
                       help="module to drop from the corpus; repeatable")

    p_run = sub.add_parser("run", help="run one arm")
    common(p_run)
    p_run.add_argument("--out", required=True)
    p_run.add_argument("--attest-file",
                       help="record per-process evaluator attestations here "
                            "and fail the arm unless they all check out")
    p_run.set_defaults(func=run_arm)

    p_diff = sub.add_parser("diff", help="compare two result JSONs")
    p_diff.add_argument("a", help="stock result.json")
    p_diff.add_argument("b", help="cinderx result.json")
    p_diff.add_argument("--report")
    p_diff.set_defaults(func=cmd_diff)

    p_gate = sub.add_parser("gate", help="run both arms and diff")
    common(p_gate)
    p_gate.add_argument("--out", required=True)

    p_exec = sub.add_parser(
        "execute-gate",
        help="72-module stock vs CINDERX_JIT_MODE=execute differential",
    )
    common(p_exec)
    p_exec.add_argument("--out", required=True)
    p_exec.add_argument(
        "--stock-dir",
        help="reuse a completed frozen stock arm (result.json plus "
             "attest-stock.log) instead of running stock again",
    )
    p_exec.add_argument(
        "--threshold",
        default="50",
        help="PYTHONJITAUTO for the execute arm (default: 50)",
    )
    p_exec.add_argument(
        "--jit-coverage",
        help="modules that must each compile a function of their own "
             "(default: jit311/data/execute_jit_coverage.txt)",
    )
    p_exec.add_argument(
        "--baseline",
        help="frozen stock-vs-execute asymmetries "
             "(default: jit311/data/execute_diff_baseline.json)",
    )
    p_exec.add_argument(
        "--write-baseline",
        help="record this run's asymmetries as the baseline instead of "
             "judging against one; every entry still needs a reason",
    )
    p_exec.set_defaults(func=cmd_execute_gate)
    p_gate.set_defaults(func=cmd_gate)

    p_off = sub.add_parser(
        "off-gate",
        help="440-module stock/evaluator-off differential",
    )
    common(p_off)
    p_off.add_argument("--out", required=True)
    p_off.set_defaults(func=cmd_off_gate)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
