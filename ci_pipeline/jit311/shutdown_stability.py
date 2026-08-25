"""Repeated real-process shutdown and finalize-state matrix.

The driver spawns one child per iteration, and the child builds the named
JIT lifecycle state, prints its readiness evidence and exits normally.
Anything the process does after that line -- interpreter finalization,
module teardown, C++ static destruction -- is the surface under test.

Two lessons from the first discovery round are baked into the driver
rather than left to the operator:

1. A teardown use-after-free is deterministic PER MEMORY LAYOUT and
   flaky ACROSS layouts.  The B9 multithread-completed crash exits 0 in
   one fixed environment and SIGSEGVs in another, so a gate that spawns
   every child with the same environment block samples exactly one
   layout and can report a clean 100/100 while the bug is deterministic
   one environment away.  Children therefore run with glibc's
   ``MALLOC_PERTURB_`` (freed memory is poisoned, so a read through a
   freed C++ object faults in every layout) plus a deterministic
   per-iteration environment pad that varies the initial layout anyway.
2. A crash without a stack is a rumor.  The driver keeps the first cores
   per state (RLIMIT_CORE is raised for the early iterations), extracts
   a gdb backtrace when gdb is present, and stores both next to the
   result instead of asking for a manual reproduction.

Per-state quotas replace the old round-robin count: 100 total exits gave
the multithread-completed state ~16 attempts, which is no statistical
power at all.  Quotas are reported per state, never as one total.
"""

from __future__ import annotations

import argparse
import gc
import json
import os
from pathlib import Path
import resource
import shutil
import subprocess
import sys
import time
import types


STATES = (
    "installed",
    "parked",
    "function-death",
    "code-death",
    "failure-unwind",
    "multithread-completed",
)

FORBIDDEN_STDERR = (
    "Fatal Python error",
    "JIT_CHECK",
    "AddressSanitizer",
    "double free",
    "invalid pointer",
    "Exception ignored in",
)

# glibc: free() fills released memory with this byte and malloc() hands out
# the complement, so a stale pointer into freed memory reads poison instead
# of a lucky survivor.  This is what turned the B9 teardown use-after-free
# from "exits 0 in the gate's fixed layout" into a deterministic SIGSEGV.
DEFAULT_MALLOC_PERTURB = 165

# The pad cycles through this span so consecutive iterations run under
# different initial heap layouts.  A prime stride visits many distinct
# sizes before repeating; determinism keeps every run reproducible.
ENV_ENTROPY_SPAN = 4096
ENV_ENTROPY_STRIDE = 173

# Early iterations per state run with RLIMIT_CORE raised so the first
# crashes leave cores; the rest run without to bound disk.
CORE_ARMED_ITERATIONS = 5
CORES_KEPT_PER_STATE = 2


def _plain(index: int):
    namespace = {"__builtins__": __builtins__, "__name__": "__main__"}
    source = (
        f"def shutdown_{index}(a, b, one):\n"
        "    total = a - a\n"
        "    i = total\n"
        "    while i < b:\n"
        "        total += a\n"
        "        i += one\n"
        f"    return total + {index}\n"
    )
    exec(compile(source, f"<shutdown-stability-{index}>", "exec"), namespace, namespace)
    return namespace[f"shutdown_{index}"], namespace, 15 + index


def child(state: str) -> dict:
    import _cinderx
    import cinderjit
    import cinderx

    cinderx.init()
    _cinderx.install_frame_evaluator()
    before = _cinderx._get_trigger_stats()["machine_code_entries"]
    roots = []

    owner, owner_namespace, expected = _plain(0)
    assert cinderjit.force_compile(owner) is True
    assert owner(3, 5, 1) == expected
    roots.extend((owner, owner_namespace))

    generator_namespace = {"__builtins__": __builtins__, "__name__": "__main__"}
    exec(
        "def shutdown_generator(value):\n"
        "    yield value\n"
        "    yield value + 1\n",
        generator_namespace,
        generator_namespace,
    )
    generator_function = generator_namespace["shutdown_generator"]
    assert cinderjit.force_compile(generator_function) is True
    suspended_generator = generator_function(1)
    assert next(suspended_generator) == 1
    roots.extend((generator_namespace, generator_function, suspended_generator))

    for index in range(1, 9):
        fresh = types.FunctionType(owner.__code__, owner.__globals__, f"fresh_{index}")
        assert cinderjit.force_compile(fresh) is True
        assert fresh(3, 5, 1) == expected
        roots.append(fresh)

    if state == "parked":
        cinderjit.disable()
    elif state in ("function-death", "code-death"):
        for index in range(20, 40):
            transient, namespace, transient_expected = _plain(index)
            assert cinderjit.force_compile(transient) is True
            assert transient(3, 5, 1) == transient_expected
            del transient, namespace
        gc.collect()
        gc.collect()
    elif state == "failure-unwind":
        transient, namespace, transient_expected = _plain(50)
        assert cinderjit._jit311_compile_with_publish_failure(transient, 5) is True
        assert not cinderjit.is_jit_compiled(transient)
        assert cinderjit.force_compile(transient) is True
        assert transient(3, 5, 1) == transient_expected
        roots.extend((transient, namespace))
    elif state == "multithread-completed":
        assert cinderjit._jit311_multithreaded_compile_test_enabled() is True
        batch = []
        for index in range(60, 68):
            function, namespace, batch_expected = _plain(index)
            assert cinderjit._jit311_register_for_compile(function) is True
            batch.append((function, namespace, batch_expected))
        cinderjit._jit311_multithreaded_compile_test()
        for function, namespace, batch_expected in batch:
            assert cinderjit.is_jit_compiled(function)
            assert function(3, 5, 1) == batch_expected
        roots.extend(batch)

    # The object is deliberately rooted until module teardown.  Its finalizer
    # reaches only the read-only private control plane, so stderr exposes an
    # unsafe finalize ordering without changing the state being observed.
    class ExitFinalizer:
        def __init__(self, jit):
            self.jit = jit

        def __del__(self):
            state = self.jit._jit311_lifecycle_snapshot()
            assert state["schema"] == "cp311-jit-lifecycle-v1"
            self.jit.is_enabled()

    roots.append(ExitFinalizer(cinderjit))
    snapshot = dict(cinderjit._jit311_lifecycle_snapshot())
    invariants = dict(cinderjit._jit311_lifecycle_invariants())
    entries = _cinderx._get_trigger_stats()["machine_code_entries"] - before
    assert entries > 0
    assert invariants.get("ok") is True, invariants
    # Keep the complete graph rooted to process shutdown.
    globals()["_A3_SHUTDOWN_ROOTS"] = roots
    return {
        "result": "READY_FOR_NORMAL_EXIT",
        "state": state,
        "machine_code_entries": entries,
        "snapshot": snapshot,
        "invariants": invariants,
    }


def parse_quota(text: str) -> dict[str, int]:
    quota: dict[str, int] = {}
    for item in text.split(","):
        item = item.strip()
        if not item:
            continue
        state, _, count = item.partition("=")
        if state not in STATES:
            raise ValueError(f"unknown shutdown state in quota: {state!r}")
        if not count.isdigit() or int(count) <= 0:
            raise ValueError(f"quota for {state} must be a positive integer")
        if state in quota:
            raise ValueError(f"duplicate quota for {state}")
        quota[state] = int(count)
    if not quota:
        raise ValueError("empty shutdown quota")
    return quota


def entropy_pad(index: int, span: int) -> int:
    return (index * ENV_ENTROPY_STRIDE) % span if span > 0 else 0


def _gdb_backtrace(core: Path, timeout: int = 60) -> str | None:
    gdb = shutil.which("gdb")
    if gdb is None:
        return None
    try:
        process = subprocess.run(
            [
                gdb,
                "-batch",
                "-q",
                "-ex",
                "set pagination off",
                "-ex",
                "bt 30",
                sys.executable,
                str(core),
            ],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    return process.stdout[-4000:] or None


def _run_one(
    state: str,
    iteration: int,
    global_index: int,
    child_timeout: int,
    *,
    malloc_perturb: int,
    entropy_span: int,
    core_dir: Path | None,
) -> dict:
    env = os.environ.copy()
    if state == "multithread-completed":
        env.update(
            PYTHONJITMULTITHREADEDCOMPILETEST="1",
            PYTHONJITBATCHCOMPILEWORKERS="4",
        )
    if malloc_perturb > 0:
        env["MALLOC_PERTURB_"] = str(malloc_perturb)
    pad = entropy_pad(global_index, entropy_span)
    if pad:
        env["SHUTDOWN_STABILITY_LAYOUT_PAD"] = "x" * pad
    # A crash during the measured workload should say where it was; the
    # teardown crashes this lane hunts happen after the handler is torn
    # down and are covered by cores instead.
    env["PYTHONFAULTHANDLER"] = "1"

    arm_core = core_dir is not None and iteration <= CORE_ARMED_ITERATIONS
    cwd = None
    preexec = None
    if arm_core:
        cwd = core_dir / f"{state}-{iteration:04d}"
        cwd.mkdir(parents=True, exist_ok=True)

        def preexec() -> None:
            resource.setrlimit(resource.RLIMIT_CORE, (resource.RLIM_INFINITY, resource.RLIM_INFINITY))

    started = time.monotonic()
    timed_out = False
    try:
        process = subprocess.run(
            [
                sys.executable,
                "-m",
                "ci_pipeline.jit311.shutdown_stability",
                "--child",
                "--state",
                state,
            ],
            capture_output=True,
            text=True,
            env=env,
            timeout=child_timeout,
            cwd=cwd,
            preexec_fn=preexec,
        )
        returncode = process.returncode
        stdout = process.stdout
        stderr = process.stderr
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        returncode = 124
        stdout = (exc.stdout or b"").decode("utf-8", "replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
        stderr = (exc.stderr or b"").decode("utf-8", "replace") if isinstance(exc.stderr, bytes) else (exc.stderr or "")
    forbidden = [token for token in FORBIDDEN_STDERR if token in stderr]
    payload = None
    try:
        payload = json.loads(stdout.strip().splitlines()[-1])
    except (IndexError, json.JSONDecodeError):
        pass
    errors = []
    if timed_out:
        errors.append("timeout")
    if returncode != 0:
        errors.append(f"exit code {returncode}")
    if forbidden:
        errors.append(f"forbidden stderr tokens: {forbidden}")
    if not payload or payload.get("result") != "READY_FOR_NORMAL_EXIT":
        errors.append("missing child readiness evidence")
    row = {
        "iteration": iteration,
        "state": state,
        "returncode": returncode,
        "timed_out": timed_out,
        "duration_s": round(time.monotonic() - started, 3),
        "layout_pad": pad,
        "errors": errors,
    }
    if errors:
        row.update(
            forbidden_stderr=forbidden,
            child=payload,
            stdout_tail=stdout[-1000:],
            stderr_tail=stderr[-2000:],
        )
    if cwd is not None:
        cores = sorted(cwd.glob("core*"))
        if errors and cores:
            row["core"] = str(cores[0])
        else:
            shutil.rmtree(cwd, ignore_errors=True)
    return row


def driver(
    quota: dict[str, int],
    child_timeout: int,
    out: Path,
    *,
    malloc_perturb: int = DEFAULT_MALLOC_PERTURB,
    entropy_span: int = ENV_ENTROPY_SPAN,
) -> dict:
    core_root = out.parent / f"{out.stem}-cores"
    per_state: dict[str, dict] = {}
    failures = []
    rows = []
    global_index = 0
    for state, attempts in quota.items():
        summary = {
            "attempts": attempts,
            "successes": 0,
            "sigsegv": 0,
            "sigabrt": 0,
            "timeouts": 0,
            "other_failures": 0,
            "forbidden_stderr": 0,
            "cores": [],
            "native_backtrace": None,
        }
        per_state[state] = summary
        for iteration in range(1, attempts + 1):
            global_index += 1
            row = _run_one(
                state,
                iteration,
                global_index,
                child_timeout,
                malloc_perturb=malloc_perturb,
                entropy_span=entropy_span,
                core_dir=core_root,
            )
            rows.append(row)
            if not row["errors"]:
                summary["successes"] += 1
                continue
            failures.append(row)
            # A deterministic teardown crash repeats thousands of times;
            # full stdio evidence on every repetition only bloats the
            # result.  The first failures per state keep their tails.
            if iteration - summary["successes"] > 10:
                for key in ("stdout_tail", "stderr_tail", "child"):
                    row.pop(key, None)
            if row["timed_out"]:
                summary["timeouts"] += 1
            elif row["returncode"] == -11 or row["returncode"] == 139:
                summary["sigsegv"] += 1
            elif row["returncode"] == -6 or row["returncode"] == 134:
                summary["sigabrt"] += 1
            else:
                summary["other_failures"] += 1
            if row.get("forbidden_stderr"):
                summary["forbidden_stderr"] += 1
            core = row.get("core")
            if core and len(summary["cores"]) < CORES_KEPT_PER_STATE:
                summary["cores"].append(core)
                if summary["native_backtrace"] is None:
                    summary["native_backtrace"] = _gdb_backtrace(Path(core))
            elif core:
                shutil.rmtree(Path(core).parent, ignore_errors=True)
                row.pop("core", None)
    repetitions = sum(quota.values())
    result = {
        "result": "PASS" if not failures else "FAIL",
        "repetitions": repetitions,
        "successful_exits": repetitions - len(failures),
        "states": list(quota),
        "quota": dict(quota),
        "per_state": per_state,
        "detector": {
            "malloc_perturb": malloc_perturb,
            "env_entropy_span": entropy_span,
            "env_entropy_stride": ENV_ENTROPY_STRIDE,
            "core_armed_iterations": CORE_ARMED_ITERATIONS,
        },
        "failures": failures,
        "rows": rows,
    }
    out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--child", action="store_true")
    parser.add_argument("--state", choices=STATES, default="installed")
    parser.add_argument(
        "--repetitions",
        type=int,
        default=100,
        help="legacy round-robin total, used only when no --quota is given",
    )
    parser.add_argument(
        "--quota",
        help="per-state attempts, e.g. installed=200,multithread-completed=2000",
    )
    parser.add_argument(
        "--only-state",
        choices=STATES,
        help="run one state for --repetitions attempts",
    )
    parser.add_argument(
        "--malloc-perturb",
        type=int,
        default=DEFAULT_MALLOC_PERTURB,
        help="glibc MALLOC_PERTURB_ byte for children; 0 disables poisoning",
    )
    parser.add_argument(
        "--env-entropy",
        type=int,
        default=ENV_ENTROPY_SPAN,
        help="span of the per-iteration environment pad; 0 pins one layout",
    )
    parser.add_argument("--child-timeout", type=int, default=30)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args(argv)
    if args.child:
        print(json.dumps(child(args.state), sort_keys=True), flush=True)
        return 0
    if args.out is None:
        parser.error("--out is required in driver mode")
    if args.quota and args.only_state:
        parser.error("--quota and --only-state are mutually exclusive")
    if args.quota:
        quota = parse_quota(args.quota)
    elif args.only_state:
        quota = {args.only_state: args.repetitions}
    else:
        base, remainder = divmod(args.repetitions, len(STATES))
        quota = {
            state: base + (1 if index < remainder else 0)
            for index, state in enumerate(STATES)
            if base > 0 or index < remainder
        }
    result = driver(
        quota,
        args.child_timeout,
        args.out,
        malloc_perturb=args.malloc_perturb,
        entropy_span=args.env_entropy,
    )
    print(
        json.dumps(
            {
                "result": result["result"],
                "successful_exits": result["successful_exits"],
                "per_state": {
                    state: {
                        key: value
                        for key, value in summary.items()
                        if key not in ("cores", "native_backtrace")
                    }
                    for state, summary in result["per_state"].items()
                },
            },
            sort_keys=True,
        )
    )
    return 0 if result["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
