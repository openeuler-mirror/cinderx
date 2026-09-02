"""Decide whether a regrtest -R "memory blocks" line is a real leak.

regrtest computes its block figure as

    alloc_after = sys.getallocatedblocks() - sys._getquickenedcount()

On CPython 3.11 that subtraction is only sound when both terms come from
the same interpreter.  They do not here.  `_Py_QuickenedCount` is a
file-local symbol in the interpreter (`nm` reports it lowercase `b`), so
it cannot be linked against, and the vendored evaluator in
cinderx/Interpreter/3.11/upstream/specialize.c has to define its own
copy.  Quickening therefore increments CinderX's counter, while
`code_dealloc` in the interpreter decrements the interpreter's counter
for every code object with `co_warmup == 0` -- the one
`sys._getquickenedcount()` returns.  That counter can only fall, and
subtracting a negative number inflates `alloc_after` by exactly the
drift.

So a "memory blocks" line means nothing on its own.  This module runs the
module again while recording both terms, and reports the line as an
artifact only when the arithmetic proves it:

  * the raw block count does not grow across the tracked window, and
  * the quickened counter's drift accounts for the whole reported figure.

If either fails, the line is treated as a real leak.  Reference leaks are
never excused here -- they are the acceptance item's actual subject.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
from pathlib import Path
import sys
import tempfile

_HOOK_FILE = Path(__file__).with_name("quickened_artifact_hook.py")
_BLOCKS = re.compile(r"^(\S+) leaked \[([-0-9, ]+)\] memory blocks", re.M)
_REFS = re.compile(r"^(\S+) leaked \[([-0-9, ]+)\] references", re.M)


_FDS = re.compile(r"^(\S+) leaked \[([-0-9, ]+)\] file descriptors", re.M)
_RESULT = re.compile(r"^== Tests result: (\w+)", re.M)
_FINAL_RESULT = re.compile(r"^Result: (\w+)$", re.M)
_TOTAL_FILES = re.compile(
    r"^Total test files: success=(\d+) failed=(\d+)$", re.M
)
_ORDINARY_FAILURE = re.compile(
    r"^(?:FAIL|ERROR): |^Traceback \(most recent call last\):|"
    r"^test .* failed --|timed out|crashed|worker thread failed",
    re.M | re.I,
)


def only_block_artifact_failure(
    module: str, text: str, returncode: int
) -> tuple[bool, str]:
    """Prove that a single-module regrtest failed only on its block line.

    A verified block figure cannot excuse another failure from the same
    module.  Accept only the complete CPython 3.11 single-module epilogue
    emitted for a refleak finding, and reject every additional ordinary
    failure signal.
    """
    if returncode != 2:
        return False, f"{module}: expected refleak exit 2, got {returncode}"

    blocks = _BLOCKS.findall(text)
    if not blocks or {name for name, _ in blocks} != {module}:
        return False, f"{module}: missing or foreign memory-block finding"
    if _REFS.search(text) or _FDS.search(text):
        return False, f"{module}: non-block leak is never an artifact"

    failure_lines = re.findall(
        rf"^{re.escape(module)} failed(?: \(([^)]*)\))?"
        r"(?: in [^\r\n]+)?$",
        text,
        re.M,
    )
    if failure_lines != ["reference leak"]:
        return False, (
            f"{module}: failure reasons were {failure_lines!r}, not only "
            "the refleak block finding"
        )

    result = _RESULT.findall(text)
    final_result = _FINAL_RESULT.findall(text)
    totals = _TOTAL_FILES.findall(text)
    if result != ["FAILURE"] or final_result != ["FAILURE"]:
        return False, f"{module}: incomplete or ambiguous FAILURE epilogue"
    if totals != [("0", "1")]:
        return False, f"{module}: unexpected test-file totals {totals!r}"

    failed_section = re.search(
        r"^1 test failed:\s*\n((?:[ \t]+\S+\s*\n)+)", text, re.M
    )
    if failed_section is None:
        return False, f"{module}: missing single-test failure summary"
    failed_names = re.findall(r"^\s+(\S+)\s*$", failed_section.group(1), re.M)
    if failed_names != [module]:
        return False, f"{module}: unexpected failed tests {failed_names!r}"

    ordinary = _ORDINARY_FAILURE.search(text)
    if ordinary is not None:
        return False, (
            f"{module}: ordinary failure signal {ordinary.group(0)!r} "
            "was present alongside the block artifact"
        )
    return True, f"{module}: only the memory-block refleak finding failed"


def _run_verification(
    python: str, module: str, warmups: int, reps: int
) -> tuple[str, int, dict[str, list[int]] | None]:
    """Run regrtest -R once with the counting hook installed.

    Returns the combined output, the process return code, and the counts
    the hook recorded (None when the hook produced nothing, which is what
    a crashed or never-started verification process looks like).
    """
    with tempfile.TemporaryDirectory() as tmp:
        hook_dir = os.path.join(tmp, "hook")
        os.mkdir(hook_dir)
        with open(os.path.join(hook_dir, "sitecustomize.py"), "w") as fh:
            fh.write(_HOOK_FILE.read_text())
        out = os.path.join(tmp, "counts.txt")
        env = dict(os.environ)
        env.update(
            QA_OUT=out,
            QA_SKIP=str(warmups),
            PYTHONPATH=hook_dir + os.pathsep + env.get("PYTHONPATH", ""),
        )
        proc = subprocess.run(
            [python, "-m", "test", "-R", f"{warmups}:{reps}", module],
            capture_output=True, text=True, env=env,
        )
        counts: dict[str, list[int]] | None = None
        if os.path.exists(out):
            parsed = {}
            with open(out) as fh:
                lines = fh.readlines()
            for line in lines:
                parts = line.split()
                if len(parts) >= 2:
                    try:
                        parsed[parts[0]] = [int(v) for v in parts[1:]]
                    except ValueError:
                        continue
            if parsed:
                counts = parsed
        return proc.stdout + proc.stderr, proc.returncode, counts


def classify_run(
    module: str,
    text: str,
    returncode: int,
    counts: dict[str, list[int]] | None,
) -> tuple[bool, str]:
    """Decide over one verification run.  Fail-closed by construction.

    Nothing here may return "artifact" on the strength of an ABSENCE.
    The verification process runs under CINDERX_JIT_MODE=execute, so the
    very fault being hunted can kill it; a crashed verifier produces no
    "memory blocks" line, and reading that silence as proof would turn
    this gate into the thing it exists to catch.  Every path that has not
    positively proven the arithmetic returns False.
    """
    if returncode is not None and returncode < 0:
        return False, (
            f"{module}: verification process died on signal {-returncode} "
            f"-- nothing was verified"
        )
    result = _RESULT.search(text)
    if result is None:
        return False, (
            f"{module}: verification run never reached a regrtest result "
            f"(exit {returncode}) -- nothing was verified"
        )

    refs = _REFS.search(text)
    if refs:
        return False, f"{module}: reference leak {refs.group(2)} -- not an artifact"
    fds = _FDS.search(text)
    if fds:
        return False, f"{module}: file-descriptor leak {fds.group(2)} -- not an artifact"

    blocks = _BLOCKS.search(text)
    if not blocks:
        # The figure did not reproduce.  That is not proof of an artifact:
        # the leg saw a block figure, this run did not explain it, and an
        # unexplained figure is exactly what must not be waved through.
        if returncode != 0 or result.group(1) != "SUCCESS":
            return False, (
                f"{module}: verification run did not complete successfully "
                f"(exit {returncode}, result {result.group(1)})"
            )
        return False, (
            f"{module}: the block figure did not reproduce on the "
            f"verification run, so the arithmetic could not be checked -- "
            f"unverified rather than excused"
        )

    shape_ok, shape_why = only_block_artifact_failure(module, text, returncode)
    if not shape_ok:
        return False, shape_why

    if counts is None or "blocks" not in counts or "quick" not in counts:
        return False, f"{module}: verification hook produced no counts"

    reported = [int(v) for v in blocks.group(2).split(",")]
    return decide(module, reported, counts["blocks"], counts["quick"])


def classify(
    python: str, module: str, warmups: int, reps: int, run=None
) -> tuple[bool, str]:
    """Return (is_artifact, explanation) for one module.

    `run` is injectable so the fail-closed paths -- a verifier that
    segfaults, exits non-zero, or never gets its hook running -- can be
    tested without arranging a real crash.
    """
    runner = run if run is not None else _run_verification
    text, returncode, counts = runner(python, module, warmups, reps)
    return classify_run(module, text, returncode, counts)


def decide(
    module: str,
    reported: list[int],
    blocks_series: list[int],
    quick_series: list[int],
) -> tuple[bool, str]:
    """The judgement, separated from the run so it can be tested directly.

    Judged on a SETTLED tail only.  Early samples still carry compilation:
    the JIT allocates code buffers while functions are crossing the
    threshold, and that growth is not a leak.  Comparing the first and
    last sample of the whole window calls every execute-mode run a leak.
    """
    tail = max(3, len(blocks_series) // 2)
    b_tail = blocks_series[-tail:]
    q_tail = quick_series[-tail:]
    raw_drift = b_tail[-1] - b_tail[0]
    quick_drift = q_tail[-1] - q_tail[0]
    steps = len(b_tail) - 1
    per_rep = reported[-1] if reported else 0

    if raw_drift > 0:
        return False, (
            f"{module}: raw blocks grew by {raw_drift} over the settled "
            f"tail {b_tail} -- a real leak, not the quickened artifact"
        )
    if quick_drift >= 0:
        return False, (
            f"{module}: quickened counter did not drift ({q_tail}) yet "
            f"{per_rep} blocks were reported -- treating as a real leak"
        )
    implied = -quick_drift / steps if steps else 0
    if per_rep and abs(implied - per_rep) > max(1, 0.25 * per_rep):
        return False, (
            f"{module}: quickened drift implies {implied:.1f} blocks per "
            f"repetition but {per_rep} were reported -- unexplained"
        )
    return True, (
        f"{module}: artifact -- raw blocks flat over the settled tail "
        f"(drift {raw_drift}), quickened counter fell {quick_drift} over "
        f"{steps} repetitions, matching the reported {per_rep} per repetition"
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("python")
    ap.add_argument("modules", nargs="+")
    ap.add_argument("--warmups", type=int, default=20)
    ap.add_argument("--reps", type=int, default=6)
    ap.add_argument(
        "--original-log-dir",
        type=Path,
        help="require each original single-module log to contain only the block failure",
    )
    args = ap.parse_args()

    ok = True
    for module in args.modules:
        if args.original_log_dir is not None:
            original_log = args.original_log_dir / f"{module}.log"
            try:
                original_text = original_log.read_text(errors="replace")
            except OSError as exc:
                print(f"REAL LEAK: {module}: cannot read {original_log}: {exc}")
                ok = False
                continue
            shape_ok, shape_why = only_block_artifact_failure(
                module, original_text, 2
            )
            print(("failure shape: " if shape_ok else "REAL LEAK: ") + shape_why)
            if not shape_ok:
                ok = False
                continue
        artifact, why = classify(args.python, module, args.warmups, args.reps)
        print(("artifact: " if artifact else "REAL LEAK: ") + why)
        ok = ok and artifact
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
