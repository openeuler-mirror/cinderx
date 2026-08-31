# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict
"""Pure-data compatibility negotiation for discovered CinderX plugins.

The decision order is SPI major, Python minor, SOABI, core build ID, CPU
requirements, target capability group, and finally plugin-ID ownership.
Negotiation never imports plugin adapters or their target frameworks.
"""

from __future__ import annotations

import sys
import sysconfig
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from enum import Enum
from itertools import islice
from typing import cast

from .discovery import PluginDiscoveryResult
from .manifest import CURRENT_SPI_VERSION, MAX_IDENTIFIER_BYTES, PluginManifest


MAX_NEGOTIATION_DETAIL_VALUES = 64
MAX_NEGOTIATION_DETAIL_CHARS = MAX_IDENTIFIER_BYTES


class NegotiationReason(str, Enum):
    """Stable machine-readable plugin unavailability reasons."""

    SPI_MISMATCH = "spi_mismatch"
    PYTHON_VERSION_MISMATCH = "py_mismatch"
    SOABI_MISMATCH = "soabi_mismatch"
    CORE_BUILD_ID_MISMATCH = "build_mismatch"
    CPU_CAPABILITY_MISSING = "cpu_insufficient"
    TARGET_CAPABILITY_MISSING = "capability_missing"
    PLUGIN_ID_CONFLICT = "id_conflict"


def _canonical_capabilities(values: Iterable[str]) -> tuple[str, ...]:
    return tuple(sorted(set(values)))


def _canonical_cpu_capability(value: str) -> str:
    return "asimd" if value == "neon" else value


def _bounded_detail(values: Iterable[str]) -> tuple[str, ...]:
    """Retain stable diagnostic context without copying unbounded input."""

    return tuple(
        value[:MAX_NEGOTIATION_DETAIL_CHARS]
        for value in islice(values, MAX_NEGOTIATION_DETAIL_VALUES)
    )


@dataclass(frozen=True, slots=True)
class RuntimeFingerprint:
    """Injectable runtime side of manifest compatibility negotiation."""

    spi_version: str
    python_version: str
    soabi: str
    core_build_id: str
    cpu_caps: tuple[str, ...]
    target_capabilities: tuple[str, ...]

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "cpu_caps",
            _canonical_capabilities(self.cpu_caps),
        )
        object.__setattr__(
            self,
            "target_capabilities",
            _canonical_capabilities(self.target_capabilities),
        )

    @classmethod
    def from_current(
        cls,
        *,
        native_capabilities: Mapping[str, object] | None = None,
        target_capabilities: Iterable[str] = (),
        spi_version: str = CURRENT_SPI_VERSION,
    ) -> RuntimeFingerprint:
        """Build a fingerprint, optionally using injected A-3 native data.

        Supplying ``native_capabilities`` keeps callers and unit tests wholly
        independent of whether the native ``_cinderx`` module is loadable.
        """

        if native_capabilities is None:
            import _cinderx

            native_capabilities = _cinderx.get_runtime_capabilities()

        core_build_id = native_capabilities["core_build_id"]
        cpu_caps = native_capabilities["cpu_caps"]
        if not isinstance(core_build_id, str):
            raise TypeError("native core_build_id must be a string")
        if not isinstance(cpu_caps, (list, tuple)) or not all(
            isinstance(capability, str) for capability in cpu_caps
        ):
            raise TypeError("native cpu_caps must be a string sequence")
        soabi = sysconfig.get_config_var("SOABI")
        return cls(
            spi_version=spi_version,
            python_version=f"{sys.version_info.major}.{sys.version_info.minor}",
            soabi=soabi if isinstance(soabi, str) else "",
            core_build_id=core_build_id,
            cpu_caps=tuple(cast(list[str] | tuple[str, ...], cpu_caps)),
            target_capabilities=tuple(target_capabilities),
        )


@dataclass(frozen=True, slots=True)
class NegotiationIssue:
    """One ordered incompatibility with bounded diagnostic context."""

    reason: NegotiationReason
    expected: tuple[str, ...] = ()
    actual: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        object.__setattr__(self, "expected", _bounded_detail(self.expected))
        object.__setattr__(self, "actual", _bounded_detail(self.actual))


@dataclass(frozen=True, slots=True)
class PluginNegotiationResult:
    """Negotiation outcome for one A-2 discovery result."""

    discovery: PluginDiscoveryResult
    reasons: tuple[NegotiationIssue, ...] = ()

    def __post_init__(self) -> None:
        object.__setattr__(self, "reasons", tuple(self.reasons))

    @property
    def available(self) -> bool:
        return self.discovery.available and not self.reasons

    @property
    def manifest(self) -> PluginManifest | None:
        return self.discovery.manifest


def _scalar_issue(
    reason: NegotiationReason,
    expected: str,
    actual: str,
) -> NegotiationIssue:
    return NegotiationIssue(reason, (expected,), (actual,))


def _compatibility_issues(
    manifest: PluginManifest,
    runtime: RuntimeFingerprint,
) -> tuple[NegotiationIssue, ...]:
    issues: list[NegotiationIssue] = []
    if manifest.spi_version != runtime.spi_version:
        issues.append(
            _scalar_issue(
                NegotiationReason.SPI_MISMATCH,
                manifest.spi_version,
                runtime.spi_version,
            )
        )
    if manifest.runtime_abi.python_version != runtime.python_version:
        issues.append(
            _scalar_issue(
                NegotiationReason.PYTHON_VERSION_MISMATCH,
                manifest.runtime_abi.python_version,
                runtime.python_version,
            )
        )
    if manifest.runtime_abi.soabi != runtime.soabi:
        issues.append(
            _scalar_issue(
                NegotiationReason.SOABI_MISMATCH,
                manifest.runtime_abi.soabi,
                runtime.soabi,
            )
        )
    if manifest.runtime_abi.core_build_id != runtime.core_build_id:
        issues.append(
            _scalar_issue(
                NegotiationReason.CORE_BUILD_ID_MISMATCH,
                manifest.runtime_abi.core_build_id,
                runtime.core_build_id,
            )
        )

    runtime_cpu_caps = frozenset(
        _canonical_cpu_capability(capability)
        for capability in runtime.cpu_caps
    )
    required_cpu_caps = {
        _canonical_cpu_capability(capability)
        for capability in manifest.runtime_abi.cpu_caps
    }
    if not required_cpu_caps.issubset(runtime_cpu_caps):
        issues.append(
            NegotiationIssue(
                NegotiationReason.CPU_CAPABILITY_MISSING,
                manifest.runtime_abi.cpu_caps,
                runtime.cpu_caps,
            )
        )

    runtime_target_capabilities = frozenset(runtime.target_capabilities)
    if not set(manifest.target_capabilities).issubset(
        runtime_target_capabilities
    ):
        issues.append(
            NegotiationIssue(
                NegotiationReason.TARGET_CAPABILITY_MISSING,
                manifest.target_capabilities,
                runtime.target_capabilities,
            )
        )
    return tuple(issues)


def negotiate(
    discovery_results: Iterable[PluginDiscoveryResult],
    *,
    runtime: RuntimeFingerprint,
) -> tuple[PluginNegotiationResult, ...]:
    """Negotiate plugins in deterministic A-2 discovery order.

    Only a candidate which passes every compatibility check reserves its
    plugin ID.  An incompatible earlier candidate therefore cannot squat an
    ID needed by a later compatible sibling.
    """

    results: list[PluginNegotiationResult] = []
    reserved_ids: dict[str, str] = {}
    for discovery in discovery_results:
        manifest = discovery.manifest
        issues = (
            _compatibility_issues(manifest, runtime)
            if discovery.available and manifest is not None
            else ()
        )
        if discovery.available and manifest is not None:
            owner = reserved_ids.get(manifest.id)
            if owner is not None:
                issues += (
                    NegotiationIssue(
                        NegotiationReason.PLUGIN_ID_CONFLICT,
                        (manifest.id,),
                        (owner,),
                    ),
                )
            elif not issues:
                reserved_ids[manifest.id] = discovery.distribution_name
        results.append(PluginNegotiationResult(discovery, issues))
    return tuple(results)
