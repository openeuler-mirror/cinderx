# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict
"""Deterministic, metadata-only discovery for CinderX plugins."""

from __future__ import annotations

import re
from collections.abc import Iterable
from dataclasses import dataclass, field
from enum import Enum
from hashlib import sha256
from importlib import metadata

from _cinderx_plugins_bootstrap import _read_manifest

from .manifest import (
    ManifestIssue,
    ManifestValidationResult,
    PluginManifest,
    validate_manifest,
)


MANIFEST_FILENAME = "cinderx_plugin.json"

_NORMALIZED_NAME_SEPARATOR = re.compile(r"[-_.]+")
_UNKNOWN_DISTRIBUTION_NAME = "<unknown>"
_MAX_CONSECUTIVE_ENUMERATION_ERRORS = 1


class DiscoveryIssueCode(str, Enum):
    """Stable machine-readable failures outside manifest validation."""

    MANIFEST_UNREADABLE = "manifest_unreadable"
    MANIFEST_INVALID_PAYLOAD = "manifest_invalid_payload"


@dataclass(frozen=True, slots=True)
class DiscoveryIssue:
    code: DiscoveryIssueCode
    message: str


@dataclass(frozen=True, slots=True)
class PluginDiscoveryResult:
    """The immutable discovery outcome for one manifest-bearing distribution."""

    distribution_name: str
    normalized_distribution_name: str
    distribution_version: str
    validation: ManifestValidationResult | None
    discovery_issue: DiscoveryIssue | None
    _sort_token: str = field(default="", repr=False, compare=False)

    def __post_init__(self) -> None:
        if (self.validation is None) == (self.discovery_issue is None):
            raise ValueError(
                "exactly one of validation and discovery_issue must be present"
            )

    @property
    def available(self) -> bool:
        return self.validation is not None and self.validation.accepted

    @property
    def manifest(self) -> PluginManifest | None:
        if self.validation is None:
            return None
        return self.validation.manifest

    @property
    def issues(self) -> tuple[ManifestIssue | DiscoveryIssue, ...]:
        if self.discovery_issue is not None:
            return (self.discovery_issue,)
        assert self.validation is not None
        return self.validation.issues


def normalize_distribution_name(name: str) -> str:
    """Return the normalized distribution-name ordering key."""

    return _NORMALIZED_NAME_SEPARATOR.sub("-", name).lower()


def _metadata_value(values: object, field: str, default: str) -> str:
    try:
        value = values.get(field)  # type: ignore[attr-defined]
    except Exception:
        return default
    return value if isinstance(value, str) and value else default


def _distribution_identity(
    distribution: metadata.Distribution,
) -> tuple[str, str, str]:
    try:
        values = distribution.metadata
    except Exception:
        values = {}
    name = _metadata_value(values, "Name", _UNKNOWN_DISTRIBUTION_NAME)
    version = _metadata_value(values, "Version", "")
    return name, normalize_distribution_name(name), version


def _unreadable_result(
    distribution: metadata.Distribution,
) -> PluginDiscoveryResult:
    name, normalized_name, version = _distribution_identity(distribution)
    return PluginDiscoveryResult(
        distribution_name=name,
        normalized_distribution_name=normalized_name,
        distribution_version=version,
        validation=None,
        discovery_issue=DiscoveryIssue(
            DiscoveryIssueCode.MANIFEST_UNREADABLE,
            f"could not read {MANIFEST_FILENAME}",
        ),
    )


def _invalid_payload_result(
    distribution: metadata.Distribution,
) -> PluginDiscoveryResult:
    name, normalized_name, version = _distribution_identity(distribution)
    return PluginDiscoveryResult(
        distribution_name=name,
        normalized_distribution_name=normalized_name,
        distribution_version=version,
        validation=None,
        discovery_issue=DiscoveryIssue(
            DiscoveryIssueCode.MANIFEST_INVALID_PAYLOAD,
            f"{MANIFEST_FILENAME} did not contain text or bytes",
        ),
    )


def _payload_sort_token(payload: str | bytes) -> str:
    encoded = (
        payload
        if isinstance(payload, bytes)
        else payload.encode("utf-8", errors="surrogatepass")
    )
    return sha256(encoded).hexdigest()


def _discover_distribution(
    distribution: metadata.Distribution,
) -> PluginDiscoveryResult | None:
    try:
        payload = _read_manifest(distribution)
    except Exception:
        return _unreadable_result(distribution)
    if payload is None:
        return None
    if not isinstance(payload, (str, bytes)):
        return _invalid_payload_result(distribution)

    name, normalized_name, version = _distribution_identity(distribution)
    try:
        validation = validate_manifest(payload)
    except Exception:
        return _invalid_payload_result(distribution)
    return PluginDiscoveryResult(
        distribution_name=name,
        normalized_distribution_name=normalized_name,
        distribution_version=version,
        validation=validation,
        discovery_issue=None,
        _sort_token=_payload_sort_token(payload),
    )


def _result_order(result: PluginDiscoveryResult) -> tuple[str, ...]:
    manifest_id = result.manifest.id if result.manifest is not None else ""
    issue_code = (
        result.discovery_issue.code.value
        if result.discovery_issue is not None
        else ""
    )
    return (
        result.normalized_distribution_name,
        result.distribution_name.casefold(),
        result.distribution_name,
        result.distribution_version,
        manifest_id,
        issue_code,
        result._sort_token,
    )


def _resilient_distributions(
    installed: Iterable[metadata.Distribution],
) -> Iterable[metadata.Distribution]:
    iterator = iter(installed)
    consecutive_errors = 0
    while True:
        try:
            distribution = next(iterator)
        except StopIteration:
            return
        except Exception:
            consecutive_errors += 1
            if consecutive_errors > _MAX_CONSECUTIVE_ENUMERATION_ERRORS:
                return
            continue
        consecutive_errors = 0
        yield distribution


def discover(
    *,
    distributions: Iterable[metadata.Distribution] | None = None,
) -> tuple[PluginDiscoveryResult, ...]:
    """Read and validate installed static manifests without loading plugins.

    Each candidate is fully contained: an unreadable or invalid manifest
    produces an unavailable result and discovery continues with every sibling.
    """

    installed = metadata.distributions() if distributions is None else distributions
    results: list[PluginDiscoveryResult] = []
    for distribution in _resilient_distributions(installed):
        result = _discover_distribution(distribution)
        if result is not None:
            results.append(result)
    return tuple(sorted(results, key=_result_order))
