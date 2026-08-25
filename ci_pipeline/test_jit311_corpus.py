# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Integrity self-test for the migrated 3.11 corpora.

Proves the migration is intact and consumable: every corpus module imports
under CPython 3.11, the case counts have not silently shrunk, and every case
runs to completion under the stock interpreter (an exception raised by a
case is a valid outcome -- several cases exist to pin error messages -- but
a hang or interpreter crash is not).
"""

import hashlib
import sys
from pathlib import Path

import pytest

if sys.version_info[:2] != (3, 11):
    pytest.skip(
        "the 3.11 corpora are exercised under CPython 3.11 only",
        allow_module_level=True,
    )

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from ci_pipeline.jit311 import corpus

# Exact per-module manifest: case count, raising-case count, and a digest of
# the sorted per-case outcome lines ("name=ok:<value-digest>" /
# "name=raise:TypeName:<message-digest>"), pinned in a committed data file.
# Deleting cases, swapping a case under an unchanged count, flipping a case
# between pass and raise, changing the raised exception type or message, or
# changing a deterministic return value is an explicit manifest edit, never
# an accident a floor would absorb.  Values whose repr is not deterministic
# across processes (objects, sets, dicts) are pinned as their type only.
MANIFEST_PATH = (
    Path(__file__).resolve().parent / "jit311" / "data" / "corpus_manifest.txt"
)


def load_manifest():
    manifest = {}
    for line in MANIFEST_PATH.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        module_name, counts = line.split("=")
        total, raised, name_digest = counts.split(":")
        manifest[module_name] = (int(total), int(raised), name_digest)
    return manifest


def measure():
    counts = {name: [0, 0] for name in corpus.CORPUS_MODULES}
    outcome_lines = {name: [] for name in corpus.CORPUS_MODULES}
    for module_name, case_name, fn in corpus.iter_cases():
        counts[module_name][0] += 1
        outcome = "ok"
        try:
            outcome = f"ok:{stable_digest(stable_repr(fn()))}"
        except BaseException as exc:  # noqa: BLE001
            # Raising is a legitimate case outcome (error-message pinning);
            # which cases raise is itself pinned by the manifest.
            if isinstance(exc, (KeyboardInterrupt, SystemExit)):
                raise AssertionError(
                    f"{module_name}.{case_name} tried to end the process"
                ) from exc
            counts[module_name][1] += 1
            outcome = (
                f"raise:{type(exc).__qualname__}:{stable_digest(str(exc))}"
            )
        outcome_lines[module_name].append(f"{case_name}={outcome}")
    return {
        name: (pair[0], pair[1], digest_outcomes(outcome_lines[name]))
        for name, pair in counts.items()
    }


def digest_outcomes(lines):
    joined = "\n".join(sorted(lines)).encode()
    return hashlib.sha256(joined).hexdigest()[:16]


def stable_repr(value):
    # Deterministic across processes: primitives and sequences thereof.
    # Sets, dicts and arbitrary objects can embed hash-randomized order or
    # addresses, so they pin as their type name only.
    if isinstance(value, (str, bytes, int, float, bool, type(None))):
        return repr(value)
    if isinstance(value, (tuple, list)):
        inner = ",".join(stable_repr(item) for item in value)
        return f"{type(value).__name__}[{inner}]"
    return f"opaque:{type(value).__qualname__}"


def stable_digest(text):
    import re

    normalized = re.sub(r"0x[0-9a-fA-F]+", "0xADDR", text)
    # Absolute file locations (tracebacks!) vary by checkout; keep only the
    # basename so the digest pins the frame identity, not the workspace.
    normalized = re.sub(
        r"(?:/[^/\s\"']+)+/([^/\s\"']+)", r"<path>/\1", normalized
    )
    return hashlib.sha256(normalized.encode()).hexdigest()[:8]


def test_corpus_matches_the_committed_manifest():
    manifest = load_manifest()
    measured = measure()
    assert measured == manifest, {
        name: (measured.get(name), manifest.get(name))
        for name in set(measured) | set(manifest)
        if measured.get(name) != manifest.get(name)
    }


def test_checkpoint_hook_is_installable():
    corpus.load_module("corpus_frames")
    import diffgate_rt

    hits = []
    diffgate_rt.set_checkpoint_hook(lambda: hits.append(1))
    try:
        diffgate_rt.checkpoint()
    finally:
        diffgate_rt.set_checkpoint_hook(None)
    assert hits == [1]
