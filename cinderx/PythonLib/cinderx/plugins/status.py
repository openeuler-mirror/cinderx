# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict
"""Immutable, bounded diagnostics for the static plugin admission stages.

Snapshot construction is an explicit bootstrap/manager operation.  The public
``status()`` query only returns the most recently installed immutable value;
it never discovers distributions, imports adapters, or probes native state.
"""

from __future__ import annotations

from collections.abc import Iterable
from dataclasses import dataclass
from enum import Enum
from hashlib import sha256
from threading import Lock

from .artifacts import ArtifactIssueCode, ArtifactVerificationResult
from .discovery import PluginDiscoveryResult, normalize_distribution_name
from .negotiation import (
    NegotiationIssue,
    NegotiationReason,
    PluginNegotiationResult,
)


MAX_STATUS_PLUGINS = 128
MAX_STATUS_DIAGNOSTICS_PER_PLUGIN = 32
MAX_STATUS_DETAILS_PER_DIAGNOSTIC = 16
MAX_STATUS_STRING_BYTES = 256

_MAX_STATUS_INPUT_RESULTS = MAX_STATUS_PLUGINS * 4
_MAX_STATUS_IDENTITY_CHARS = MAX_STATUS_STRING_BYTES * 4
_INVALID_DISTRIBUTION_NAME = "<invalid-status-input>"


class PluginState(str, Enum):
    """The A-7 plugin states available before lifecycle activation exists."""

    DISCOVERED = "discovered"
    AVAILABLE = "available"
    UNAVAILABLE = "unavailable"


class PluginStatusReason(str, Enum):
    """Stable, low-cardinality public plugin unavailability families."""

    SPI_MISMATCH = "spi_mismatch"
    PYTHON_VERSION_MISMATCH = "py_mismatch"
    SOABI_MISMATCH = "soabi_mismatch"
    CORE_BUILD_ID_MISMATCH = "build_mismatch"
    CPU_CAPABILITY_MISSING = "cpu_insufficient"
    TARGET_CAPABILITY_MISSING = "capability_missing"
    PLUGIN_ID_CONFLICT = "id_conflict"
    SCHEMA_INVALID = "schema_invalid"
    NATIVE_IN_CLOSURE = "native_in_closure"


_NEGOTIATION_REASON_MAP = {
    NegotiationReason.SPI_MISMATCH: PluginStatusReason.SPI_MISMATCH,
    NegotiationReason.PYTHON_VERSION_MISMATCH: (
        PluginStatusReason.PYTHON_VERSION_MISMATCH
    ),
    NegotiationReason.SOABI_MISMATCH: PluginStatusReason.SOABI_MISMATCH,
    NegotiationReason.CORE_BUILD_ID_MISMATCH: (
        PluginStatusReason.CORE_BUILD_ID_MISMATCH
    ),
    NegotiationReason.CPU_CAPABILITY_MISSING: (
        PluginStatusReason.CPU_CAPABILITY_MISSING
    ),
    NegotiationReason.TARGET_CAPABILITY_MISSING: (
        PluginStatusReason.TARGET_CAPABILITY_MISSING
    ),
    NegotiationReason.PLUGIN_ID_CONFLICT: PluginStatusReason.PLUGIN_ID_CONFLICT,
}

_STAGE_ORDER = {
    "discovery": 0,
    "manifest": 1,
    "negotiation": 2,
    "artifact": 3,
    "status": 4,
}


def _bounded_text(value: object, *, fallback: str = "") -> str:
    if not isinstance(value, str):
        value = fallback
    truncated_by_chars = len(value) > MAX_STATUS_STRING_BYTES
    candidate = value[:MAX_STATUS_STRING_BYTES]
    try:
        encoded = candidate.encode("utf-8", errors="replace")
    except Exception:
        encoded = fallback.encode("utf-8", errors="replace")
    if not truncated_by_chars and len(encoded) <= MAX_STATUS_STRING_BYTES:
        return encoded.decode("utf-8", errors="replace")
    marker = b"..."
    prefix = encoded[: MAX_STATUS_STRING_BYTES - len(marker)]
    while prefix:
        try:
            return prefix.decode("utf-8") + marker.decode("ascii")
        except UnicodeDecodeError:
            prefix = prefix[:-1]
    return marker.decode("ascii")


def _bounded_details(values: object) -> tuple[str, ...]:
    if isinstance(values, (str, bytes)):
        return (_bounded_text(values),)
    try:
        iterator = iter(values)  # type: ignore[arg-type]
    except Exception:
        return ()
    details: list[str] = []
    while len(details) < MAX_STATUS_DETAILS_PER_DIAGNOSTIC:
        try:
            value = next(iterator)
        except StopIteration:
            break
        except Exception:
            break
        details.append(_bounded_text(value))
    return tuple(details)


@dataclass(frozen=True, slots=True)
class StageDiagnostic:
    """One bounded lower-level diagnostic from an admission stage."""

    stage: str
    code: str
    message: str = ""
    expected: tuple[str, ...] = ()
    actual: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        object.__setattr__(self, "stage", _bounded_text(self.stage))
        object.__setattr__(self, "code", _bounded_text(self.code))
        object.__setattr__(self, "message", _bounded_text(self.message))
        object.__setattr__(self, "expected", _bounded_details(self.expected))
        object.__setattr__(self, "actual", _bounded_details(self.actual))


def _diagnostic_order(
    diagnostic: StageDiagnostic,
) -> tuple[object, ...]:
    return (
        _STAGE_ORDER.get(diagnostic.stage, len(_STAGE_ORDER)),
        diagnostic.stage,
        diagnostic.code,
        diagnostic.message,
        diagnostic.expected,
        diagnostic.actual,
    )


_DiagnosticSpec = tuple[
    str,
    str,
    str,
    tuple[str, ...],
    tuple[str, ...],
]


def _diagnostic_spec(
    stage: object,
    code: object,
    message: object = "",
    expected: object = (),
    actual: object = (),
) -> _DiagnosticSpec:
    return (
        _bounded_text(stage),
        _bounded_text(code),
        _bounded_text(message),
        _bounded_details(expected),
        _bounded_details(actual),
    )


def _diagnostic_spec_order(spec: _DiagnosticSpec) -> tuple[object, ...]:
    return (
        _STAGE_ORDER.get(spec[0], len(_STAGE_ORDER)),
        *spec,
    )


def _materialize_diagnostic(spec: _DiagnosticSpec) -> StageDiagnostic:
    return StageDiagnostic(*spec)


class _DiagnosticAccumulator:
    """Retain the canonical diagnostic cap without constructing discarded values."""

    def __init__(self) -> None:
        self._specs: set[_DiagnosticSpec] = set()

    def add(
        self,
        stage: object,
        code: object,
        message: object = "",
        *,
        expected: object = (),
        actual: object = (),
    ) -> None:
        spec = _diagnostic_spec(stage, code, message, expected, actual)
        self._specs.add(spec)
        if len(self._specs) > MAX_STATUS_DIAGNOSTICS_PER_PLUGIN:
            self._specs.remove(max(self._specs, key=_diagnostic_spec_order))

    def add_schema(self, code: object, message: object = "") -> None:
        self.add("status", code, message)

    def materialize(self) -> tuple[StageDiagnostic, ...]:
        return tuple(
            _materialize_diagnostic(spec)
            for spec in sorted(self._specs, key=_diagnostic_spec_order)
        )


def _ordered_reasons(values: object) -> tuple[PluginStatusReason, ...]:
    try:
        iterator = iter(values)  # type: ignore[arg-type]
    except Exception:
        return ()
    present: set[PluginStatusReason] = set()
    inspected = 0
    while inspected < _MAX_STATUS_INPUT_RESULTS:
        try:
            value = next(iterator)
        except StopIteration:
            break
        except Exception:
            break
        inspected += 1
        if isinstance(value, PluginStatusReason):
            present.add(value)
            if len(present) == len(PluginStatusReason):
                break
    return tuple(reason for reason in PluginStatusReason if reason in present)


def _ordered_diagnostics(values: object) -> tuple[StageDiagnostic, ...]:
    try:
        iterator = iter(values)  # type: ignore[arg-type]
    except Exception:
        return ()
    unique: set[StageDiagnostic] = set()
    inspected = 0
    while inspected < _MAX_STATUS_INPUT_RESULTS:
        try:
            value = next(iterator)
        except StopIteration:
            break
        except Exception:
            break
        inspected += 1
        if isinstance(value, StageDiagnostic):
            unique.add(value)
    return tuple(
        sorted(unique, key=_diagnostic_order)[
            :MAX_STATUS_DIAGNOSTICS_PER_PLUGIN
        ]
    )


@dataclass(frozen=True, slots=True)
class PluginStatus:
    """Immutable public status for one discovered distribution."""

    distribution_name: str
    normalized_distribution_name: str
    distribution_version: str
    plugin_id: str | None
    state: PluginState
    reasons: tuple[PluginStatusReason, ...] = ()
    diagnostics: tuple[StageDiagnostic, ...] = ()

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "distribution_name",
            _bounded_text(
                self.distribution_name,
                fallback=_INVALID_DISTRIBUTION_NAME,
            ),
        )
        object.__setattr__(
            self,
            "normalized_distribution_name",
            _bounded_text(
                self.normalized_distribution_name,
                fallback=_INVALID_DISTRIBUTION_NAME,
            ),
        )
        object.__setattr__(
            self,
            "distribution_version",
            _bounded_text(self.distribution_version),
        )
        object.__setattr__(
            self,
            "plugin_id",
            _bounded_text(self.plugin_id) if self.plugin_id is not None else None,
        )
        if not isinstance(self.state, PluginState):
            raise TypeError("state must be a PluginState")
        object.__setattr__(self, "reasons", _ordered_reasons(self.reasons))
        object.__setattr__(
            self,
            "diagnostics",
            _ordered_diagnostics(self.diagnostics),
        )


def _plugin_order(plugin: PluginStatus) -> tuple[str, ...]:
    return (
        plugin.normalized_distribution_name,
        plugin.distribution_name.casefold(),
        plugin.distribution_name,
        plugin.distribution_version,
        plugin.plugin_id or "",
    )


@dataclass(frozen=True, slots=True)
class StatusSnapshot:
    """One recursively immutable, deterministically ordered status view."""

    plugins: tuple[PluginStatus, ...] = ()

    def __post_init__(self) -> None:
        try:
            iterator = iter(self.plugins)
        except Exception:
            values: list[PluginStatus] = []
        else:
            values = []
            while len(values) < _MAX_STATUS_INPUT_RESULTS:
                try:
                    value = next(iterator)
                except StopIteration:
                    break
                except Exception:
                    break
                if isinstance(value, PluginStatus):
                    values.append(value)
        object.__setattr__(
            self,
            "plugins",
            tuple(sorted(values, key=_plugin_order)[:MAX_STATUS_PLUGINS]),
        )


def _bounded_inputs(values: object) -> tuple[object, ...]:
    try:
        iterator = iter(values)  # type: ignore[arg-type]
    except Exception:
        return ()
    results: list[object] = []
    consecutive_errors = 0
    while len(results) < _MAX_STATUS_INPUT_RESULTS:
        try:
            value = next(iterator)
        except StopIteration:
            break
        except Exception:
            consecutive_errors += 1
            if consecutive_errors > 1:
                break
            continue
        consecutive_errors = 0
        results.append(value)
    return tuple(results)


def _bounded_stage_inputs(
    values: object,
) -> tuple[tuple[object, ...], str | None]:
    try:
        iterator = iter(values)  # type: ignore[arg-type]
    except Exception:
        return (), "status_input_incomplete"
    results: list[object] = []
    while len(results) <= _MAX_STATUS_INPUT_RESULTS:
        try:
            value = next(iterator)
        except StopIteration:
            return tuple(results), None
        except Exception:
            return (), "status_input_incomplete"
        results.append(value)
    return (), "status_input_budget_exceeded"


def _normalized_name(value: object) -> str | None:
    if not isinstance(value, str) or not value:
        return None
    if len(value) > _MAX_STATUS_IDENTITY_CHARS:
        return None
    try:
        return normalize_distribution_name(value)
    except Exception:
        return None


def _identity_key(value: object) -> str | None:
    normalized = _normalized_name(value)
    if normalized is None:
        return None
    return sha256(normalized.encode("utf-8", errors="surrogatepass")).hexdigest()


def _bounded_normalized_name(value: object) -> str | None:
    normalized = _normalized_name(value)
    return _bounded_text(normalized) if normalized is not None else None


def _discovery_identity(value: object) -> str | None:
    if not isinstance(value, PluginDiscoveryResult):
        return None
    return _identity_key(value.normalized_distribution_name)


def _negotiation_identity(value: object) -> str | None:
    if not isinstance(value, PluginNegotiationResult):
        return None
    return _discovery_identity(value.discovery)


def _artifact_identity(value: object) -> str | None:
    if not isinstance(value, ArtifactVerificationResult):
        return None
    return _identity_key(value.root_distribution_name)


def _group_by_identity(
    values: object,
    identity: object,
) -> dict[str, tuple[object, ...]]:
    grouped: dict[str, list[object]] = {}
    for value in _bounded_inputs(values):
        try:
            key = identity(value)  # type: ignore[operator]
        except Exception:
            key = None
        if key is not None:
            bucket = grouped.setdefault(key, [])
            bucket.append(value)
    return {key: tuple(items) for key, items in grouped.items()}


def _schema_diagnostic(code: str, message: str = "") -> StageDiagnostic:
    return StageDiagnostic("status", code, message)


def _malformed_status() -> PluginStatus:
    return PluginStatus(
        distribution_name=_INVALID_DISTRIBUTION_NAME,
        normalized_distribution_name=_INVALID_DISTRIBUTION_NAME,
        distribution_version="",
        plugin_id=None,
        state=PluginState.UNAVAILABLE,
        reasons=(PluginStatusReason.SCHEMA_INVALID,),
        diagnostics=(
            _schema_diagnostic(
                "invalid_status_input",
                "a discovery status input was malformed",
            ),
        ),
    )


def _input_failure_status(codes: Iterable[str]) -> PluginStatus:
    messages = {
        "status_input_budget_exceeded": (
            "status inputs exceeded the materialization budget"
        ),
        "status_input_incomplete": "a status input could not be read completely",
    }
    return PluginStatus(
        distribution_name=_INVALID_DISTRIBUTION_NAME,
        normalized_distribution_name=_INVALID_DISTRIBUTION_NAME,
        distribution_version="",
        plugin_id=None,
        state=PluginState.UNAVAILABLE,
        reasons=(PluginStatusReason.SCHEMA_INVALID,),
        diagnostics=tuple(
            _schema_diagnostic(code, messages[code])
            for code in sorted(set(codes))
        ),
    )


def _discovery_diagnostics(
    discovery: PluginDiscoveryResult,
    diagnostics: _DiagnosticAccumulator,
) -> bool:
    fatal = False
    try:
        discovery_issue = discovery.discovery_issue
        validation = discovery.validation
        available = discovery.available
    except Exception:
        diagnostics.add_schema(
            "invalid_discovery_result",
            "a discovery result was malformed",
        )
        return True

    if discovery_issue is not None:
        try:
            code = discovery_issue.code.value
            message = discovery_issue.message
        except Exception:
            code = "invalid_discovery_issue"
            message = "a discovery diagnostic was malformed"
        diagnostics.add("discovery", code, message)
    if validation is not None:
        try:
            issues = validation.issues
            manifest_rejection = validation.manifest_rejection
        except Exception:
            diagnostics.add_schema(
                "invalid_manifest_validation",
                "a manifest validation result was malformed",
            )
            return True
        for issue in _bounded_inputs(issues):
            try:
                diagnostics.add(
                    "manifest",
                    issue.code.value,  # type: ignore[attr-defined]
                    issue.message,  # type: ignore[attr-defined]
                    actual=(issue.path,),  # type: ignore[attr-defined]
                )
            except Exception:
                diagnostics.add_schema(
                    "invalid_manifest_issue",
                    "a manifest diagnostic was malformed",
                )
                fatal = True
        fatal = fatal or manifest_rejection is not None
    return fatal or not available


def _same_discovery(
    value: PluginNegotiationResult,
    discovery: PluginDiscoveryResult,
) -> bool:
    try:
        embedded = value.discovery
        return (
            embedded == discovery
            and embedded._sort_token == discovery._sort_token
        )
    except Exception:
        return False


def _negotiation_diagnostics(
    discovery: PluginDiscoveryResult,
    values: tuple[object, ...],
    diagnostics: _DiagnosticAccumulator,
) -> set[PluginStatusReason]:
    reasons: set[PluginStatusReason] = set()
    malformed = False
    matching_results = 0
    for value in values:
        if not isinstance(value, PluginNegotiationResult):
            malformed = True
            continue
        if not _same_discovery(value, discovery):
            malformed = True
            continue
        matching_results += 1
        try:
            issues = value.reasons
            available = value.available
            discovery_available = value.discovery.available
        except Exception:
            malformed = True
            continue
        valid_issue_count = 0
        for issue in _bounded_inputs(issues):
            if not isinstance(issue, NegotiationIssue):
                malformed = True
                continue
            try:
                public_reason = _NEGOTIATION_REASON_MAP.get(issue.reason)
            except Exception:
                public_reason = None
            if public_reason is None:
                malformed = True
                continue
            valid_issue_count += 1
            reasons.add(public_reason)
            diagnostics.add(
                "negotiation",
                issue.reason.value,
                expected=issue.expected,
                actual=issue.actual,
            )
        if not available and discovery_available and valid_issue_count == 0:
            malformed = True
    if matching_results > 1:
        malformed = True
    if malformed:
        reasons.add(PluginStatusReason.SCHEMA_INVALID)
        diagnostics.add_schema(
            "invalid_negotiation_result",
            "a negotiation status input was malformed",
        )
    return reasons


def _artifact_diagnostics(
    discovery: PluginDiscoveryResult,
    values: tuple[object, ...],
    diagnostics: _DiagnosticAccumulator,
) -> tuple[set[PluginStatusReason], bool]:
    reasons: set[PluginStatusReason] = set()
    malformed = False
    matching_results = 0
    accepted_result = False
    expected_identity = _discovery_identity(discovery)
    try:
        expected_version = discovery.distribution_version
    except Exception:
        expected_version = None
    for value in values:
        if not isinstance(value, ArtifactVerificationResult):
            malformed = True
            continue
        try:
            artifact_identity = _artifact_identity(value)
            artifact_version = value.root_distribution_version
        except Exception:
            malformed = True
            continue
        if (
            artifact_identity != expected_identity
            or not isinstance(artifact_version, str)
            or artifact_version != expected_version
        ):
            malformed = True
            continue
        matching_results += 1
        try:
            accepted = value.accepted
            issue = value.issue
        except Exception:
            malformed = True
            continue
        if type(accepted) is not bool or accepted != (issue is None):
            malformed = True
            continue
        if accepted:
            accepted_result = True
            continue
        try:
            issue_code = issue.code  # type: ignore[union-attr]
        except Exception:
            issue_code = None
        if not isinstance(issue_code, ArtifactIssueCode):
            malformed = True
            continue
        reasons.add(PluginStatusReason.NATIVE_IN_CLOSURE)
        diagnostics.add("artifact", issue_code.value)
    if matching_results > 1:
        malformed = True
    if malformed:
        reasons.add(PluginStatusReason.SCHEMA_INVALID)
        diagnostics.add_schema(
            "invalid_artifact_result",
            "an artifact status input was malformed",
        )
    return (
        reasons,
        matching_results == 1 and accepted_result and not malformed,
    )


def _plugin_status(
    discovery: PluginDiscoveryResult,
    negotiations: tuple[object, ...],
    artifacts: tuple[object, ...],
) -> PluginStatus:
    diagnostics = _DiagnosticAccumulator()
    discovery_fatal = _discovery_diagnostics(discovery, diagnostics)
    reasons: set[PluginStatusReason] = set()
    if discovery_fatal:
        reasons.add(PluginStatusReason.SCHEMA_INVALID)

    reasons.update(
        _negotiation_diagnostics(discovery, negotiations, diagnostics)
    )
    artifact_reasons, artifact_accepted = _artifact_diagnostics(
        discovery, artifacts, diagnostics
    )
    reasons.update(artifact_reasons)

    try:
        distribution_name = discovery.distribution_name
        normalized_name = discovery.normalized_distribution_name
        distribution_version = discovery.distribution_version
        manifest = discovery.manifest
        plugin_id = manifest.id if manifest is not None else None
    except Exception:
        return _malformed_status()

    if reasons:
        state = PluginState.UNAVAILABLE
    elif negotiations and artifact_accepted:
        state = PluginState.AVAILABLE
    else:
        state = PluginState.DISCOVERED
    return PluginStatus(
        distribution_name=distribution_name,
        normalized_distribution_name=(
            _bounded_normalized_name(normalized_name)
            or _INVALID_DISTRIBUTION_NAME
        ),
        distribution_version=distribution_version,
        plugin_id=plugin_id,
        state=state,
        reasons=tuple(reasons),
        diagnostics=diagnostics.materialize(),
    )


def build_status_snapshot(
    discovery_results: Iterable[PluginDiscoveryResult],
    *,
    negotiation_results: Iterable[PluginNegotiationResult] = (),
    artifact_results: Iterable[ArtifactVerificationResult] = (),
) -> StatusSnapshot:
    """Correlate already-computed stage results into one bounded snapshot."""

    negotiation_values, negotiation_failure = _bounded_stage_inputs(
        negotiation_results
    )
    artifact_values, artifact_failure = _bounded_stage_inputs(
        artifact_results
    )
    discovery_values, discovery_failure = _bounded_stage_inputs(
        discovery_results
    )
    input_failures = {
        failure
        for failure in (
            discovery_failure,
            negotiation_failure,
            artifact_failure,
        )
        if failure is not None
    }
    if input_failures:
        return StatusSnapshot((_input_failure_status(input_failures),))

    negotiations = _group_by_identity(
        negotiation_values,
        _negotiation_identity,
    )
    artifacts = _group_by_identity(artifact_values, _artifact_identity)
    identity_counts: dict[str, int] = {}
    for value in discovery_values:
        key = _discovery_identity(value)
        if key is not None:
            identity_counts[key] = identity_counts.get(key, 0) + 1

    plugins: list[PluginStatus] = []
    for value in discovery_values:
        identity = _discovery_identity(value)
        if identity is None or not isinstance(value, PluginDiscoveryResult):
            plugins.append(_malformed_status())
            continue
        if identity_counts.get(identity, 0) > 1:
            plugins.append(
                PluginStatus(
                    distribution_name=value.distribution_name,
                    normalized_distribution_name=value.normalized_distribution_name,
                    distribution_version=value.distribution_version,
                    plugin_id=(
                        value.manifest.id if value.manifest is not None else None
                    ),
                    state=PluginState.UNAVAILABLE,
                    reasons=(PluginStatusReason.SCHEMA_INVALID,),
                    diagnostics=(
                        _schema_diagnostic(
                            "duplicate_status_identity",
                            "multiple discovery results share one identity",
                        ),
                    ),
                )
            )
            continue
        try:
            plugins.append(
                _plugin_status(
                    value,
                    negotiations.get(identity, ()),
                    artifacts.get(identity, ()),
                )
            )
        except Exception:
            plugins.append(_malformed_status())
    return StatusSnapshot(tuple(plugins))


class _StatusStore:
    def __init__(self) -> None:
        self._lock = Lock()
        self._snapshot = StatusSnapshot()

    def install(self, snapshot: StatusSnapshot) -> StatusSnapshot:
        if not isinstance(snapshot, StatusSnapshot):
            raise TypeError("snapshot must be a StatusSnapshot")
        with self._lock:
            self._snapshot = snapshot
        return snapshot

    def read(self) -> StatusSnapshot:
        return self._snapshot


_STATUS_STORE = _StatusStore()


def install_status_snapshot(snapshot: StatusSnapshot) -> StatusSnapshot:
    """Atomically install a fully built immutable status snapshot."""

    return _STATUS_STORE.install(snapshot)


def update_status(
    discovery_results: Iterable[PluginDiscoveryResult],
    *,
    negotiation_results: Iterable[PluginNegotiationResult] = (),
    artifact_results: Iterable[ArtifactVerificationResult] = (),
) -> StatusSnapshot:
    """Build and atomically install a snapshot outside the query path."""

    snapshot = build_status_snapshot(
        discovery_results,
        negotiation_results=negotiation_results,
        artifact_results=artifact_results,
    )
    return install_status_snapshot(snapshot)


def status() -> StatusSnapshot:
    """Return the installed immutable snapshot without imports or file I/O."""

    return _STATUS_STORE.read()


__all__ = [
    "MAX_STATUS_DETAILS_PER_DIAGNOSTIC",
    "MAX_STATUS_DIAGNOSTICS_PER_PLUGIN",
    "MAX_STATUS_PLUGINS",
    "MAX_STATUS_STRING_BYTES",
    "PluginState",
    "PluginStatus",
    "PluginStatusReason",
    "StageDiagnostic",
    "StatusSnapshot",
    "build_status_snapshot",
    "install_status_snapshot",
    "status",
    "update_status",
]
