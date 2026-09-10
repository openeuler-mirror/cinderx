# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict
"""Parse and validate the static ``cinderx_plugin.json`` manifest.

This module operates only on JSON text.  It deliberately does not import an
adapter or otherwise execute code from the plugin distribution.
"""

from __future__ import annotations

import json
import math
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from enum import Enum
from pathlib import PurePosixPath
from types import MappingProxyType
from typing import Any, NoReturn


CURRENT_SPI_VERSION = "1"

MAX_MANIFEST_BYTES = 64 * 1024
MAX_PROVIDE_ENTRIES = 256
MAX_ENTRIES_PER_SECTION = 128
MAX_TARGET_CAPABILITIES = 64
MAX_IDENTIFIER_BYTES = 128


_TOP_LEVEL_REQUIRED_FIELDS = (
    "id",
    "spi_version",
    "runtime_abi",
    "target_capabilities",
    "provides",
)
_TOP_LEVEL_FIELDS = frozenset((*_TOP_LEVEL_REQUIRED_FIELDS, "adapter"))
_RUNTIME_ABI_REQUIRED_FIELDS = (
    "python_version",
    "soabi",
    "core_build_id",
    "cpu_caps",
)
_RUNTIME_ABI_FIELDS = frozenset(_RUNTIME_ABI_REQUIRED_FIELDS)
_ADAPTER_REQUIRED_FIELDS = ("entry", "target")
_ADAPTER_FIELDS = frozenset(_ADAPTER_REQUIRED_FIELDS)
_LIST_PROVIDE_SECTIONS = ("contracts", "policies", "seeds")
_SINGLETON_PROVIDE_SECTIONS = ("diagnostics", "pass")
_PROVIDES_FIELDS = frozenset(
    (*_LIST_PROVIDE_SECTIONS, *_SINGLETON_PROVIDE_SECTIONS)
)

_UTF8_SIZE_CHUNK_CHARS = 4096


class ManifestIssueCode(str, Enum):
    """Stable machine-readable reasons emitted by the v1 validator."""

    MALFORMED_JSON = "malformed_json"
    DUPLICATE_FIELD = "duplicate_field"
    MANIFEST_TOO_LARGE = "manifest_too_large"
    TOO_MANY_ENTRIES = "too_many_entries"
    SCHEMA_INVALID = "schema_invalid"
    ENTRY_INVALID = "entry_invalid"


@dataclass(frozen=True, slots=True)
class ManifestIssue:
    code: ManifestIssueCode
    path: str
    message: str


@dataclass(frozen=True, slots=True)
class RuntimeABI:
    python_version: str
    soabi: str
    core_build_id: str
    cpu_caps: tuple[str, ...]


@dataclass(frozen=True, slots=True)
class PluginAdapter:
    entry: str
    target: str


@dataclass(frozen=True, slots=True)
class PluginProvides:
    contracts: tuple[Mapping[str, object], ...]
    policies: tuple[Mapping[str, object], ...]
    seeds: tuple[str, ...]
    diagnostics: Mapping[str, object] | None
    pass_config: Mapping[str, object] | None


@dataclass(frozen=True, slots=True)
class PluginManifest:
    id: str
    spi_version: str
    runtime_abi: RuntimeABI
    target_capabilities: tuple[str, ...]
    provides: PluginProvides
    adapter: PluginAdapter | None


@dataclass(frozen=True, slots=True)
class ManifestValidationResult:
    """A fatal rejection or an accepted manifest with isolated bad entries."""

    manifest: PluginManifest | None
    manifest_rejection: ManifestIssue | None
    entry_rejections: tuple[ManifestIssue, ...] = ()

    def __post_init__(self) -> None:
        if (self.manifest is None) == (self.manifest_rejection is None):
            raise ValueError(
                "exactly one of manifest and manifest_rejection must be present"
            )
        if self.manifest_rejection is not None and self.entry_rejections:
            raise ValueError("a rejected manifest cannot have entry rejections")

    @property
    def accepted(self) -> bool:
        return self.manifest is not None

    @property
    def issues(self) -> tuple[ManifestIssue, ...]:
        if self.manifest_rejection is not None:
            return (self.manifest_rejection,)
        return self.entry_rejections


class _DuplicateFieldError(ValueError):
    def __init__(self, field: str) -> None:
        super().__init__(field)
        self.field = field


class _InvalidJSONConstantError(ValueError):
    pass


class _ManifestRejected(ValueError):
    def __init__(self, issue: ManifestIssue) -> None:
        super().__init__(issue.message)
        self.issue = issue


def _reject(
    code: ManifestIssueCode,
    path: str,
    message: str,
) -> NoReturn:
    raise _ManifestRejected(ManifestIssue(code, path, message))


def _reject_duplicate_fields(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for field, value in pairs:
        if field in result:
            raise _DuplicateFieldError(field)
        result[field] = value
    return result


def _reject_json_constant(value: str) -> None:
    raise _InvalidJSONConstantError(value)


def _parse_finite_json_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed):
        raise _InvalidJSONConstantError(value)
    return parsed


def _validate_field_set(
    value: dict[str, Any],
    *,
    allowed: frozenset[str],
    required: Sequence[str],
    path: str,
) -> None:
    unknown = min((field for field in value if field not in allowed), default=None)
    if unknown is not None:
        field = unknown
        _reject(
            ManifestIssueCode.SCHEMA_INVALID,
            f"{path}.{field}",
            f"unknown field '{field}'",
        )
    for field in required:
        if field not in value:
            _reject(
                ManifestIssueCode.SCHEMA_INVALID,
                f"{path}.{field}",
                f"missing required field '{field}'",
            )


def _require_object(value: Any, path: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        _reject(
            ManifestIssueCode.SCHEMA_INVALID,
            path,
            "expected a JSON object",
        )
    return value


def _bounded_text(value: Any, path: str) -> str:
    if not isinstance(value, str) or not value:
        _reject(
            ManifestIssueCode.SCHEMA_INVALID,
            path,
            "expected a non-empty string",
        )
    try:
        byte_count = len(value.encode("utf-8"))
    except UnicodeEncodeError:
        _reject(
            ManifestIssueCode.SCHEMA_INVALID,
            path,
            "string is not valid UTF-8",
        )
    if byte_count > MAX_IDENTIFIER_BYTES:
        _reject(
            ManifestIssueCode.SCHEMA_INVALID,
            path,
            f"string exceeds {MAX_IDENTIFIER_BYTES}-byte identifier limit",
        )
    return value


def _bounded_string_list(value: Any, path: str) -> tuple[str, ...]:
    if not isinstance(value, list):
        _reject(
            ManifestIssueCode.SCHEMA_INVALID,
            path,
            "expected a JSON array",
        )
    if len(value) > MAX_TARGET_CAPABILITIES:
        _reject(
            ManifestIssueCode.TOO_MANY_ENTRIES,
            path,
            f"array exceeds {MAX_TARGET_CAPABILITIES}-entry limit",
        )
    return tuple(
        _bounded_text(item, f"{path}[{index}]")
        for index, item in enumerate(value)
    )


def _validate_runtime_abi(value: Any) -> RuntimeABI:
    path = "$.runtime_abi"
    runtime_abi = _require_object(value, path)
    _validate_field_set(
        runtime_abi,
        allowed=_RUNTIME_ABI_FIELDS,
        required=_RUNTIME_ABI_REQUIRED_FIELDS,
        path=path,
    )
    return RuntimeABI(
        python_version=_bounded_text(
            runtime_abi["python_version"], f"{path}.python_version"
        ),
        soabi=_bounded_text(runtime_abi["soabi"], f"{path}.soabi"),
        core_build_id=_bounded_text(
            runtime_abi["core_build_id"], f"{path}.core_build_id"
        ),
        cpu_caps=_bounded_string_list(runtime_abi["cpu_caps"], f"{path}.cpu_caps"),
    )


def _validate_adapter(value: Any) -> PluginAdapter:
    path = "$.adapter"
    adapter = _require_object(value, path)
    _validate_field_set(
        adapter,
        allowed=_ADAPTER_FIELDS,
        required=_ADAPTER_REQUIRED_FIELDS,
        path=path,
    )
    return PluginAdapter(
        entry=_bounded_text(adapter["entry"], f"{path}.entry"),
        target=_bounded_text(adapter["target"], f"{path}.target"),
    )


def _provided_entry_count(provides: dict[str, Any]) -> int:
    count = 0
    for section in _LIST_PROVIDE_SECTIONS:
        if section not in provides:
            continue
        value = provides[section]
        count += len(value) if isinstance(value, list) else 1
    for section in _SINGLETON_PROVIDE_SECTIONS:
        count += int(section in provides)
    return count


def _validate_provide_budgets(provides: dict[str, Any]) -> None:
    for section in _LIST_PROVIDE_SECTIONS:
        value = provides.get(section)
        if isinstance(value, list) and len(value) > MAX_ENTRIES_PER_SECTION:
            _reject(
                ManifestIssueCode.TOO_MANY_ENTRIES,
                f"$.provides.{section}",
                f"section exceeds {MAX_ENTRIES_PER_SECTION}-entry limit",
            )
    if _provided_entry_count(provides) > MAX_PROVIDE_ENTRIES:
        _reject(
            ManifestIssueCode.TOO_MANY_ENTRIES,
            "$.provides",
            f"provides exceeds {MAX_PROVIDE_ENTRIES}-entry limit",
        )


def _freeze_json(value: Any) -> object:
    if isinstance(value, dict):
        frozen = {key: _freeze_json(item) for key, item in value.items()}
        return MappingProxyType(frozen)
    if isinstance(value, list):
        return tuple(_freeze_json(item) for item in value)
    return value


def _entry_issue(path: str, message: str) -> ManifestIssue:
    return ManifestIssue(ManifestIssueCode.ENTRY_INVALID, path, message)


def _validate_object_entries(
    provides: dict[str, Any],
    section: str,
    rejections: list[ManifestIssue],
) -> tuple[Mapping[str, object], ...]:
    if section not in provides:
        return ()
    value = provides[section]
    path = f"$.provides.{section}"
    if not isinstance(value, list):
        rejections.append(_entry_issue(path, "expected a JSON array"))
        return ()

    accepted: list[Mapping[str, object]] = []
    for index, entry in enumerate(value):
        entry_path = f"{path}[{index}]"
        if not isinstance(entry, dict):
            rejections.append(_entry_issue(entry_path, "expected a JSON object"))
            continue
        try:
            frozen = _freeze_json(entry)
        except RecursionError:
            rejections.append(
                _entry_issue(entry_path, "JSON object nesting is too deep")
            )
            continue
        assert isinstance(frozen, Mapping)
        accepted.append(frozen)
    return tuple(accepted)


def _is_safe_seed_path(value: Any) -> bool:
    if not isinstance(value, str) or not value or "\\" in value or "\x00" in value:
        return False
    try:
        value.encode("utf-8")
    except UnicodeEncodeError:
        return False
    path = PurePosixPath(value)
    return (
        not path.is_absolute()
        and path != PurePosixPath(".")
        and ".." not in path.parts
    )


def _validate_seed_entries(
    provides: dict[str, Any],
    rejections: list[ManifestIssue],
) -> tuple[str, ...]:
    if "seeds" not in provides:
        return ()
    value = provides["seeds"]
    path = "$.provides.seeds"
    if not isinstance(value, list):
        rejections.append(_entry_issue(path, "expected a JSON array"))
        return ()

    accepted: list[str] = []
    for index, entry in enumerate(value):
        if not _is_safe_seed_path(entry):
            rejections.append(
                _entry_issue(
                    f"{path}[{index}]",
                    "expected a safe non-empty POSIX-relative path",
                )
            )
            continue
        accepted.append(entry)
    return tuple(accepted)


def _validate_singleton_object(
    provides: dict[str, Any],
    section: str,
    rejections: list[ManifestIssue],
) -> Mapping[str, object] | None:
    if section not in provides:
        return None
    value = provides[section]
    path = f"$.provides.{section}"
    if not isinstance(value, dict):
        rejections.append(_entry_issue(path, "expected a JSON object"))
        return None
    try:
        frozen = _freeze_json(value)
    except RecursionError:
        rejections.append(_entry_issue(path, "JSON object nesting is too deep"))
        return None
    assert isinstance(frozen, Mapping)
    return frozen


def _validate_provides(
    value: Any,
) -> tuple[PluginProvides, tuple[ManifestIssue, ...]]:
    path = "$.provides"
    provides = _require_object(value, path)
    _validate_field_set(
        provides,
        allowed=_PROVIDES_FIELDS,
        required=(),
        path=path,
    )
    _validate_provide_budgets(provides)

    rejections: list[ManifestIssue] = []
    validated = PluginProvides(
        contracts=_validate_object_entries(provides, "contracts", rejections),
        policies=_validate_object_entries(provides, "policies", rejections),
        seeds=_validate_seed_entries(provides, rejections),
        diagnostics=_validate_singleton_object(
            provides, "diagnostics", rejections
        ),
        pass_config=_validate_singleton_object(provides, "pass", rejections),
    )
    return validated, tuple(rejections)


def _validate_parsed_manifest(
    value: Any,
    *,
    allow_unsupported_spi: bool,
) -> tuple[PluginManifest, tuple[ManifestIssue, ...]]:
    manifest = _require_object(value, "$")
    _validate_field_set(
        manifest,
        allowed=_TOP_LEVEL_FIELDS,
        required=_TOP_LEVEL_REQUIRED_FIELDS,
        path="$",
    )

    plugin_id = _bounded_text(manifest["id"], "$.id")
    spi_version = _bounded_text(manifest["spi_version"], "$.spi_version")
    if not allow_unsupported_spi and spi_version != CURRENT_SPI_VERSION:
        _reject(
            ManifestIssueCode.SCHEMA_INVALID,
            "$.spi_version",
            f"expected SPI/schema major '{CURRENT_SPI_VERSION}'",
        )
    runtime_abi = _validate_runtime_abi(manifest["runtime_abi"])
    target_capabilities = _bounded_string_list(
        manifest["target_capabilities"], "$.target_capabilities"
    )
    provides, entry_rejections = _validate_provides(manifest["provides"])
    adapter = (
        _validate_adapter(manifest["adapter"])
        if "adapter" in manifest
        else None
    )
    return (
        PluginManifest(
            id=plugin_id,
            spi_version=spi_version,
            runtime_abi=runtime_abi,
            target_capabilities=target_capabilities,
            provides=provides,
            adapter=adapter,
        ),
        entry_rejections,
    )


def _rejected(issue: ManifestIssue) -> ManifestValidationResult:
    return ManifestValidationResult(
        manifest=None,
        manifest_rejection=issue,
    )


def _bounded_utf8_size(payload: str) -> int:
    """Count UTF-8 bytes with bounded transient memory.

    Scanning continues after the limit so invalid Unicode has the same
    rejection precedence as encoding the complete string at once.
    """

    byte_count = 0
    for offset in range(0, len(payload), _UTF8_SIZE_CHUNK_CHARS):
        chunk = payload[offset : offset + _UTF8_SIZE_CHUNK_CHARS]
        chunk_size = len(chunk.encode("utf-8"))
        if byte_count <= MAX_MANIFEST_BYTES:
            byte_count = min(
                MAX_MANIFEST_BYTES + 1,
                byte_count + chunk_size,
            )
    return byte_count


def validate_manifest(
    payload: str | bytes,
    *,
    allow_unsupported_spi: bool = False,
) -> ManifestValidationResult:
    """Validate one v1 manifest without importing plugin-owned modules."""

    if not isinstance(payload, (str, bytes)):
        raise TypeError("payload must be str or bytes")
    try:
        payload_size = (
            len(payload) if isinstance(payload, bytes) else _bounded_utf8_size(payload)
        )
    except UnicodeEncodeError:
        return _rejected(
            ManifestIssue(
                ManifestIssueCode.MALFORMED_JSON,
                "$",
                "manifest text is not valid UTF-8",
            )
        )
    if payload_size > MAX_MANIFEST_BYTES:
        return _rejected(
            ManifestIssue(
                ManifestIssueCode.MANIFEST_TOO_LARGE,
                "$",
                f"manifest exceeds {MAX_MANIFEST_BYTES}-byte limit",
            )
        )

    try:
        parsed = json.loads(
            payload,
            object_pairs_hook=_reject_duplicate_fields,
            parse_constant=_reject_json_constant,
            parse_float=_parse_finite_json_float,
        )
    except _DuplicateFieldError as error:
        return _rejected(
            ManifestIssue(
                ManifestIssueCode.DUPLICATE_FIELD,
                "$",
                f"duplicate JSON field '{error.field}'",
            )
        )
    except (UnicodeError, RecursionError, ValueError):
        return _rejected(
            ManifestIssue(
                ManifestIssueCode.MALFORMED_JSON,
                "$",
                "manifest is not valid JSON",
            )
        )

    try:
        manifest, entry_rejections = _validate_parsed_manifest(
            parsed,
            allow_unsupported_spi=allow_unsupported_spi,
        )
    except _ManifestRejected as error:
        return _rejected(error.issue)
    except RecursionError:
        return _rejected(
            ManifestIssue(
                ManifestIssueCode.SCHEMA_INVALID,
                "$",
                "manifest nesting is too deep",
            )
        )
    return ManifestValidationResult(
        manifest=manifest,
        manifest_rejection=None,
        entry_rejections=entry_rejections,
    )


def parse_manifest(payload: str | bytes) -> ManifestValidationResult:
    """Alias for callers that treat validation as the manifest parse step."""

    return validate_manifest(payload)
