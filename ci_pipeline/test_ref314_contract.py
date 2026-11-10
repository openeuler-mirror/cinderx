"""Fail-closed regressions for explicitly reviewed reference-line additions."""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from ci_pipeline.ref314_contract import (
    check_execution,
    check_goldens,
    check_registered,
)


class ReferenceContractTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name) / "base"
        self.head = Path(self.temp.name) / "head"
        self.base.mkdir()
        self.head.mkdir()
        for root in (self.base, self.head):
            (root / "old.txt").write_bytes(b"old\n")
        (self.head / "new.txt").write_bytes(b"new\n")
        self.contract = {
            "golden_additions": {"new.txt": hashlib.sha256(b"new\n").hexdigest()},
            "test_additions": {"New.Pass": "pass", "New.Skip": "skip"},
        }
        self.before = ["Old.Test"]
        self.after = self.before + ["New.Pass", "New.Skip"]
        self.log = (
            "[ RUN      ] New.Pass\n[       OK ] New.Pass (0 ms)\n"
            "[ RUN      ] New.Skip\n[  SKIPPED ] New.Skip (0 ms)\n"
        )

    def test_reviewed_additions_and_already_integrated_base(self):
        check_goldens(self.base, self.head, self.contract)
        check_goldens(self.head, self.head, self.contract)
        check_execution(self.before, self.after, self.log, self.contract)
        self.assertEqual(check_registered(self.after, self.after, self.contract), set())

    def test_existing_golden_change_or_deletion_rejected(self):
        for data in (b"changed", None):
            with self.subTest(data=data):
                target = self.head / "old.txt"
                if data is None:
                    target.unlink()
                else:
                    target.write_bytes(data)
                with self.assertRaises(ValueError):
                    check_goldens(self.base, self.head, self.contract)

    def test_new_golden_missing_changed_or_unexpected_rejected(self):
        target = self.head / "new.txt"
        target.write_bytes(b"changed")
        with self.assertRaises(ValueError):
            check_goldens(self.base, self.head, self.contract)
        target.unlink()
        with self.assertRaises(ValueError):
            check_goldens(self.base, self.head, self.contract)
        target.write_bytes(b"new\n")
        (self.head / "surprise.txt").write_bytes(b"surprise")
        with self.assertRaises(ValueError):
            check_goldens(self.base, self.head, self.contract)

    def test_registration_deletion_replacement_and_unexpected_addition_rejected(self):
        for names in (self.after[1:], self.after[:-1], self.after + ["Other.Test"],
                      self.after + ["New.Pass"]):
            with self.subTest(names=names), self.assertRaises(ValueError):
                check_registered(self.before, names, self.contract)

    def test_disabled_addition_is_rejected_even_when_declared(self):
        self.contract["test_additions"] = {"New.DISABLED_Test": "pass"}
        with self.assertRaisesRegex(ValueError, "disabled"):
            check_registered(self.before, self.before + ["New.DISABLED_Test"], self.contract)

    def test_new_tests_must_really_run_and_have_exact_status(self):
        for log in ("", self.log.replace("[ RUN      ] New.Pass\n", ""),
                    self.log.replace("[       OK ]", "[  SKIPPED ]"),
                    self.log.replace("[  SKIPPED ]", "[       OK ]"),
                    self.log + "[ RUN      ] New.Pass\n"):
            with self.subTest(log=log), self.assertRaises(ValueError):
                check_execution(self.before, self.after, log, self.contract)

    @unittest.skipIf(os.name == "nt", "the gate's log comparator requires POSIX bash")
    def test_cli_reconciliation_retains_legacy_failures_skips_and_diagnostics(self):
        root = Path(self.temp.name)
        repo = Path(__file__).resolve().parent.parent
        contract = root / "contract.json"
        contract.write_text(json.dumps(self.contract), encoding="utf-8")
        base_names, head_names = root / "base.txt", root / "head.txt"
        base_names.write_text("\n".join(self.before), encoding="utf-8")
        head_names.write_text("\n".join(self.after), encoding="utf-8")
        base_log, head_log, legacy_log = (root / n for n in ("base.log", "head.log", "legacy.log"))
        old = "[ RUN      ] Old.Test\n[       OK ] Old.Test (0 ms)\n"
        allowlist = root / "allowlist.txt"
        allowlist.write_text("Old.Test\n", encoding="utf-8")

        def compare(base, head):
            base_log.write_text(base, encoding="utf-8")
            head_log.write_text(head + self.log, encoding="utf-8")
            subprocess.run(
                [sys.executable, str(repo / "ci_pipeline/ref314_contract.py"),
                 "execution", str(contract), str(base_names), str(head_names),
                 "--log", str(head_log), "--legacy-log", str(legacy_log)],
                check=True, capture_output=True, timeout=30,
            )
            self.assertIn(head, legacy_log.read_text(encoding="utf-8"))
            return subprocess.run(
                ["bash", str(repo / "ci_pipeline/scripts/rt314_differential.sh"),
                 "--verify-logs", str(base_log), str(legacy_log), str(allowlist)],
                capture_output=True, timeout=30,
            ).returncode

        self.assertEqual(compare(old, old), 0)
        self.assertEqual(compare(old, old.replace("[       OK ]", "[  SKIPPED ]")), 1)
        failed = "[ RUN      ] Old.Test\nold.cpp:10: invariant A\n[  FAILED  ] Old.Test (0 ms)\n"
        self.assertEqual(compare(old, failed), 1)
        self.assertEqual(compare(failed, failed), 0)
        self.assertEqual(compare(failed, failed.replace("invariant A", "invariant B")), 1)


if __name__ == "__main__":
    unittest.main()
