"""Reconcile reviewed test additions without weakening the reference line.

Existing goldens remain byte-identical. Only hash-pinned new goldens and exact
new test identities are accepted. Every addition must execute with its declared
status; a declaration already present in the base is inert.
"""

import argparse
import hashlib
import json
import re
from pathlib import Path


def check_goldens(base, head, contract):
    def inventory(root):
        return {
            p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob("*") if p.is_file()
        }

    before, after = inventory(base), inventory(head)
    additions = contract["golden_additions"]
    for name, digest in before.items():
        if after.get(name) != digest:
            raise ValueError(f"existing golden removed or changed: {name}")
    for name in after.keys() - before.keys():
        if additions.get(name) != after[name]:
            raise ValueError(f"undeclared or changed new golden: {name}")
    for name, digest in additions.items():
        if after.get(name) != digest:
            raise ValueError(f"declared golden missing or hash changed: {name}")


def check_registered(base, head, contract):
    if len(base) != len(set(base)) or len(head) != len(set(head)):
        raise ValueError("duplicate registered identity")
    before, after = set(base), set(head)
    declared = set(contract["test_additions"])
    if before - after:
        raise ValueError(f"registered tests removed: {sorted(before - after)}")
    if after - before != declared - before:
        raise ValueError(f"unexpected test additions: {sorted(after - before)}")
    if not declared <= after:
        raise ValueError("declared tests missing from head")
    if any("DISABLED_" in name for name in declared):
        raise ValueError("disabled additions are not executable evidence")
    return declared - before


def check_execution(base, head, log, contract):
    additions = check_registered(base, head, contract)
    for name in additions:
        expected = contract["test_additions"][name]
        marker = "OK" if expected == "pass" else "SKIPPED"
        if expected not in ("pass", "skip"):
            raise ValueError(f"invalid expected status: {name}")
        if len(re.findall(r"^\[ RUN      \] " + re.escape(name) + r"$", log, re.M)) != 1:
            raise ValueError(f"addition did not execute exactly once: {name}")
        if not re.search(r"^\[\s*" + marker + r"\s*\] " + re.escape(name) + r"(?: \(|$)", log, re.M):
            raise ValueError(f"addition did not {expected}: {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("goldens", "registered", "execution"))
    parser.add_argument("contract", type=Path)
    parser.add_argument("base", type=Path)
    parser.add_argument("head", type=Path)
    parser.add_argument("--log", type=Path)
    parser.add_argument("--legacy-log", type=Path)
    args = parser.parse_args()
    contract = json.loads(args.contract.read_text(encoding="utf-8"))
    if args.stage == "goldens":
        check_goldens(args.base, args.head, contract)
    else:
        base = args.base.read_text(encoding="utf-8").splitlines()
        head = args.head.read_text(encoding="utf-8").splitlines()
        additions = check_registered(base, head, contract)
        if args.stage == "execution":
            log = args.log.read_text(encoding="utf-8")
            check_execution(base, head, log, contract)
            # Compare old skip/failure/diagnostic sets with the original strict
            # checker. Only approved new identities are removed from that view.
            lines = [line for line in log.splitlines()
                     if not any(re.match(r"^\[.*?\] " + re.escape(name) + r"(?: \(|$)", line)
                                for name in additions)]
            args.legacy_log.write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
