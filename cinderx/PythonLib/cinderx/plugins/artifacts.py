# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict
"""Deterministic native-free checks over installed distribution closures.

This is an admission-time governance check for trusted installed packages. It
does not isolate Python code and is not a security sandbox.
"""

from __future__ import annotations

import base64
import csv
from dataclasses import dataclass, field
from enum import Enum
import hashlib
from importlib import metadata
import io
import os
from pathlib import Path, PurePosixPath
import re
from typing import Callable, Iterable

from .discovery import normalize_distribution_name


MAX_CLOSURE_NODES = 128
MAX_CLOSURE_EDGES = 512
MAX_INSPECTED_FILE_RECORDS = 10_000
MAX_RECORD_BYTES = 16 * 1024 * 1024
MAX_REQUIREMENT_BYTES = 16 * 1024
MAX_DIAGNOSTIC_BYTES = 512
MAX_EVIDENCE_PATH_BYTES = 256

_UNKNOWN_DISTRIBUTION_NAME = "<unknown>"
_WINDOWS_ABSOLUTE_PATH = re.compile(r"^[A-Za-z]:[/\\]")
_VERSIONED_SO_SUFFIX = re.compile(r"\.so(?:\.\d+)+$")
_NATIVE_SUFFIXES = frozenset(
    {
        ".a",
        ".bundle",
        ".dll",
        ".dso",
        ".dylib",
        ".exe",
        ".lib",
        ".o",
        ".obj",
        ".pyd",
        ".so",
    }
)
_MACH_O_MAGICS = frozenset(
    {
        b"\xca\xfe\xba\xbe",
        b"\xbe\xba\xfe\xca",
        b"\xca\xfe\xba\xbf",
        b"\xbf\xba\xfe\xca",
        b"\xfe\xed\xfa\xce",
        b"\xce\xfa\xed\xfe",
        b"\xfe\xed\xfa\xcf",
        b"\xcf\xfa\xed\xfe",
    }
)
_COFF_MACHINE_MAGICS = frozenset(
    {
        b"\x4c\x01",  # IMAGE_FILE_MACHINE_I386
        b"\x64\x86",  # IMAGE_FILE_MACHINE_AMD64
        b"\xc0\x01",  # IMAGE_FILE_MACHINE_ARM
        b"\xc2\x01",  # IMAGE_FILE_MACHINE_THUMB
        b"\x64\xaa",  # IMAGE_FILE_MACHINE_ARM64
    }
)


class ArtifactIssueCode(str, Enum):
    """Stable machine-readable closure and installed-file failures."""

    DEPENDENCY_ENUMERATION_UNREADABLE = "dependency_enumeration_unreadable"
    DEPENDENCY_METADATA_UNREADABLE = "dependency_metadata_unreadable"
    DEPENDENCY_PARSER_UNAVAILABLE = "dependency_parser_unavailable"
    DEPENDENCY_UNVERIFIABLE = "dependency_unverifiable"
    DEPENDENCY_MISSING = "dependency_missing"
    DEPENDENCY_AMBIGUOUS = "dependency_ambiguous"
    DEPENDENCY_VERSION_MISMATCH = "dependency_version_mismatch"
    NODE_BUDGET_EXCEEDED = "node_budget_exceeded"
    EDGE_BUDGET_EXCEEDED = "edge_budget_exceeded"
    FILE_BUDGET_EXCEEDED = "file_budget_exceeded"
    REQUIREMENT_BUDGET_EXCEEDED = "requirement_budget_exceeded"
    RECORD_BUDGET_EXCEEDED = "record_budget_exceeded"
    RECORD_MISSING = "record_missing"
    RECORD_UNREADABLE = "record_unreadable"
    RECORD_INVALID = "record_invalid"
    DISTRIBUTION_ROOT_UNREADABLE = "distribution_root_unreadable"
    FILE_PATH_INVALID = "file_path_invalid"
    FILE_MISSING = "file_missing"
    FILE_UNREADABLE = "file_unreadable"
    FILE_SYMLINK = "file_symlink"
    FILE_OUTSIDE_ROOT = "file_outside_root"
    HASH_UNVERIFIABLE = "hash_unverifiable"
    HASH_MISMATCH = "hash_mismatch"
    SIZE_MISMATCH = "size_mismatch"
    EXECUTABLE_IN_CLOSURE = "executable_in_closure"
    NATIVE_IN_CLOSURE = "native_in_closure"
    INTERNAL_ERROR = "internal_error"


@dataclass(frozen=True, slots=True)
class ArtifactBudgets:
    """Independent limits applied anew to each plugin root."""

    max_nodes: int = MAX_CLOSURE_NODES
    max_edges: int = MAX_CLOSURE_EDGES
    max_file_records: int = MAX_INSPECTED_FILE_RECORDS
    max_record_bytes: int = MAX_RECORD_BYTES
    max_requirement_bytes: int = MAX_REQUIREMENT_BYTES


@dataclass(frozen=True, slots=True)
class ArtifactIssue:
    code: ArtifactIssueCode
    distribution_name: str
    path: str | None
    message: str


@dataclass(frozen=True, slots=True)
class ArtifactVerificationResult:
    """Immutable outcome of verifying one declared distribution closure."""

    root_distribution_name: str
    accepted: bool
    issue: ArtifactIssue | None
    closure: tuple[str, ...]
    inspected_file_records: int


@dataclass(frozen=True, slots=True)
class _ClosureResolution:
    distributions: tuple[tuple[str, metadata.Distribution], ...]
    issue: ArtifactIssue | None


@dataclass(frozen=True, slots=True)
class _RawRequirements:
    values: tuple[str, ...] | None
    issue: ArtifactIssue | None


@dataclass(frozen=True, slots=True)
class _ParsedRequirement:
    requirement: object | None
    dependency_name: str | None
    dependency_extras: frozenset[str]
    marker: object | None


@dataclass(frozen=True, slots=True)
class _RecordRows:
    rows: tuple[tuple[str, str, int | None], ...]
    issue: ArtifactIssue | None


@dataclass(frozen=True, slots=True)
class _DistributionInspection:
    record_rows: tuple[tuple[str, str, int | None], ...]
    records_complete: bool
    issue: ArtifactIssue | None
    inspected_file_records: int


@dataclass(slots=True)
class _VerificationCache:
    requirement_parser: RequirementParser | None
    parser_loaded: bool
    parsed_requirements: dict[str, _ParsedRequirement] = field(
        default_factory=dict
    )
    raw_requirements: dict[
        int, tuple[metadata.Distribution, _RawRequirements]
    ] = field(default_factory=dict)
    inspections: dict[
        int, tuple[metadata.Distribution, _DistributionInspection]
    ] = field(default_factory=dict)


RequirementParser = Callable[[str], object]
RequirementParserLoader = Callable[[], RequirementParser | None]


def _bounded_text(value: str, limit: int) -> str:
    encoded = value.encode("utf-8", errors="replace")
    if len(encoded) <= limit:
        return value
    marker = b"..."
    prefix = encoded[: max(0, limit - len(marker))]
    while prefix:
        try:
            return prefix.decode("utf-8") + marker.decode("ascii")
        except UnicodeDecodeError:
            prefix = prefix[:-1]
    return marker[:limit].decode("ascii")


def _distribution_name(distribution: metadata.Distribution) -> str:
    try:
        name = distribution.metadata.get("Name")
    except Exception:
        name = None
    if not isinstance(name, str) or not name:
        name = _UNKNOWN_DISTRIBUTION_NAME
    return normalize_distribution_name(name)


def _distribution_version(distribution: metadata.Distribution) -> str:
    try:
        version = distribution.metadata.get("Version")
    except Exception:
        version = None
    return version if isinstance(version, str) else ""


def _issue(
    code: ArtifactIssueCode,
    distribution_name: str,
    message: str,
    *,
    path: str | None = None,
) -> ArtifactIssue:
    return ArtifactIssue(
        code=code,
        distribution_name=_bounded_text(
            distribution_name, MAX_DIAGNOSTIC_BYTES
        ),
        path=(
            _bounded_text(path, MAX_EVIDENCE_PATH_BYTES)
            if path is not None
            else None
        ),
        message=_bounded_text(message, MAX_DIAGNOSTIC_BYTES),
    )


def _rejected(
    root_name: str,
    issue: ArtifactIssue,
    closure: Iterable[str],
    inspected: int = 0,
) -> ArtifactVerificationResult:
    return ArtifactVerificationResult(
        root_distribution_name=root_name,
        accepted=False,
        issue=issue,
        closure=tuple(sorted(closure)),
        inspected_file_records=inspected,
    )


def _accepted(
    root_name: str,
    closure: Iterable[str],
    inspected: int,
) -> ArtifactVerificationResult:
    return ArtifactVerificationResult(
        root_distribution_name=root_name,
        accepted=True,
        issue=None,
        closure=tuple(sorted(closure)),
        inspected_file_records=inspected,
    )


def _load_default_requirement_parser() -> RequirementParser | None:
    try:
        from packaging.requirements import Requirement
    except Exception:
        return None
    return Requirement


def _materialize(
    values: Iterable[metadata.Distribution],
) -> tuple[tuple[metadata.Distribution, ...], bool]:
    result: list[metadata.Distribution] = []
    try:
        iterator = iter(values)
    except Exception:
        return (), False
    while True:
        try:
            result.append(next(iterator))
        except StopIteration:
            return tuple(result), True
        except Exception:
            return tuple(result), False


def _materialize_installed(
    distributions: Iterable[metadata.Distribution] | None,
) -> tuple[tuple[metadata.Distribution, ...], bool]:
    try:
        installed = (
            metadata.distributions() if distributions is None else distributions
        )
    except Exception:
        return (), False
    return _materialize(installed)


def _raw_requirements(
    distribution: metadata.Distribution,
    count_limit: int,
    byte_limit: int,
) -> _RawRequirements:
    name = _distribution_name(distribution)
    try:
        values = distribution.requires
    except Exception:
        return _RawRequirements(
            None,
            _issue(
                ArtifactIssueCode.DEPENDENCY_METADATA_UNREADABLE,
                name,
                "could not read installed Requires-Dist metadata",
            ),
        )
    if values is None:
        return _RawRequirements((), None)
    if isinstance(values, (str, bytes)):
        return _RawRequirements(
            None,
            _issue(
                ArtifactIssueCode.DEPENDENCY_UNVERIFIABLE,
                name,
                "installed Requires-Dist metadata is not a sequence of strings",
            ),
        )
    requirements: list[str] = []
    try:
        for value in values:
            if not isinstance(value, str):
                return _RawRequirements(
                    None,
                    _issue(
                        ArtifactIssueCode.DEPENDENCY_UNVERIFIABLE,
                        name,
                        "installed Requires-Dist metadata contains a non-text value",
                    ),
                )
            try:
                encoded_size = len(value.encode("utf-8"))
            except Exception:
                return _RawRequirements(
                    None,
                    _issue(
                        ArtifactIssueCode.DEPENDENCY_UNVERIFIABLE,
                        name,
                        "installed Requires-Dist metadata contains invalid text",
                    ),
                )
            if encoded_size > byte_limit:
                return _RawRequirements(
                    None,
                    _issue(
                        ArtifactIssueCode.REQUIREMENT_BUDGET_EXCEEDED,
                        name,
                        "a Requires-Dist entry exceeded the byte budget",
                    ),
                )
            requirements.append(value)
            if len(requirements) > count_limit:
                return _RawRequirements(
                    None,
                    _issue(
                        ArtifactIssueCode.EDGE_BUDGET_EXCEEDED,
                        name,
                        "declared closure exceeded the dependency-edge budget",
                    ),
                )
    except Exception:
        return _RawRequirements(
            None,
            _issue(
                ArtifactIssueCode.DEPENDENCY_METADATA_UNREADABLE,
                name,
                "could not enumerate installed Requires-Dist metadata",
            ),
        )
    return _RawRequirements(tuple(requirements), None)


def _cached_raw_requirements(
    distribution: metadata.Distribution,
    budgets: ArtifactBudgets,
    cache: _VerificationCache,
) -> _RawRequirements:
    cache_key = id(distribution)
    cached = cache.raw_requirements.get(cache_key)
    if cached is not None and cached[0] is distribution:
        return cached[1]
    result = _raw_requirements(
        distribution,
        max(0, budgets.max_edges) + 1,
        max(0, budgets.max_requirement_bytes),
    )
    cache.raw_requirements[cache_key] = (distribution, result)
    return result


def _marker_applies(marker: object, extras: frozenset[str]) -> bool:
    evaluate = marker.evaluate  # type: ignore[attr-defined]
    environments = sorted(extras) if extras else [""]
    for extra in environments:
        try:
            applies = evaluate(environment={"extra": extra})
        except TypeError:
            applies = evaluate({"extra": extra})
        if bool(applies):
            return True
    return False


def _requirement_extras(requirement: object) -> frozenset[str]:
    extras: set[str] = set()
    for extra in requirement.extras:  # type: ignore[attr-defined]
        if not isinstance(extra, str) or not extra:
            raise ValueError("invalid dependency extra")
        extras.add(normalize_distribution_name(extra))
    return frozenset(extras)


def _version_matches(requirement: object, version: str) -> bool:
    specifier = requirement.specifier  # type: ignore[attr-defined]
    try:
        return bool(specifier.contains(version, prereleases=True))
    except TypeError:
        return bool(specifier.contains(version))


def _resolve_closure(
    root: metadata.Distribution,
    candidates: tuple[metadata.Distribution, ...],
    parser_loader: RequirementParserLoader,
    root_extras: Iterable[str],
    budgets: ArtifactBudgets,
    cache: _VerificationCache,
    *,
    root_identity_ambiguous: bool,
) -> _ClosureResolution:
    root_name = _distribution_name(root)
    if budgets.max_nodes < 1:
        return _ClosureResolution(
            (),
            _issue(
                ArtifactIssueCode.NODE_BUDGET_EXCEEDED,
                root_name,
                "declared closure exceeded the distribution-node budget",
            ),
        )

    by_name: dict[str, list[metadata.Distribution]] = {}
    for distribution in candidates:
        bucket = by_name.setdefault(_distribution_name(distribution), [])
        if all(distribution is not item for item in bucket):
            bucket.append(distribution)
    root_bucket = by_name.setdefault(root_name, [])
    if all(root is not item for item in root_bucket):
        root_bucket.append(root)
    if root_identity_ambiguous or len(root_bucket) != 1:
        return _ClosureResolution(
            ((root_name, root),),
            _issue(
                ArtifactIssueCode.DEPENDENCY_AMBIGUOUS,
                root_name,
                "the root distribution has ambiguous installed metadata",
            ),
        )

    requested_extras = {
        root_name: {
            normalize_distribution_name(extra)
            for extra in root_extras
            if isinstance(extra, str) and extra
        }
    }
    resolved: dict[str, metadata.Distribution] = {root_name: root}
    processed_extras: dict[str, frozenset[str]] = {}
    counted_edges: set[tuple[str, int, str]] = set()
    edge_count = 0

    while True:
        pending = sorted(
            name
            for name in resolved
            if processed_extras.get(name)
            != frozenset(requested_extras.get(name, set()))
        )
        if not pending:
            return _ClosureResolution(tuple(sorted(resolved.items())), None)
        source_name = pending[0]
        source = resolved[source_name]
        active_extras = frozenset(requested_extras.get(source_name, set()))
        processed_extras[source_name] = active_extras

        raw_result = _cached_raw_requirements(source, budgets, cache)
        if raw_result.issue is not None:
            return _ClosureResolution(
                tuple(sorted(resolved.items())), raw_result.issue
            )
        assert raw_result.values is not None
        ordered_requirements = sorted(
            enumerate(raw_result.values), key=lambda item: (item[1], item[0])
        )
        for index, raw_requirement in ordered_requirements:
            edge = (source_name, index, raw_requirement)
            if edge not in counted_edges:
                counted_edges.add(edge)
                edge_count += 1
                if edge_count > budgets.max_edges:
                    return _ClosureResolution(
                        tuple(sorted(resolved.items())),
                        _issue(
                            ArtifactIssueCode.EDGE_BUDGET_EXCEEDED,
                            source_name,
                            "declared closure exceeded the dependency-edge budget",
                        ),
                    )

            if not cache.parser_loaded:
                try:
                    cache.requirement_parser = parser_loader()
                except Exception:
                    cache.requirement_parser = None
                cache.parser_loaded = True
            parser = cache.requirement_parser
            if parser is None:
                return _ClosureResolution(
                    tuple(sorted(resolved.items())),
                    _issue(
                        ArtifactIssueCode.DEPENDENCY_PARSER_UNAVAILABLE,
                        source_name,
                        "Requires-Dist cannot be verified without a PEP 508 parser",
                    ),
                )

            parsed = cache.parsed_requirements.get(raw_requirement)
            if parsed is None:
                try:
                    requirement = parser(raw_requirement)
                    raw_name = requirement.name  # type: ignore[attr-defined]
                    if not isinstance(raw_name, str) or not raw_name:
                        raise ValueError("invalid dependency name")
                    dependency_name = normalize_distribution_name(raw_name)
                    marker = requirement.marker  # type: ignore[attr-defined]
                    dependency_extras = _requirement_extras(requirement)
                    parsed = _ParsedRequirement(
                        requirement,
                        dependency_name,
                        dependency_extras,
                        marker,
                    )
                except Exception:
                    parsed = _ParsedRequirement(None, None, frozenset(), None)
                cache.parsed_requirements[raw_requirement] = parsed
            if parsed.requirement is None or parsed.dependency_name is None:
                return _ClosureResolution(
                    tuple(sorted(resolved.items())),
                    _issue(
                        ArtifactIssueCode.DEPENDENCY_UNVERIFIABLE,
                        source_name,
                        "a Requires-Dist entry could not be parsed or evaluated",
                    ),
                )

            try:
                if parsed.marker is not None and not _marker_applies(
                    parsed.marker, active_extras
                ):
                    continue
            except Exception:
                return _ClosureResolution(
                    tuple(sorted(resolved.items())),
                    _issue(
                        ArtifactIssueCode.DEPENDENCY_UNVERIFIABLE,
                        source_name,
                        "a Requires-Dist entry could not be parsed or evaluated",
                    ),
                )

            dependency_name = parsed.dependency_name
            matches = by_name.get(dependency_name, [])
            if not matches:
                return _ClosureResolution(
                    tuple(sorted(resolved.items())),
                    _issue(
                        ArtifactIssueCode.DEPENDENCY_MISSING,
                        dependency_name,
                        "a declared closure dependency is not installed",
                    ),
                )
            if len(matches) != 1:
                return _ClosureResolution(
                    tuple(sorted(resolved.items())),
                    _issue(
                        ArtifactIssueCode.DEPENDENCY_AMBIGUOUS,
                        dependency_name,
                        "a declared closure dependency has ambiguous installed metadata",
                    ),
                )
            dependency = matches[0]
            try:
                version_matches = _version_matches(
                    parsed.requirement, _distribution_version(dependency)
                )
            except Exception:
                return _ClosureResolution(
                    tuple(sorted(resolved.items())),
                    _issue(
                        ArtifactIssueCode.DEPENDENCY_UNVERIFIABLE,
                        dependency_name,
                        "the installed dependency version could not be verified",
                    ),
                )
            if not version_matches:
                return _ClosureResolution(
                    tuple(sorted(resolved.items())),
                    _issue(
                        ArtifactIssueCode.DEPENDENCY_VERSION_MISMATCH,
                        dependency_name,
                        "the installed dependency does not satisfy Requires-Dist",
                    ),
                )

            if dependency_name not in resolved:
                if len(resolved) >= budgets.max_nodes:
                    return _ClosureResolution(
                        tuple(sorted(resolved.items())),
                        _issue(
                            ArtifactIssueCode.NODE_BUDGET_EXCEEDED,
                            dependency_name,
                            "declared closure exceeded the distribution-node budget",
                        ),
                    )
                resolved[dependency_name] = dependency
                requested_extras[dependency_name] = set()
            before = len(requested_extras[dependency_name])
            requested_extras[dependency_name].update(parsed.dependency_extras)
            if len(requested_extras[dependency_name]) != before:
                processed_extras.pop(dependency_name, None)


def _valid_record_path(value: str) -> bool:
    if (
        not value
        or "\x00" in value
        or "\\" in value
        or any(ord(character) < 32 for character in value)
    ):
        return False
    if _WINDOWS_ABSOLUTE_PATH.match(value) or value.startswith("/"):
        return False
    parts = value.split("/")
    if any(part in ("", ".", "..") for part in parts):
        return False
    path = PurePosixPath(value)
    return not path.is_absolute() and ".." not in path.parts


def _read_record_payload(
    distribution: metadata.Distribution,
    byte_limit: int,
) -> tuple[str | None, ArtifactIssue | None]:
    name = _distribution_name(distribution)
    metadata_path = (
        getattr(distribution, "_path", None)
        if isinstance(distribution, metadata.PathDistribution)
        else None
    )
    if metadata_path is not None:
        try:
            record_path = metadata_path.joinpath("RECORD")
            with record_path.open("rb") as stream:
                raw_payload = stream.read(byte_limit + 1)
        except FileNotFoundError:
            return None, _issue(
                ArtifactIssueCode.RECORD_MISSING,
                name,
                "the installed distribution has no RECORD",
            )
        except Exception:
            return None, _issue(
                ArtifactIssueCode.RECORD_UNREADABLE,
                name,
                "could not read the installed RECORD",
            )
        if not isinstance(raw_payload, bytes):
            return None, _issue(
                ArtifactIssueCode.RECORD_UNREADABLE,
                name,
                "the installed RECORD did not contain bytes",
            )
        if len(raw_payload) > byte_limit:
            return None, _issue(
                ArtifactIssueCode.RECORD_BUDGET_EXCEEDED,
                name,
                "the installed RECORD exceeded the byte budget",
            )
        try:
            return raw_payload.decode("utf-8"), None
        except UnicodeDecodeError:
            return None, _issue(
                ArtifactIssueCode.RECORD_UNREADABLE,
                name,
                "the installed RECORD did not contain UTF-8 text",
            )

    try:
        payload = distribution.read_text("RECORD")
    except Exception:
        return None, _issue(
            ArtifactIssueCode.RECORD_UNREADABLE,
            name,
            "could not read the installed RECORD",
        )
    if payload is None:
        return None, _issue(
            ArtifactIssueCode.RECORD_MISSING,
            name,
            "the installed distribution has no RECORD",
        )
    if not isinstance(payload, str):
        return None, _issue(
            ArtifactIssueCode.RECORD_UNREADABLE,
            name,
            "the installed RECORD did not contain text",
        )
    try:
        payload_size = len(payload.encode("utf-8"))
    except Exception:
        return None, _issue(
            ArtifactIssueCode.RECORD_UNREADABLE,
            name,
            "the installed RECORD did not contain valid text",
        )
    if payload_size > byte_limit:
        return None, _issue(
            ArtifactIssueCode.RECORD_BUDGET_EXCEEDED,
            name,
            "the installed RECORD exceeded the byte budget",
        )
    return payload, None


def _record_rows(
    distribution: metadata.Distribution,
    byte_limit: int,
) -> _RecordRows:
    name = _distribution_name(distribution)
    payload, payload_issue = _read_record_payload(distribution, byte_limit)
    if payload_issue is not None:
        return _RecordRows((), payload_issue)
    assert payload is not None

    rows: list[tuple[str, str, int | None]] = []
    seen_paths: set[str] = set()
    try:
        reader = csv.reader(io.StringIO(payload, newline=""), strict=True)
        for row in reader:
            if len(row) != 3:
                return _RecordRows(
                    tuple(rows),
                    _issue(
                        ArtifactIssueCode.RECORD_INVALID,
                        name,
                        "the installed RECORD contains an invalid row",
                    ),
                )
            path, hash_spec, size_spec = row
            if path in seen_paths:
                return _RecordRows(
                    tuple(rows),
                    _issue(
                        ArtifactIssueCode.RECORD_INVALID,
                        name,
                        "the installed RECORD contains a duplicate path",
                        path=path,
                    ),
                )
            seen_paths.add(path)
            size = None
            if size_spec:
                try:
                    size = int(size_spec)
                except (TypeError, ValueError):
                    return _RecordRows(
                        tuple(rows),
                        _issue(
                            ArtifactIssueCode.RECORD_INVALID,
                            name,
                            "the installed RECORD contains an invalid size",
                            path=path,
                        ),
                    )
                if size < 0:
                    return _RecordRows(
                        tuple(rows),
                        _issue(
                            ArtifactIssueCode.RECORD_INVALID,
                            name,
                            "the installed RECORD contains a negative size",
                            path=path,
                        ),
                    )
            rows.append((path, hash_spec, size))
    except Exception:
        return _RecordRows(
            tuple(rows),
            _issue(
                ArtifactIssueCode.RECORD_INVALID,
                name,
                "the installed RECORD is malformed",
            ),
        )
    if not rows:
        return _RecordRows(
            (),
            _issue(
                ArtifactIssueCode.RECORD_INVALID,
                name,
                "the installed RECORD contains no file records",
            ),
        )
    return _RecordRows(tuple(rows), None)


def _is_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
    except ValueError:
        return False
    return True


def _locate_root(
    distribution: metadata.Distribution,
) -> tuple[tuple[Path, Path] | None, ArtifactIssue | None]:
    name = _distribution_name(distribution)
    try:
        located = distribution.locate_file(metadata.PackagePath(""))
        lexical_root = Path(os.fspath(located)).absolute()
        canonical_root = lexical_root.resolve(strict=True)
        if not canonical_root.is_dir():
            raise OSError("not a directory")
    except Exception:
        return None, _issue(
            ArtifactIssueCode.DISTRIBUTION_ROOT_UNREADABLE,
            name,
            "could not establish the installed distribution root",
        )
    return (lexical_root, canonical_root), None


def _locate_record_file(
    distribution: metadata.Distribution,
    relative_path: str,
    lexical_root: Path,
    canonical_root: Path,
) -> tuple[Path | None, ArtifactIssue | None]:
    name = _distribution_name(distribution)
    if not _valid_record_path(relative_path):
        return None, _issue(
            ArtifactIssueCode.FILE_PATH_INVALID,
            name,
            "the installed RECORD contains a non-relative or parent path",
            path=relative_path,
        )
    try:
        package_path = metadata.PackagePath(relative_path)
        package_path.dist = distribution  # type: ignore[assignment]
        located = distribution.locate_file(package_path)
        lexical_path = Path(os.fspath(located)).absolute()
    except Exception:
        return None, _issue(
            ArtifactIssueCode.FILE_UNREADABLE,
            name,
            "could not locate an installed file record",
            path=relative_path,
        )
    if not _is_within(lexical_path, lexical_root):
        return None, _issue(
            ArtifactIssueCode.FILE_OUTSIDE_ROOT,
            name,
            "an installed file resolves outside the distribution root",
            path=relative_path,
        )
    try:
        current = lexical_root
        for component in lexical_path.relative_to(lexical_root).parts:
            current = current / component
            if current.is_symlink():
                return None, _issue(
                    ArtifactIssueCode.FILE_SYMLINK,
                    name,
                    "an installed file record traverses a symbolic link",
                    path=relative_path,
                )
        if not lexical_path.exists():
            return None, _issue(
                ArtifactIssueCode.FILE_MISSING,
                name,
                "an installed RECORD file is missing",
                path=relative_path,
            )
        canonical_path = lexical_path.resolve(strict=True)
    except OSError:
        return None, _issue(
            ArtifactIssueCode.FILE_UNREADABLE,
            name,
            "could not resolve an installed file record",
            path=relative_path,
        )
    if not _is_within(canonical_path, canonical_root):
        return None, _issue(
            ArtifactIssueCode.FILE_OUTSIDE_ROOT,
            name,
            "an installed file resolves outside the distribution root",
            path=relative_path,
        )
    if not canonical_path.is_file():
        return None, _issue(
            ArtifactIssueCode.FILE_UNREADABLE,
            name,
            "an installed RECORD entry is not a regular file",
            path=relative_path,
        )
    return canonical_path, None


def _hash_factory(hash_spec: str) -> tuple[object | None, bytes | None]:
    mode, separator, encoded_digest = hash_spec.partition("=")
    if not separator or not mode or not encoded_digest:
        return None, None
    try:
        digest = base64.b64decode(
            encoded_digest + "=" * (-len(encoded_digest) % 4),
            altchars=b"-_",
            validate=True,
        )
        hasher = hashlib.new(mode)
    except Exception:
        return None, None
    if not digest or len(digest) != hasher.digest_size:
        return None, None
    return hasher, digest


def _native_magic(header: bytes) -> bool:
    if header.startswith((b"\x7fELF", b"MZ")):
        return True
    if header[:4] in _MACH_O_MAGICS:
        return True
    if header.startswith((b"!<arch>\n", b"!<thin>\n")):
        return True
    if len(header) >= 20 and header[:2] in _COFF_MACHINE_MAGICS:
        section_count = int.from_bytes(header[2:4], "little")
        return 0 < section_count <= 96
    return False


def _native_suffix(relative_path: str) -> bool:
    filename = PurePosixPath(relative_path).name.casefold()
    return (
        any(filename.endswith(suffix) for suffix in _NATIVE_SUFFIXES)
        or _VERSIONED_SO_SUFFIX.search(filename) is not None
    )


def _inspect_file(
    distribution: metadata.Distribution,
    relative_path: str,
    path: Path,
    hash_spec: str,
    expected_size: int | None,
) -> ArtifactIssue | None:
    name = _distribution_name(distribution)
    hasher = None
    expected_digest = None
    if hash_spec:
        hasher, expected_digest = _hash_factory(hash_spec)
        if hasher is None or expected_digest is None:
            return _issue(
                ArtifactIssueCode.HASH_UNVERIFIABLE,
                name,
                "an installed RECORD hash cannot be verified",
                path=relative_path,
            )
    header = bytearray()
    observed_size = 0
    executable = False
    try:
        with path.open("rb") as stream:
            mode = os.fstat(stream.fileno()).st_mode
            executable = os.name == "posix" and bool(mode & 0o111)
            while True:
                chunk = stream.read(64 * 1024)
                if not chunk:
                    break
                if len(header) < 4096:
                    header.extend(chunk[: 4096 - len(header)])
                observed_size += len(chunk)
                if hasher is not None:
                    hasher.update(chunk)  # type: ignore[attr-defined]
    except Exception:
        return _issue(
            ArtifactIssueCode.FILE_UNREADABLE,
            name,
            "could not read an installed file record",
            path=relative_path,
        )
    if expected_size is not None and observed_size != expected_size:
        return _issue(
            ArtifactIssueCode.SIZE_MISMATCH,
            name,
            "an installed file size does not match RECORD",
            path=relative_path,
        )
    if (
        hasher is not None
        and hasher.digest() != expected_digest  # type: ignore[attr-defined]
    ):
        return _issue(
            ArtifactIssueCode.HASH_MISMATCH,
            name,
            "an installed file hash does not match RECORD",
            path=relative_path,
        )
    if executable:
        return _issue(
            ArtifactIssueCode.EXECUTABLE_IN_CLOSURE,
            name,
            "an executable file is present in the declared distribution closure",
            path=relative_path,
        )
    if _native_suffix(relative_path) or _native_magic(bytes(header)):
        return _issue(
            ArtifactIssueCode.NATIVE_IN_CLOSURE,
            name,
            "a native artifact is present in the declared distribution closure",
            path=relative_path,
        )
    return None


def _inspect_distribution(
    distribution: metadata.Distribution,
    record_byte_limit: int,
) -> _DistributionInspection:
    record_result = _record_rows(distribution, record_byte_limit)
    if record_result.issue is not None:
        return _DistributionInspection(
            record_result.rows,
            False,
            record_result.issue,
            0,
        )

    roots, root_issue = _locate_root(distribution)
    if root_issue is not None:
        return _DistributionInspection(
            record_result.rows,
            True,
            root_issue,
            0,
        )
    assert roots is not None
    lexical_root, canonical_root = roots
    inspected = 0
    for relative_path, hash_spec, expected_size in sorted(
        record_result.rows, key=lambda row: row[0]
    ):
        inspected += 1
        path, path_issue = _locate_record_file(
            distribution,
            relative_path,
            lexical_root,
            canonical_root,
        )
        if path_issue is not None:
            return _DistributionInspection(
                record_result.rows,
                True,
                path_issue,
                inspected,
            )
        assert path is not None
        file_issue = _inspect_file(
            distribution,
            relative_path,
            path,
            hash_spec,
            expected_size,
        )
        if file_issue is not None:
            return _DistributionInspection(
                record_result.rows,
                True,
                file_issue,
                inspected,
            )
    return _DistributionInspection(
        record_result.rows,
        True,
        None,
        inspected,
    )


def _cached_inspection(
    distribution: metadata.Distribution,
    budgets: ArtifactBudgets,
    cache: _VerificationCache,
) -> _DistributionInspection:
    cache_key = id(distribution)
    cached = cache.inspections.get(cache_key)
    if cached is not None and cached[0] is distribution:
        return cached[1]
    result = _inspect_distribution(
        distribution, max(0, budgets.max_record_bytes)
    )
    cache.inspections[cache_key] = (distribution, result)
    return result


def _verify(
    root: metadata.Distribution,
    candidates: tuple[metadata.Distribution, ...],
    *,
    parser_loader: RequirementParserLoader,
    extras: Iterable[str],
    budgets: ArtifactBudgets,
    enumeration_complete: bool,
    cache: _VerificationCache,
    root_identity_ambiguous: bool = False,
) -> ArtifactVerificationResult:
    root_name = _distribution_name(root)
    if budgets.max_edges < 0:
        return _rejected(
            root_name,
            _issue(
                ArtifactIssueCode.EDGE_BUDGET_EXCEEDED,
                root_name,
                "declared closure exceeded the dependency-edge budget",
            ),
            (root_name,),
        )
    if budgets.max_file_records < 0:
        return _rejected(
            root_name,
            _issue(
                ArtifactIssueCode.FILE_BUDGET_EXCEEDED,
                root_name,
                "declared closure exceeded the installed-file-record budget",
            ),
            (root_name,),
        )
    if budgets.max_requirement_bytes < 0:
        return _rejected(
            root_name,
            _issue(
                ArtifactIssueCode.REQUIREMENT_BUDGET_EXCEEDED,
                root_name,
                "a Requires-Dist entry exceeded the byte budget",
            ),
            (root_name,),
        )
    if budgets.max_record_bytes < 0:
        return _rejected(
            root_name,
            _issue(
                ArtifactIssueCode.RECORD_BUDGET_EXCEEDED,
                root_name,
                "the installed RECORD exceeded the byte budget",
            ),
            (root_name,),
        )
    if not enumeration_complete:
        return _rejected(
            root_name,
            _issue(
                ArtifactIssueCode.DEPENDENCY_ENUMERATION_UNREADABLE,
                root_name,
                "installed distributions could not be enumerated completely",
            ),
            (root_name,),
        )
    resolution = _resolve_closure(
        root,
        candidates,
        parser_loader,
        extras,
        budgets,
        cache,
        root_identity_ambiguous=root_identity_ambiguous,
    )
    closure = tuple(name for name, _ in resolution.distributions)
    if resolution.issue is not None:
        return _rejected(root_name, resolution.issue, closure or (root_name,))

    inspected = 0
    for distribution_name, distribution in resolution.distributions:
        remaining = budgets.max_file_records - inspected
        inspection = _cached_inspection(distribution, budgets, cache)
        if len(inspection.record_rows) > remaining:
            relative_path = inspection.record_rows[remaining][0]
            return _rejected(
                root_name,
                _issue(
                    ArtifactIssueCode.FILE_BUDGET_EXCEEDED,
                    distribution_name,
                    "declared closure exceeded the installed-file-record budget",
                    path=relative_path,
                ),
                closure,
                inspected,
            )
        if not inspection.records_complete:
            assert inspection.issue is not None
            return _rejected(root_name, inspection.issue, closure, inspected)
        inspected += inspection.inspected_file_records
        if inspection.issue is not None:
            return _rejected(root_name, inspection.issue, closure, inspected)
    return _accepted(root_name, closure, inspected)


def verify_distribution_closure(
    root: metadata.Distribution,
    *,
    distributions: Iterable[metadata.Distribution] | None = None,
    requirement_parser: RequirementParser | None = None,
    load_default_requirement_parser: RequirementParserLoader = (
        _load_default_requirement_parser
    ),
    extras: Iterable[str] = (),
    budgets: ArtifactBudgets = ArtifactBudgets(),
) -> ArtifactVerificationResult:
    """Verify one root's declared installed closure without importing it.

    The PEP 508 parser is loaded only when Requires-Dist exists. Thus a
    dependency-free install remains verifiable where packaging is absent.
    Every metadata or filesystem failure becomes one bounded rejection result.
    """

    root_name = _distribution_name(root)
    candidates, complete = _materialize_installed(distributions)
    cache = _VerificationCache(
        requirement_parser,
        requirement_parser is not None,
    )
    try:
        return _verify(
            root,
            candidates,
            parser_loader=load_default_requirement_parser,
            extras=extras,
            budgets=budgets,
            enumeration_complete=complete,
            cache=cache,
        )
    except Exception:
        return _rejected(
            root_name,
            _issue(
                ArtifactIssueCode.INTERNAL_ERROR,
                root_name,
                "the installed distribution closure could not be verified",
            ),
            (root_name,),
        )


def verify_distribution_closures(
    roots: Iterable[metadata.Distribution],
    *,
    distributions: Iterable[metadata.Distribution] | None = None,
    requirement_parser: RequirementParser | None = None,
    load_default_requirement_parser: RequirementParserLoader = (
        _load_default_requirement_parser
    ),
    extras: Iterable[str] = (),
    budgets: ArtifactBudgets = ArtifactBudgets(),
) -> tuple[ArtifactVerificationResult, ...]:
    """Verify sibling plugin roots independently in normalized-name order."""

    root_values, roots_complete = _materialize(roots)
    candidates, enumeration_complete = _materialize_installed(distributions)
    try:
        extra_values = tuple(extras)
    except Exception:
        extra_values = ()
        extras_complete = False
    else:
        extras_complete = True

    root_name_counts: dict[str, int] = {}
    for root in root_values:
        name = _distribution_name(root)
        root_name_counts[name] = root_name_counts.get(name, 0) + 1
    duplicate_root_names = {
        name for name, count in root_name_counts.items() if count > 1
    }

    cache = _VerificationCache(
        requirement_parser,
        requirement_parser is not None,
    )
    results = []
    ordered_roots = sorted(
        root_values,
        key=lambda item: (_distribution_name(item), _distribution_version(item)),
    )
    for root in ordered_roots:
        root_name = _distribution_name(root)
        if not extras_complete:
            result = _rejected(
                root_name,
                _issue(
                    ArtifactIssueCode.INTERNAL_ERROR,
                    root_name,
                    "the installed distribution closure could not be verified",
                ),
                (root_name,),
            )
        else:
            try:
                result = _verify(
                    root,
                    candidates,
                    parser_loader=load_default_requirement_parser,
                    extras=extra_values,
                    budgets=budgets,
                    enumeration_complete=enumeration_complete,
                    cache=cache,
                    root_identity_ambiguous=(
                        root_name in duplicate_root_names
                    ),
                )
            except Exception:
                result = _rejected(
                    root_name,
                    _issue(
                        ArtifactIssueCode.INTERNAL_ERROR,
                        root_name,
                        "the installed distribution closure could not be verified",
                    ),
                    (root_name,),
                )
        results.append(result)

    if not roots_complete:
        results.append(
            _rejected(
                _UNKNOWN_DISTRIBUTION_NAME,
                _issue(
                    ArtifactIssueCode.DEPENDENCY_ENUMERATION_UNREADABLE,
                    _UNKNOWN_DISTRIBUTION_NAME,
                    "plugin roots could not be enumerated completely",
                ),
                (),
            )
        )
    return tuple(results)


__all__ = [
    "ArtifactBudgets",
    "ArtifactIssue",
    "ArtifactIssueCode",
    "ArtifactVerificationResult",
    "MAX_CLOSURE_EDGES",
    "MAX_CLOSURE_NODES",
    "MAX_DIAGNOSTIC_BYTES",
    "MAX_EVIDENCE_PATH_BYTES",
    "MAX_INSPECTED_FILE_RECORDS",
    "MAX_RECORD_BYTES",
    "MAX_REQUIREMENT_BYTES",
    "verify_distribution_closure",
    "verify_distribution_closures",
]
