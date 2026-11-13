"""Require an unmodified, successful CPython -R result for every module."""

import argparse
from pathlib import Path
import re


def check(module, text, returncode):
    if returncode != 0:
        return False, f"{module}: regrtest exited {returncode}"
    if re.search(r"leaked \[[^\]]*\] (references|memory blocks|file descriptors)", text):
        return False, f"{module}: reported leak; no counter-artifact exemption"
    if re.search(r"^FAIL:|^ERROR:|^Traceback |^test .* failed|crashed|timed out", text, re.M):
        return False, f"{module}: ordinary failure in regrtest output"
    if (re.findall(r"^== Tests result: (\w+) ==$", text, re.M) != ["SUCCESS"]
            or re.findall(r"^Result: (\w+)$", text, re.M) != ["SUCCESS"]
            or re.findall(r"^Total test files: (.*)$", text, re.M)
            not in (["success=1"], ["success=1 failed=0"])):
        return False, f"{module}: incomplete or non-successful single-module result"
    counts = re.findall(r"^Total tests: run=(\d+)", text, re.M)
    if len(counts) != 1 or int(counts[0]) == 0:
        return False, f"{module}: no completed test cases"
    return True, f"{module}: original regrtest passed without any reported leak"


def check_attestation(module, text):
    matched = 0
    for line in text.splitlines():
        fields = line.split()
        if not fields:
            continue
        if len(fields) != 6:
            return False, f"{module}: malformed attestation row"
        pid, installed, entries, creations, isolated, subject = fields
        if (any(re.fullmatch(r"[0-9]+", value) is None
                for value in (pid, entries, creations))
                or int(pid) <= 0
                or installed not in ("True", "False", "unavailable")
                or isolated not in ("True", "False")):
            return False, f"{module}: invalid attestation fields"
        if subject != module:
            continue  # A helper subprocess may not have called -R.
        matched += 1
        if installed != "True" or int(entries) <= 0 or isolated != "True":
            return False, f"{module}: unproven original -R process {pid}"
    if not matched:
        return False, f"{module}: no attested original -R process"
    return True, f"{module}: {matched} original -R process(es) entered JIT with interpreted measurement"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--logs", type=Path)
    source.add_argument("--attest", type=Path)
    parser.add_argument("modules", nargs="+")
    args = parser.parse_args()
    if args.attest is not None:
        if len(args.modules) != 1:
            parser.error("--attest requires exactly one module")
        module = args.modules[0]
        try:
            ok, why = check_attestation(
                module, args.attest.read_text(encoding="utf-8")
            )
        except (OSError, ValueError) as exc:
            ok, why = False, f"{module}: missing attestation: {exc}"
        print(("PASS: " if ok else "FAIL: ") + why)
        return 0 if ok else 1
    passed = True
    for module in args.modules:
        try:
            text = (args.logs / f"{module}.log").read_text()
            rc = int((args.logs / f"{module}.exitcode").read_text())
            ok, why = check(module, text, rc)
        except (OSError, ValueError) as exc:
            ok, why = False, f"{module}: missing original result: {exc}"
        print(("PASS: " if ok else "FAIL: ") + why)
        passed = passed and ok
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
