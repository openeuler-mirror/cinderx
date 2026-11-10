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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--logs", type=Path, required=True)
    parser.add_argument("modules", nargs="+")
    args = parser.parse_args()
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
