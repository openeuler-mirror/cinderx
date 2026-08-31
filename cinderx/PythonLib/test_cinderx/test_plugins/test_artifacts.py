# Copyright (c) Meta Platforms, Inc. and affiliates.

from __future__ import annotations

import base64
import hashlib
from importlib import metadata
import os
from pathlib import Path
import tempfile
import unittest
from dataclasses import FrozenInstanceError
from typing import Callable
from unittest.mock import patch

from cinderx.plugins import (
    ArtifactBudgets,
    ArtifactIssueCode,
    verify_distribution_closure,
    verify_distribution_closures,
)


FIXTURES = Path(__file__).with_name("fixtures")


def _record_hash(payload: bytes) -> str:
    digest = hashlib.sha256(payload).digest()
    encoded = base64.urlsafe_b64encode(digest).rstrip(b"=").decode("ascii")
    return f"sha256={encoded}"


class FakeDistribution:
    """Small installed-distribution fake with real PackagePath/FileHash records."""

    def __init__(
        self,
        installation_root: Path,
        name: str,
        *,
        requires: tuple[str, ...] = (),
        record_error: Exception | None = None,
    ) -> None:
        self.metadata = {"Name": name, "Version": "1.0"}
        self.requires = list(requires)
        self._installation_root = installation_root
        self._record_error = record_error
        self._record_rows: list[tuple[str, str, str]] = []

    @property
    def files(self) -> tuple[metadata.PackagePath, ...] | None:
        if self._record_error is not None or not self._record_rows:
            return None
        records = []
        for path, hash_spec, size in self._record_rows:
            record = metadata.PackagePath(path)
            record.dist = self  # type: ignore[assignment]
            record.hash = metadata.FileHash(hash_spec) if hash_spec else None
            record.size = int(size) if size else None
            records.append(record)
        return tuple(records)

    def add_file(
        self,
        relative_path: str,
        payload: bytes = b"pass\n",
        *,
        hash_spec: str | None = None,
        size: int | None = None,
    ) -> Path:
        path = self._installation_root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        self._record_rows.append(
            (
                relative_path,
                _record_hash(payload) if hash_spec is None else hash_spec,
                str(len(payload) if size is None else size),
            )
        )
        return path

    def add_record_only(
        self,
        relative_path: str,
        *,
        hash_spec: str = "",
        size: str = "",
    ) -> None:
        self._record_rows.append((relative_path, hash_spec, size))

    def read_text(self, filename: str) -> str | None:
        if filename != "RECORD":
            return None
        if self._record_error is not None:
            raise self._record_error
        if not self._record_rows:
            return None
        return "".join(
            f"{path},{hash_spec},{size}\n"
            for path, hash_spec, size in self._record_rows
        )

    def locate_file(self, path: os.PathLike[str] | str) -> Path:
        return self._installation_root / os.fspath(path)


class FakeMarker:
    def __init__(self, predicate: Callable[[dict[str, str]], bool]) -> None:
        self._predicate = predicate

    def evaluate(self, environment: dict[str, str]) -> bool:
        return self._predicate(environment)


class FakeSpecifier:
    def __init__(self, accepted_version: str | None = None) -> None:
        self._accepted_version = accepted_version

    def contains(self, version: str, prereleases: bool = True) -> bool:
        return self._accepted_version is None or version == self._accepted_version


class FakeRequirement:
    def __init__(
        self,
        name: str,
        *,
        extras: tuple[str, ...] = (),
        marker: FakeMarker | None = None,
        accepted_version: str | None = None,
    ) -> None:
        self.name = name
        self.extras = frozenset(extras)
        self.marker = marker
        self.specifier = FakeSpecifier(accepted_version)


class RequirementParser:
    def __init__(self, requirements: dict[str, FakeRequirement]) -> None:
        self._requirements = requirements

    def __call__(self, value: str) -> FakeRequirement:
        return self._requirements[value]


class ArtifactClosureTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tempdir = tempfile.TemporaryDirectory()
        self.root_path = Path(self._tempdir.name)

    def tearDown(self) -> None:
        self._tempdir.cleanup()

    def distribution(
        self,
        name: str,
        *,
        requires: tuple[str, ...] = (),
        record_error: Exception | None = None,
    ) -> FakeDistribution:
        distribution = FakeDistribution(
            self.root_path,
            name,
            requires=requires,
            record_error=record_error,
        )
        distribution.add_file(f"{name}/__init__.py")
        return distribution

    def test_pure_root_without_packaging_parser_is_accepted(self) -> None:
        root = self.distribution("Pure.Plugin")

        result = verify_distribution_closure(
            root,
            distributions=(root,),
            requirement_parser=None,
            load_default_requirement_parser=lambda: None,
        )

        self.assertTrue(result.accepted)
        self.assertIsNone(result.issue)
        self.assertEqual(result.root_distribution_name, "pure-plugin")
        self.assertEqual(result.closure, ("pure-plugin",))
        self.assertEqual(result.inspected_file_records, 1)
        with self.assertRaises(FrozenInstanceError):
            result.accepted = False  # type: ignore[misc]

    def test_transitive_native_names_exact_distribution_and_path(self) -> None:
        parser = RequirementParser(
            {
                "Middle": FakeRequirement("Middle"),
                "Native_Dep": FakeRequirement("Native.Dep"),
            }
        )
        root = self.distribution("Root", requires=("Middle",))
        middle = self.distribution("Middle", requires=("Native_Dep",))
        native = self.distribution("Native.Dep")
        fixture = (FIXTURES / "native_archive.fixture").read_bytes()
        native.add_file("native_dep/libpayload.a", fixture)

        result = verify_distribution_closure(
            root,
            distributions=(native, root, middle),
            requirement_parser=parser,
        )

        self.assertFalse(result.accepted)
        self.assertIsNotNone(result.issue)
        assert result.issue is not None
        self.assertEqual(result.issue.code, ArtifactIssueCode.NATIVE_IN_CLOSURE)
        self.assertEqual(result.issue.distribution_name, "native-dep")
        self.assertEqual(result.issue.path, "native_dep/libpayload.a")
        self.assertNotIn("sandbox", result.issue.message.casefold())
        self.assertEqual(result.closure, ("middle", "native-dep", "root"))

    def test_marker_false_and_undeclared_native_package_are_excluded(self) -> None:
        parser = RequirementParser(
            {
                "OptionalNative; never": FakeRequirement(
                    "OptionalNative",
                    marker=FakeMarker(lambda environment: False),
                )
            }
        )
        root = self.distribution(
            "Root", requires=("OptionalNative; never",)
        )
        optional = self.distribution("OptionalNative")
        framework = self.distribution("Domain.Framework")
        optional.add_file("optional/native.so")
        framework.add_file("framework/native.dll")

        result = verify_distribution_closure(
            root,
            distributions=(framework, optional, root),
            requirement_parser=parser,
        )

        self.assertTrue(result.accepted)
        self.assertEqual(result.closure, ("root",))
        self.assertEqual(result.inspected_file_records, 1)

    def test_dependency_extras_are_evaluated_deterministically(self) -> None:
        parser = RequirementParser(
            {
                "Middle[fast]": FakeRequirement("Middle", extras=("fast",)),
                "Leaf; extra == 'fast'": FakeRequirement(
                    "Leaf",
                    marker=FakeMarker(
                        lambda environment: environment.get("extra") == "fast"
                    ),
                ),
            }
        )
        root = self.distribution("Root", requires=("Middle[fast]",))
        middle = self.distribution(
            "Middle", requires=("Leaf; extra == 'fast'",)
        )
        leaf = self.distribution("Leaf")

        forward = verify_distribution_closure(
            root,
            distributions=(root, middle, leaf),
            requirement_parser=parser,
        )
        reverse = verify_distribution_closure(
            root,
            distributions=(leaf, middle, root),
            requirement_parser=parser,
        )

        self.assertTrue(forward.accepted)
        self.assertEqual(forward, reverse)
        self.assertEqual(forward.closure, ("leaf", "middle", "root"))

    def test_missing_dependency_and_unavailable_parser_fail_closed(self) -> None:
        root = self.distribution("Root", requires=("Missing",))
        parser = RequirementParser({"Missing": FakeRequirement("Missing")})

        missing = verify_distribution_closure(
            root,
            distributions=(root,),
            requirement_parser=parser,
        )
        unavailable = verify_distribution_closure(
            root,
            distributions=(root,),
            requirement_parser=None,
            load_default_requirement_parser=lambda: None,
        )

        self.assertEqual(
            missing.issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.DEPENDENCY_MISSING,
        )
        self.assertEqual(
            unavailable.issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.DEPENDENCY_PARSER_UNAVAILABLE,
        )

    def test_missing_and_unreadable_record_fail_closed(self) -> None:
        missing = FakeDistribution(self.root_path, "MissingRecord")
        unreadable = self.distribution(
            "UnreadableRecord", record_error=OSError("private filesystem detail")
        )

        missing_result = verify_distribution_closure(
            missing, distributions=(missing,)
        )
        unreadable_result = verify_distribution_closure(
            unreadable, distributions=(unreadable,)
        )

        self.assertEqual(
            missing_result.issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.RECORD_MISSING,
        )
        self.assertEqual(
            unreadable_result.issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.RECORD_UNREADABLE,
        )
        self.assertNotIn(
            "private filesystem detail",
            unreadable_result.issue.message,  # type: ignore[union-attr]
        )

    def test_invalid_absolute_and_parent_record_paths_fail_closed(self) -> None:
        for path in ("../escape.py", "/absolute.py", "C:/absolute.py"):
            with self.subTest(path=path):
                distribution = self.distribution("Root")
                distribution.add_record_only(path)

                result = verify_distribution_closure(
                    distribution, distributions=(distribution,)
                )

                self.assertEqual(
                    result.issue.code,  # type: ignore[union-attr]
                    ArtifactIssueCode.FILE_PATH_INVALID,
                )
                self.assertEqual(result.issue.path, path)  # type: ignore[union-attr]

    @unittest.skipUnless(hasattr(os, "symlink"), "requires symlink support")
    def test_symlink_and_out_of_root_targets_fail_closed(self) -> None:
        outside = Path(self._tempdir.name).with_name("outside-artifact.py")
        outside.write_bytes(b"pass\n")
        self.addCleanup(lambda: outside.unlink(missing_ok=True))
        distribution = self.distribution("Root")
        symlink = self.root_path / "root" / "linked.py"
        symlink.parent.mkdir(parents=True, exist_ok=True)
        symlink.symlink_to(outside)
        distribution.add_record_only("root/linked.py")

        result = verify_distribution_closure(
            distribution, distributions=(distribution,)
        )

        self.assertEqual(
            result.issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.FILE_SYMLINK,
        )

    def test_out_of_root_locate_target_and_unreadable_file_fail_closed(self) -> None:
        distribution = self.distribution("Root")
        distribution.add_record_only("root/external.py")
        original_locate_file = distribution.locate_file
        with tempfile.TemporaryDirectory() as outside_dir:
            outside = Path(outside_dir) / "external.py"
            outside.write_bytes(b"pass\n")

            def locate_file(path: os.PathLike[str] | str) -> Path:
                if os.fspath(path) == "root/external.py":
                    return outside
                return original_locate_file(path)

            distribution.locate_file = locate_file  # type: ignore[method-assign]
            outside_result = verify_distribution_closure(
                distribution, distributions=(distribution,)
            )

        unreadable = self.distribution("Unreadable")
        with patch.object(Path, "open", side_effect=PermissionError("private")):
            unreadable_result = verify_distribution_closure(
                unreadable, distributions=(unreadable,)
            )

        self.assertEqual(
            outside_result.issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.FILE_OUTSIDE_ROOT,
        )
        self.assertEqual(
            unreadable_result.issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.FILE_UNREADABLE,
        )
        self.assertNotIn(
            "private",
            unreadable_result.issue.message,  # type: ignore[union-attr]
        )

    def test_missing_file_and_hash_mismatch_fail_closed(self) -> None:
        missing = self.distribution("MissingFile")
        missing.add_record_only("missing_file/not-there.py")
        mismatch = self.distribution("Mismatch")
        mismatch.add_file(
            "mismatch/tampered.py",
            b"tampered\n",
            hash_spec="sha256=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        )

        missing_result = verify_distribution_closure(
            missing, distributions=(missing,)
        )
        mismatch_result = verify_distribution_closure(
            mismatch, distributions=(mismatch,)
        )

        self.assertEqual(
            missing_result.issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.FILE_MISSING,
        )
        self.assertEqual(
            mismatch_result.issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.HASH_MISMATCH,
        )

    def test_native_magic_and_canonical_suffixes_fail_closed(self) -> None:
        payloads = {
            "payload.bin": b"\x7fELF" + b"\0" * 64,
            "payload.dat": b"MZ" + b"\0" * 64,
            "payload.raw": b"\xfe\xed\xfa\xcf" + b"\0" * 64,
            "payload.pkg": b"!<arch>\nmember",
            "payload.coff": b"\x64\x86\x01\x00" + b"\0" * 64,
            "payload.so": (FIXTURES / "native_suffix.fixture").read_bytes(),
            "libpayload.so.1": b"versioned shared library suffix",
            "PAYLOAD.DLL": b"suffixes are case insensitive",
        }
        for index, (filename, payload) in enumerate(payloads.items()):
            with self.subTest(filename=filename):
                distribution = self.distribution(f"Native{index}")
                relative_path = f"native{index}/{filename}"
                distribution.add_file(relative_path, payload)

                result = verify_distribution_closure(
                    distribution, distributions=(distribution,)
                )

                self.assertEqual(
                    result.issue.code,  # type: ignore[union-attr]
                    ArtifactIssueCode.NATIVE_IN_CLOSURE,
                )
                self.assertEqual(
                    result.issue.path,  # type: ignore[union-attr]
                    relative_path,
                )

    def test_noncanonical_so_substring_is_not_a_native_suffix(self) -> None:
        distribution = self.distribution("Root")
        distribution.add_file("root/notes.so.backup", b"plain data")

        result = verify_distribution_closure(
            distribution, distributions=(distribution,)
        )

        self.assertTrue(result.accepted)

    def test_node_edge_and_file_budgets_are_independent(self) -> None:
        parser = RequirementParser(
            {
                "Left": FakeRequirement("Left"),
                "Right": FakeRequirement("Right"),
            }
        )
        root = self.distribution("Root", requires=("Left", "Right"))
        left = self.distribution("Left")
        right = self.distribution("Right")
        left.add_file("left/second.py")

        node_result = verify_distribution_closure(
            root,
            distributions=(root, left, right),
            requirement_parser=parser,
            budgets=ArtifactBudgets(max_nodes=2, max_edges=10, max_file_records=10),
        )
        edge_result = verify_distribution_closure(
            root,
            distributions=(root, left, right),
            requirement_parser=parser,
            budgets=ArtifactBudgets(max_nodes=10, max_edges=1, max_file_records=10),
        )
        file_result = verify_distribution_closure(
            left,
            distributions=(left,),
            budgets=ArtifactBudgets(max_nodes=10, max_edges=10, max_file_records=1),
        )

        self.assertEqual(
            node_result.issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.NODE_BUDGET_EXCEEDED,
        )
        self.assertEqual(
            edge_result.issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.EDGE_BUDGET_EXCEEDED,
        )
        self.assertEqual(
            file_result.issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.FILE_BUDGET_EXCEEDED,
        )

    def test_bad_root_does_not_raise_or_truncate_sibling_verification(self) -> None:
        bad = self.distribution("Bad", record_error=RuntimeError("boom"))
        good = self.distribution("Good")

        results = verify_distribution_closures(
            (good, bad), distributions=(bad, good)
        )

        self.assertEqual(
            [result.root_distribution_name for result in results],
            ["bad", "good"],
        )
        self.assertFalse(results[0].accepted)
        self.assertEqual(
            results[0].issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.RECORD_UNREADABLE,
        )
        self.assertTrue(results[1].accepted)


    @unittest.skipUnless(os.name == "posix", "requires POSIX executable bits")
    def test_extensionless_executable_fails_closed(self) -> None:
        distribution = self.distribution("Executable")
        executable = distribution.add_file(
            "executable/neutral-tool", b"plain non-native payload"
        )
        executable.chmod(0o755)

        result = verify_distribution_closure(
            distribution, distributions=(distribution,)
        )

        self.assertEqual(
            result.issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.EXECUTABLE_IN_CLOSURE,
        )
        self.assertEqual(
            result.issue.path,  # type: ignore[union-attr]
            "executable/neutral-tool",
        )
        self.assertNotIn(
            "sandbox",
            result.issue.message.casefold(),  # type: ignore[union-attr]
        )

    def test_duplicate_normalized_root_identity_is_deterministic(self) -> None:
        first = self.distribution("Root")
        second = self.distribution("ROOT")

        single = verify_distribution_closure(
            first, distributions=(first, second)
        )
        forward = verify_distribution_closures(
            (first, second), distributions=()
        )
        reverse = verify_distribution_closures(
            (second, first), distributions=()
        )

        self.assertEqual(
            single.issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.DEPENDENCY_AMBIGUOUS,
        )
        self.assertEqual(forward, reverse)
        self.assertEqual(len(forward), 2)
        self.assertTrue(
            all(
                result.issue is not None
                and result.issue.code
                is ArtifactIssueCode.DEPENDENCY_AMBIGUOUS
                for result in forward
            )
        )

    def test_partial_root_enumeration_preserves_yielded_results(self) -> None:
        good = self.distribution("Good")

        def roots():
            yield good
            raise OSError("private root source detail")

        results = verify_distribution_closures(
            roots(), distributions=(good,)
        )

        self.assertEqual(len(results), 2)
        self.assertTrue(results[0].accepted)
        self.assertEqual(results[0].root_distribution_name, "good")
        self.assertEqual(results[1].root_distribution_name, "<unknown>")
        self.assertEqual(
            results[1].issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.DEPENDENCY_ENUMERATION_UNREADABLE,
        )
        self.assertNotIn(
            "private root source detail",
            results[1].issue.message,  # type: ignore[union-attr]
        )

    def test_requirement_row_and_record_payload_have_byte_budgets(self) -> None:
        giant_requirement = "Dependency" + "x" * 128
        requirement_root = self.distribution(
            "RequirementRoot", requires=(giant_requirement,)
        )
        parser_called = False

        def parser(value: str) -> FakeRequirement:
            nonlocal parser_called
            parser_called = True
            return FakeRequirement(value)

        requirement_result = verify_distribution_closure(
            requirement_root,
            distributions=(requirement_root,),
            requirement_parser=parser,
            budgets=ArtifactBudgets(max_requirement_bytes=32),
        )

        record_root = self.distribution("RecordRoot")
        record_result = verify_distribution_closure(
            record_root,
            distributions=(record_root,),
            budgets=ArtifactBudgets(max_record_bytes=8),
        )

        self.assertFalse(parser_called)
        self.assertEqual(
            requirement_result.issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.REQUIREMENT_BUDGET_EXCEEDED,
        )
        self.assertEqual(
            record_result.issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.RECORD_BUDGET_EXCEEDED,
        )

    def test_path_distribution_record_is_bounded_before_text_read(self) -> None:
        class GuardedPathDistribution(metadata.PathDistribution):
            def read_text(self, filename: str) -> str | None:
                if filename == "RECORD":
                    raise AssertionError("unbounded RECORD text read")
                return super().read_text(filename)

        dist_info = self.root_path / "bounded-1.0.dist-info"
        dist_info.mkdir()
        (dist_info / "METADATA").write_text(
            "Metadata-Version: 2.1\nName: Bounded\nVersion: 1.0\n",
            encoding="utf-8",
        )
        package_file = self.root_path / "bounded" / "__init__.py"
        package_file.parent.mkdir()
        payload = b"pass\n"
        package_file.write_bytes(payload)
        record_payload = (
            f"bounded/__init__.py,{_record_hash(payload)},{len(payload)}\n"
        )
        (dist_info / "RECORD").write_text(record_payload, encoding="utf-8")
        distribution = GuardedPathDistribution(dist_info)

        result = verify_distribution_closure(
            distribution,
            distributions=(distribution,),
            budgets=ArtifactBudgets(
                max_record_bytes=len(record_payload.encode("utf-8")) - 1
            ),
        )

        self.assertEqual(
            result.issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.RECORD_BUDGET_EXCEEDED,
        )

    def test_batch_caches_shared_distribution_file_inspection(self) -> None:
        parser = RequirementParser({"Shared": FakeRequirement("Shared")})
        first = self.distribution("First", requires=("Shared",))
        second = self.distribution("Second", requires=("Shared",))
        shared = self.distribution("Shared")
        shared_path = self.root_path / "Shared" / "__init__.py"
        open_count = 0
        original_open = Path.open

        def counting_open(
            path: Path, *args: object, **kwargs: object
        ):
            nonlocal open_count
            if path == shared_path:
                open_count += 1
            return original_open(path, *args, **kwargs)

        with patch.object(Path, "open", counting_open):
            results = verify_distribution_closures(
                (second, first),
                distributions=(shared, second, first),
                requirement_parser=parser,
            )

        self.assertTrue(all(result.accepted for result in results))
        self.assertEqual(
            [result.inspected_file_records for result in results], [2, 2]
        )
        self.assertEqual(open_count, 1)

    def test_requirement_parse_cache_rechecks_markers_for_extras(self) -> None:
        marker_extras: list[str] = []

        def marker_predicate(environment: dict[str, str]) -> bool:
            marker_extras.append(environment.get("extra", ""))
            return environment.get("extra") == "fast"

        parsed = {
            "Middle": FakeRequirement("Middle"),
            "Provider": FakeRequirement("Provider"),
            "Middle[fast]": FakeRequirement("Middle", extras=("fast",)),
            "Leaf; extra == 'fast'": FakeRequirement(
                "Leaf",
                marker=FakeMarker(marker_predicate),
            ),
        }
        parser_calls: dict[str, int] = {}

        def parser(value: str) -> FakeRequirement:
            parser_calls[value] = parser_calls.get(value, 0) + 1
            return parsed[value]

        first = self.distribution(
            "First", requires=("Middle", "Provider")
        )
        second = self.distribution(
            "Second", requires=("Middle", "Provider")
        )
        middle = self.distribution(
            "Middle", requires=("Leaf; extra == 'fast'",)
        )
        provider = self.distribution(
            "Provider", requires=("Middle[fast]",)
        )
        leaf = self.distribution("Leaf")

        results = verify_distribution_closures(
            (second, first),
            distributions=(leaf, provider, middle, second, first),
            requirement_parser=parser,
        )

        self.assertTrue(all(result.accepted for result in results))
        self.assertEqual(
            parser_calls,
            {
                "Leaf; extra == 'fast'": 1,
                "Middle": 1,
                "Middle[fast]": 1,
                "Provider": 1,
            },
        )
        self.assertEqual(marker_extras, ["", "fast", "", "fast"])

    def test_single_and_batch_share_distribution_enumeration_reason(self) -> None:
        root = self.distribution("Root")

        with patch(
            "cinderx.plugins.artifacts.metadata.distributions",
            side_effect=OSError("private enumeration detail"),
        ):
            single = verify_distribution_closure(root)
            batch = verify_distribution_closures((root,))

        self.assertEqual(
            single.issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.DEPENDENCY_ENUMERATION_UNREADABLE,
        )
        self.assertEqual(
            batch[0].issue.code,  # type: ignore[union-attr]
            ArtifactIssueCode.DEPENDENCY_ENUMERATION_UNREADABLE,
        )
        self.assertNotIn(
            "private enumeration detail",
            single.issue.message,  # type: ignore[union-attr]
        )


if __name__ == "__main__":
    unittest.main()
