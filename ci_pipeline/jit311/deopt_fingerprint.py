"""Fail closed on stdlib canary deopt drift, including equal-total drift."""

from collections import Counter


def _site(row, reason_key):
    filename = row["filename"]
    if not isinstance(filename, str):
        raise ValueError("invalid deopt filename")
    # The CPython installation prefix varies across the anchored 3.11 images.
    filename = filename.split("/python3.11/", 1)[-1]
    strings = (filename, row["qualname"], row[reason_key])
    offsets = (row["cause_offset"], row["resume_offset"])
    if not all(isinstance(x, str) for x in strings):
        raise ValueError("invalid deopt identity")
    if not all(type(x) is int and x >= 0 for x in offsets):
        raise ValueError("invalid deopt offsets")
    return (*strings, *offsets)


def check(ledger, manifest, organic_total):
    try:
        if (manifest["format"] != "cp311-stdlib-canary-deopts-v1"
                or manifest["python"] != "3.11.6"
                or type(manifest["total"]) is not int):
            raise ValueError("unexpected deopt manifest format")
        if type(ledger["dropped"]) is not int or ledger["dropped"] != 0:
            raise ValueError("deopt ledger dropped evidence")
        expected = Counter()
        for row in manifest["sites"]:
            key = _site(row, "reason")
            if key in expected or type(row["count"]) is not int or row["count"] <= 0:
                raise ValueError("duplicate or invalid deopt baseline")
            expected[key] = row["count"]
        if type(organic_total) is not int or organic_total != manifest["total"]:
            raise ValueError("deopt total does not match the manifest")
        if sum(expected.values()) != organic_total:
            raise ValueError("deopt baseline does not account for the total")
        actual = Counter()
        for row in ledger["rows"]:
            if row["transition"] != "deopt":
                continue
            if row["forced"] is not False or row["instrumentation"] is not False:
                raise ValueError("unexpected forced/instrumentation deopt")
            actual[_site(row, "deopt_reason")] += 1
        if actual != expected:
            return [f"deopt fingerprint drift: missing={dict(expected - actual)}, "
                    f"unexpected={dict(actual - expected)}"]
    except (KeyError, TypeError, ValueError) as exc:
        return [f"invalid deopt fingerprint evidence: {exc}"]
    return []
