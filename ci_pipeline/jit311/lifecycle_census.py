"""Lifecycle census for the CPython 3.11 Auto-JIT execute mode.

Not a reference-leak hunt in regrtest's sense.  regrtest's -R needs
sys.gettotalrefcount, which only a debug interpreter has, and the openEuler
3.11.6 this port runs on is a release build; a real -R leg belongs on a
Py_DEBUG interpreter and is not what this is.  What this does is run the
execute-mode lifecycle repeatedly and require every resource it allocates
to come back, using regrtest's *criterion* -- report only what grows in
EVERY measured round, so a cache that fills once and stops is not a
finding.

Two censuses, because one of them cannot see the interesting failures.
The Python object census (gc.get_objects()) covers artifacts, functions
and code objects.  It is blind to everything the runtime allocates
outside the GC: the observer's table comes from calloc(), a code-extra
block from PyMem_Calloc(), a machine-code buffer from an executable
mapping.  Leaking one code buffer per round leaves the object census
perfectly flat, so the runtime's own gauges are censused alongside it:

    resident_code_buffers        buffers a CompiledFunction still owns
    resident_code_extra_blocks   owned code-extra blocks still allocated
    watched_codes                observer slots keyed to a live code object
    compiled_functions           functions the JIT registry still holds

The reference-count matrix is the complement of this leg: it watches the
reference counts of objects a corpus names, which is exactly what a census
cannot see, and it cannot see an allocation no name reaches, which is
exactly what a census can.

The workload drives scheduling, fresh attachment, outer anchoring,
explicit retirement, pause and resume, and code death, and returns to its
starting state each round.  The leg fails closed: a round that never
entered machine code, never attached and never retired anything proves
nothing, and says so instead of passing.
"""

from __future__ import annotations

import argparse
import collections
import gc
import json
import sys

SRC = "\n".join(
    (
        "def outer():",
        "    def inner(a, b):",
        "        total = a - a",
        "        i = total",
        "        while i < b:",
        "            total = total + a",
        "            i = i + 1",
        "        return total",
        "    return inner",
    )
)


def build_round(index: int):
    # A code object that has never been seen before: a round must not be
    # able to ride on the previous round's scheduling decisions.
    namespace = {}
    exec(compile(SRC, f"<census{index}>", "exec"), namespace, namespace)

    class Holder:
        # A class namespace is one of the two places the outer-function
        # walk looks, and a staticmethod is the wrapper it has to unwrap.
        make = staticmethod(namespace["outer"])

    namespace["Holder"] = Holder
    return namespace


def run_round(jit, index: int, threshold: int) -> dict:
    namespace = build_round(index)
    warm = threshold * 3

    # Scheduling: the first instance crosses the threshold on its own.
    first = namespace["Holder"].make()
    for i in range(warm):
        assert first(i, 2) == i * 2

    # Fresh attachment: further instances over the same code object take
    # the published artifact without a compile of their own.
    fresh = [namespace["Holder"].make() for _ in range(8)]
    for f in fresh:
        for i in range(warm):
            assert f(i, 2) == i * 2

    compiled = sum(1 for f in [first, *fresh] if jit.is_jit_compiled(f))

    # Park and resume: disable() deopts the registry, enable() puts the
    # published artifacts back to work.
    jit.disable()
    for i in range(4):
        assert first(i, 2) == i * 2
    jit.enable()
    for i in range(warm):
        assert first(i, 2) == i * 2

    # Explicit retirement, then death.  The instances share one artifact,
    # so the first retirement takes it for all of them and the rest answer
    # False -- the count below reads "the retirement path ran", not "four
    # artifacts went away".
    retired = sum(1 for f in fresh[:4] if jit.force_uncompile(f))

    del first, fresh, namespace
    gc.collect()
    gc.collect()
    return {"compiled": compiled, "retired": retired}


def census(jit) -> dict:
    gc.collect()
    gc.collect()
    counts = collections.Counter()
    for obj in gc.get_objects():
        counts[type(obj).__qualname__] += 1
    counts["<all gc objects>"] = sum(counts.values())
    # The runtime's own gauges, named apart from the type census so a
    # finding says which of the two censuses saw it.
    import _cinderx

    trigger = _cinderx._get_trigger_stats()
    observe = _cinderx._get_observe_stats()
    for key in ("resident_code_buffers", "resident_code_extra_blocks"):
        counts[f"<native> {key}"] = trigger[key]
    counts["<native> watched_codes"] = observe["watched_codes"]
    # The observer's table is a calloc() allocation sized by capacity, so
    # watched_codes returning to baseline does not by itself say the
    # memory did: a table that keeps its dead entries' keys can ratchet up
    # an octave per churn cycle with a flat live population.
    counts["<native> observer_table_capacity"] = observe["table_capacity"]
    counts["<native> compiled_functions"] = len(jit.get_compiled_functions())
    return dict(counts)


NATIVE_PREFIX = "<native> "


def native_residency_drift(samples: list) -> dict:
    """Native gauges that did not come back to the post-warmup baseline.

    A different criterion from the object census below, because these
    measure a different thing.  The workload returns to its starting state
    every round by construction, so each of these gauges must read exactly
    what it read after warm-up -- and "grew in every round" would wave
    through the commonest shape a real leak has: one object stranded on
    the first measured round and never again (0, 1, 1, 1), or one stranded
    every other round (0, 1, 1, 2, 2).  Both are permanent, and both are
    monotonic-with-flat-spots rather than strictly increasing.

    The observer's table capacity is a high-watermark rather than a
    population, so it is held to the same rule for a different reason: an
    identical workload must not need a bigger table on round five than it
    needed on round one.
    """
    if len(samples) < 2:
        return {}
    baseline = samples[0]
    drift = {}
    for key in sorted(k for k in baseline if k.startswith(NATIVE_PREFIX)):
        rounds = [sample.get(key) for sample in samples[1:]]
        if any(value != baseline[key] for value in rounds):
            drift[key] = {"baseline": baseline[key], "rounds": rounds}
    return drift


def growing_every_round(samples: list) -> dict:
    # regrtest's criterion: a key that rose between every consecutive pair
    # of measured censuses.  Anything that settles -- a cache filling, a
    # table being built once -- fails this and is not reported.
    keys = set()
    for sample in samples:
        keys |= set(sample)
    # The native gauges have their own, stricter judge above; holding them
    # to this one is what let a permanent leak read as green.
    keys = {key for key in keys if not key.startswith(NATIVE_PREFIX)}
    leaks = {}
    for key in sorted(keys):
        deltas = [
            samples[i + 1].get(key, 0) - samples[i].get(key, 0)
            for i in range(len(samples) - 1)
        ]
        if deltas and all(delta > 0 for delta in deltas):
            leaks[key] = deltas
    return leaks


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--threshold", type=int, default=20)
    parser.add_argument("--out")
    args = parser.parse_args()

    import _cinderx
    import cinderx

    cinderx.init()
    # Without the frame evaluator nothing is scheduled and the hunt would
    # measure an interpreter that never ran the machinery under test.
    _cinderx.install_frame_evaluator()
    if not _cinderx.is_frame_evaluator_installed():
        print("lifecycle-census: the frame evaluator did not install", file=sys.stderr)
        return 2
    try:
        import cinderjit as jit
    except ImportError:
        print(
            "lifecycle-census: cinderjit is not available; refusing to hunt in an "
            "interpreter that cannot execute machine code",
            file=sys.stderr,
        )
        return 2
    stats = _cinderx._get_observe_stats()
    if stats["mode"] != "execute":
        print(
            f"lifecycle-census: the hunt only means anything in execute mode; the "
            f"interpreter reports {stats['mode']!r}",
            file=sys.stderr,
        )
        return 2

    def entries() -> int:
        return _cinderx._get_trigger_stats()["machine_code_entries"]

    # Rounds the census is not taken over: the first pass through any
    # workload fills caches that a leak hunt must not be shown.
    for index in range(args.warmup):
        run_round(jit, index, args.threshold)

    samples, evidence = [], []
    before_entries = entries()
    samples.append(census(jit))
    for index in range(args.rounds):
        evidence.append(run_round(jit, args.warmup + index, args.threshold))
        samples.append(census(jit))
    entered = entries() - before_entries

    report = {
        "rounds": args.rounds,
        "warmup": args.warmup,
        "machine_code_entries": entered,
        "evidence": evidence,
        "leaks": growing_every_round(samples),
        "residency_drift": native_residency_drift(samples),
    }
    if args.out:
        json.dump(report, open(args.out, "w"), indent=1, sort_keys=True)

    # Fail closed before judging: a hunt over a workload that never
    # compiled, never attached and never retired anything would report a
    # clean census for the wrong reason.
    if entered <= 0:
        print("lifecycle-census: no round entered machine code", file=sys.stderr)
        return 2
    silent = [r for r in evidence if r["compiled"] < 9 or r["retired"] < 1]
    if silent:
        print(
            f"lifecycle-census: {len(silent)} round(s) did not exercise the "
            f"lifecycle (compiled/retired counts: {silent})",
            file=sys.stderr,
        )
        return 2

    if report["residency_drift"]:
        print(json.dumps(report["residency_drift"], indent=1, sort_keys=True))
        print(
            f"lifecycle-census: {len(report['residency_drift'])} residency "
            f"gauge(s) did not return to the post-warmup baseline"
        )
        return 1
    if report["leaks"]:
        print(json.dumps(report["leaks"], indent=1, sort_keys=True))
        print(
            f"lifecycle-census: {len(report['leaks'])} census key(s) grew in "
            f"every one of {args.rounds} rounds"
        )
        return 1
    native = sum(1 for key in samples[-1] if key.startswith(NATIVE_PREFIX))
    print(
        f"lifecycle-census: {args.rounds} rounds, {entered} machine-code "
        f"entries, {native} residency gauge(s) back at baseline, "
        f"{len(samples[-1]) - native} object census key(s), none grew in "
        f"every round"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
