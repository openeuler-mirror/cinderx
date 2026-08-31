# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict
"""Namespaced, immutable publication of validated plugin declarations.

The registry exposes four logical entry families: contracts, policies,
admission data (seeds and diagnostics), and pass configuration.  Seeds,
diagnostics, and pass configuration are immutable leaves; contracts and
policies may depend on another contract or policy in the same namespace.

Publication is copy-on-write.  A plugin is fully staged before the publication
lock is acquired, and readers only load the current immutable snapshot.  This
module does not inspect or import a plugin's adapter.
"""

from __future__ import annotations

import threading
from collections import Counter
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from enum import Enum
from types import MappingProxyType

from .manifest import MAX_IDENTIFIER_BYTES, PluginManifest


class RegistryRejectionReason(str, Enum):
    """Stable machine-readable registry rejection reasons."""

    DUPLICATE_NAMESPACE = "duplicate_namespace"
    INVALID_ENTRY = "invalid_entry"
    DUPLICATE_ENTRY_ID = "duplicate_entry_id"
    MISSING_DEPENDENCY = "missing_dependency"
    REJECTED_DEPENDENCY = "rejected_dependency"
    DEPENDENCY_CYCLE = "dependency_cycle"


class RegistryEntryFamily(str, Enum):
    """The four logical families consumed by downstream components."""

    CONTRACT = "contract"
    POLICY = "policy"
    ADMISSION_DATA = "admission_data"
    PASS_CONFIGURATION = "pass_configuration"


@dataclass(frozen=True, slots=True)
class RegistryRejection:
    namespace: str
    reason: RegistryRejectionReason
    path: str
    message: str
    entry_id: str | None = None
    dependency: str | None = None
    cycle: tuple[str, ...] = ()


@dataclass(frozen=True, slots=True)
class RegistryEntry:
    """One immutable declaration tagged with its publication identity."""

    namespace: str
    generation: int
    family: RegistryEntryFamily
    id: str
    value: object
    depends_on: tuple[str, ...] = ()


@dataclass(frozen=True, slots=True)
class AdmissionInputs:
    """The policy and admission-data buckets for one plugin namespace."""

    policies: Mapping[str, RegistryEntry]
    seeds: tuple[RegistryEntry, ...]
    diagnostics: RegistryEntry | None


@dataclass(frozen=True, slots=True)
class NamespaceSnapshot:
    """All declaration buckets atomically published for one plugin."""

    namespace: str
    generation: int
    contracts: Mapping[str, RegistryEntry]
    admission_inputs: AdmissionInputs
    pass_configuration: RegistryEntry | None


@dataclass(frozen=True, slots=True)
class RegistrySnapshot:
    """An immutable point-in-time view of all published namespaces."""

    generation: int
    namespaces: Mapping[str, NamespaceSnapshot]

    def for_namespace(self, namespace: str) -> NamespaceSnapshot | None:
        """Return only the entries registered under ``namespace``."""

        return self.namespaces.get(namespace)


@dataclass(frozen=True, slots=True)
class RegistrationResult:
    """The snapshot visible after one attempted plugin publication."""

    namespace: str
    published: bool
    snapshot: RegistrySnapshot
    rejections: tuple[RegistryRejection, ...] = ()

    @property
    def generation(self) -> int:
        return self.snapshot.generation


@dataclass(frozen=True, slots=True)
class _SourceEntry:
    family: RegistryEntryFamily
    bucket: str
    bucket_order: int
    index: int
    path: str
    value: Mapping[str, object]
    entry_id: str | None


@dataclass(frozen=True, slots=True)
class _StagedEntry:
    family: RegistryEntryFamily
    bucket: str
    bucket_order: int
    index: int
    id: str
    value: object
    depends_on: tuple[str, ...] = ()


@dataclass(frozen=True, slots=True)
class _StagedNamespace:
    namespace: str
    contracts: tuple[_StagedEntry, ...]
    policies: tuple[_StagedEntry, ...]
    seeds: tuple[_StagedEntry, ...]
    diagnostics: _StagedEntry | None
    pass_configuration: _StagedEntry | None
    rejections: tuple[RegistryRejection, ...]
    cycle: tuple[str, ...] = ()


def _is_bounded_id(value: object) -> bool:
    if not isinstance(value, str) or not value:
        return False
    try:
        return len(value.encode("utf-8")) <= MAX_IDENTIFIER_BYTES
    except UnicodeEncodeError:
        return False


def _freeze_json(value: object) -> object:
    """Copy an A-1 JSON value into recursively immutable containers."""

    if isinstance(value, Mapping):
        frozen = {key: _freeze_json(item) for key, item in value.items()}
        return MappingProxyType(frozen)
    if isinstance(value, (tuple, list)):
        return tuple(_freeze_json(item) for item in value)
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    raise TypeError(f"unsupported declaration value {type(value).__name__}")


def _rejection(
    namespace: str,
    reason: RegistryRejectionReason,
    path: str,
    message: str,
    *,
    entry_id: str | None = None,
    dependency: str | None = None,
    cycle: tuple[str, ...] = (),
) -> RegistryRejection:
    return RegistryRejection(
        namespace=namespace,
        reason=reason,
        path=path,
        message=message,
        entry_id=entry_id,
        dependency=dependency,
        cycle=cycle,
    )


def _source_entries(manifest: PluginManifest) -> tuple[_SourceEntry, ...]:
    sources: list[_SourceEntry] = []
    buckets = (
        (
            RegistryEntryFamily.CONTRACT,
            "contracts",
            manifest.provides.contracts,
        ),
        (
            RegistryEntryFamily.POLICY,
            "policies",
            manifest.provides.policies,
        ),
    )
    for bucket_order, (family, bucket, entries) in enumerate(buckets):
        for index, entry in enumerate(entries):
            entry_id_value = entry.get("id")
            entry_id = entry_id_value if _is_bounded_id(entry_id_value) else None
            sources.append(
                _SourceEntry(
                    family=family,
                    bucket=bucket,
                    bucket_order=bucket_order,
                    index=index,
                    path=f"$.provides.{bucket}[{index}]",
                    value=entry,
                    entry_id=entry_id,
                )
            )
    return tuple(sources)


def _stage_dependent_entries(
    manifest: PluginManifest,
) -> tuple[tuple[_StagedEntry, ...], tuple[RegistryRejection, ...], tuple[str, ...]]:
    namespace = manifest.id
    sources = _source_entries(manifest)
    id_counts = Counter(
        source.entry_id for source in sources if source.entry_id is not None
    )
    declared_ids = frozenset(id_counts)
    staged: list[_StagedEntry] = []
    ordered_rejections: list[tuple[tuple[int, int], RegistryRejection]] = []
    rejected_ids: set[str] = set()

    for source in sources:
        raw_id = source.value.get("id")
        if source.entry_id is None:
            entry_id = raw_id if isinstance(raw_id, str) else None
            ordered_rejections.append(
                (
                    (source.bucket_order, source.index),
                    _rejection(
                        namespace,
                        RegistryRejectionReason.INVALID_ENTRY,
                        source.path,
                        (
                            "entry 'id' must be a non-empty UTF-8 string no longer "
                            f"than {MAX_IDENTIFIER_BYTES} bytes"
                        ),
                        entry_id=entry_id,
                    ),
                )
            )
            continue

        entry_id = source.entry_id
        if id_counts[entry_id] > 1:
            rejected_ids.add(entry_id)
            ordered_rejections.append(
                (
                    (source.bucket_order, source.index),
                    _rejection(
                        namespace,
                        RegistryRejectionReason.DUPLICATE_ENTRY_ID,
                        source.path,
                        f"entry id '{entry_id}' is duplicated in this namespace",
                        entry_id=entry_id,
                    ),
                )
            )
            continue

        raw_dependencies = source.value.get("depends_on", ())
        if not isinstance(raw_dependencies, (tuple, list)):
            rejected_ids.add(entry_id)
            ordered_rejections.append(
                (
                    (source.bucket_order, source.index),
                    _rejection(
                        namespace,
                        RegistryRejectionReason.INVALID_ENTRY,
                        source.path,
                        "entry 'depends_on' must be an array of bounded strings",
                        entry_id=entry_id,
                    ),
                )
            )
            continue
        if any(not _is_bounded_id(item) for item in raw_dependencies):
            rejected_ids.add(entry_id)
            ordered_rejections.append(
                (
                    (source.bucket_order, source.index),
                    _rejection(
                        namespace,
                        RegistryRejectionReason.INVALID_ENTRY,
                        source.path,
                        "entry 'depends_on' must contain only bounded strings",
                        entry_id=entry_id,
                    ),
                )
            )
            continue

        try:
            frozen_value = _freeze_json(source.value)
        except (RecursionError, TypeError) as error:
            rejected_ids.add(entry_id)
            ordered_rejections.append(
                (
                    (source.bucket_order, source.index),
                    _rejection(
                        namespace,
                        RegistryRejectionReason.INVALID_ENTRY,
                        source.path,
                        f"entry cannot be frozen: {error}",
                        entry_id=entry_id,
                    ),
                )
            )
            continue

        dependencies = tuple(raw_dependencies)
        staged.append(
            _StagedEntry(
                family=source.family,
                bucket=source.bucket,
                bucket_order=source.bucket_order,
                index=source.index,
                id=entry_id,
                value=frozen_value,
                depends_on=dependencies,
            )
        )

    cycle = _find_cycle(staged)
    if cycle:
        return (), (), cycle

    staged_by_id = {entry.id: entry for entry in staged}
    rejected_entries: set[str] = set(rejected_ids)
    changed = True
    while changed:
        changed = False
        for entry in staged:
            if entry.id in rejected_entries:
                continue
            rejection_reason: RegistryRejectionReason | None = None
            failed_dependency: str | None = None
            for dependency in entry.depends_on:
                if dependency not in declared_ids:
                    rejection_reason = RegistryRejectionReason.MISSING_DEPENDENCY
                    failed_dependency = dependency
                    break
                if dependency in rejected_entries:
                    rejection_reason = RegistryRejectionReason.REJECTED_DEPENDENCY
                    failed_dependency = dependency
                    break
            if rejection_reason is None:
                continue
            rejected_entries.add(entry.id)
            changed = True
            assert failed_dependency is not None
            ordered_rejections.append(
                (
                    (entry.bucket_order, entry.index),
                    _rejection(
                        namespace,
                        rejection_reason,
                        f"$.provides.{entry.bucket}[{entry.index}]",
                        (
                            f"dependency '{failed_dependency}' is missing"
                            if rejection_reason
                            is RegistryRejectionReason.MISSING_DEPENDENCY
                            else f"dependency '{failed_dependency}' was rejected"
                        ),
                        entry_id=entry.id,
                        dependency=failed_dependency,
                    ),
                )
            )

    accepted = tuple(
        entry for entry in staged if entry.id not in rejected_entries
    )
    # The lookup is deliberately local: no dependency can resolve through a
    # different namespace or a leaf bucket.
    assert all(
        dependency in staged_by_id
        for entry in accepted
        for dependency in entry.depends_on
    )
    ordered_rejections.sort(key=lambda item: item[0])
    return accepted, tuple(item[1] for item in ordered_rejections), ()


def _find_cycle(entries: Sequence[_StagedEntry]) -> tuple[str, ...]:
    entries_by_id = {entry.id: entry for entry in entries}
    state: dict[str, int] = {}
    stack: list[str] = []
    stack_indexes: dict[str, int] = {}

    def visit(entry_id: str) -> tuple[str, ...]:
        state[entry_id] = 1
        stack_indexes[entry_id] = len(stack)
        stack.append(entry_id)
        for dependency in entries_by_id[entry_id].depends_on:
            if dependency not in entries_by_id:
                continue
            dependency_state = state.get(dependency, 0)
            if dependency_state == 0:
                cycle = visit(dependency)
                if cycle:
                    return cycle
            elif dependency_state == 1:
                start = stack_indexes[dependency]
                return (*stack[start:], dependency)
        stack.pop()
        stack_indexes.pop(entry_id)
        state[entry_id] = 2
        return ()

    for entry in entries:
        if state.get(entry.id, 0) == 0:
            cycle = visit(entry.id)
            if cycle:
                return cycle
    return ()


def _stage_leaf(
    namespace: str,
    *,
    family: RegistryEntryFamily,
    bucket: str,
    index: int,
    entry_id: str,
    value: object,
) -> tuple[_StagedEntry | None, RegistryRejection | None]:
    path = (
        f"$.provides.{bucket}[{index}]"
        if index >= 0
        else f"$.provides.{bucket}"
    )
    try:
        frozen_value = _freeze_json(value)
    except (RecursionError, TypeError) as error:
        return (
            None,
            _rejection(
                namespace,
                RegistryRejectionReason.INVALID_ENTRY,
                path,
                f"entry cannot be frozen: {error}",
                entry_id=entry_id,
            ),
        )
    return (
        _StagedEntry(
            family=family,
            bucket=bucket,
            bucket_order=2 if bucket == "seeds" else 3,
            index=index,
            id=entry_id,
            value=frozen_value,
        ),
        None,
    )


def _stage_manifest(manifest: PluginManifest) -> _StagedNamespace:
    accepted, dependent_rejections, cycle = _stage_dependent_entries(manifest)
    if cycle:
        return _StagedNamespace(
            namespace=manifest.id,
            contracts=(),
            policies=(),
            seeds=(),
            diagnostics=None,
            pass_configuration=None,
            rejections=(),
            cycle=cycle,
        )

    leaf_rejections: list[RegistryRejection] = []
    seeds: list[_StagedEntry] = []
    seed_counts = Counter(manifest.provides.seeds)
    for index, seed in enumerate(manifest.provides.seeds):
        if seed_counts[seed] > 1:
            leaf_rejections.append(
                _rejection(
                    manifest.id,
                    RegistryRejectionReason.DUPLICATE_ENTRY_ID,
                    f"$.provides.seeds[{index}]",
                    f"seed entry '{seed}' is duplicated in this namespace",
                    entry_id=seed,
                )
            )
            continue
        staged, rejection = _stage_leaf(
            manifest.id,
            family=RegistryEntryFamily.ADMISSION_DATA,
            bucket="seeds",
            index=index,
            entry_id=seed,
            value=seed,
        )
        if rejection is not None:
            leaf_rejections.append(rejection)
        else:
            assert staged is not None
            seeds.append(staged)

    diagnostics = None
    if manifest.provides.diagnostics is not None:
        diagnostics, rejection = _stage_leaf(
            manifest.id,
            family=RegistryEntryFamily.ADMISSION_DATA,
            bucket="diagnostics",
            index=-1,
            entry_id="diagnostics",
            value=manifest.provides.diagnostics,
        )
        if rejection is not None:
            leaf_rejections.append(rejection)

    pass_configuration = None
    if manifest.provides.pass_config is not None:
        pass_configuration, rejection = _stage_leaf(
            manifest.id,
            family=RegistryEntryFamily.PASS_CONFIGURATION,
            bucket="pass",
            index=-1,
            entry_id="pass",
            value=manifest.provides.pass_config,
        )
        if rejection is not None:
            leaf_rejections.append(rejection)

    return _StagedNamespace(
        namespace=manifest.id,
        contracts=tuple(
            entry
            for entry in accepted
            if entry.family is RegistryEntryFamily.CONTRACT
        ),
        policies=tuple(
            entry
            for entry in accepted
            if entry.family is RegistryEntryFamily.POLICY
        ),
        seeds=tuple(seeds),
        diagnostics=diagnostics,
        pass_configuration=pass_configuration,
        rejections=(*dependent_rejections, *leaf_rejections),
    )


def _published_entry(
    namespace: str,
    generation: int,
    staged: _StagedEntry,
) -> RegistryEntry:
    return RegistryEntry(
        namespace=namespace,
        generation=generation,
        family=staged.family,
        id=staged.id,
        value=staged.value,
        depends_on=staged.depends_on,
    )


def _namespace_snapshot(
    staged: _StagedNamespace,
    generation: int,
) -> NamespaceSnapshot:
    namespace = staged.namespace
    contracts = MappingProxyType(
        {
            entry.id: _published_entry(namespace, generation, entry)
            for entry in staged.contracts
        }
    )
    policies = MappingProxyType(
        {
            entry.id: _published_entry(namespace, generation, entry)
            for entry in staged.policies
        }
    )
    seeds = tuple(
        _published_entry(namespace, generation, entry) for entry in staged.seeds
    )
    diagnostics = (
        _published_entry(namespace, generation, staged.diagnostics)
        if staged.diagnostics is not None
        else None
    )
    pass_configuration = (
        _published_entry(namespace, generation, staged.pass_configuration)
        if staged.pass_configuration is not None
        else None
    )
    return NamespaceSnapshot(
        namespace=namespace,
        generation=generation,
        contracts=contracts,
        admission_inputs=AdmissionInputs(
            policies=policies,
            seeds=seeds,
            diagnostics=diagnostics,
        ),
        pass_configuration=pass_configuration,
    )


class PluginRegistry:
    """Copy-on-write container for namespaced plugin declarations."""

    def __init__(self) -> None:
        self._publication_lock = threading.Lock()
        self._snapshot = RegistrySnapshot(
            generation=0,
            namespaces=MappingProxyType({}),
        )

    @property
    def snapshot(self) -> RegistrySnapshot:
        """Load the current snapshot without locking or executing callbacks."""

        return self._snapshot

    def publish(self, manifest: PluginManifest) -> RegistrationResult:
        """Stage and atomically publish one previously unseen namespace."""

        staged = _stage_manifest(manifest)

        with self._publication_lock:
            current = self._snapshot
            if staged.namespace in current.namespaces:
                rejection = _rejection(
                    staged.namespace,
                    RegistryRejectionReason.DUPLICATE_NAMESPACE,
                    "$.id",
                    f"namespace '{staged.namespace}' is already published",
                )
                return RegistrationResult(
                    namespace=staged.namespace,
                    published=False,
                    snapshot=current,
                    rejections=(rejection,),
                )

            if staged.cycle:
                cycle_text = " -> ".join(staged.cycle)
                rejection = _rejection(
                    staged.namespace,
                    RegistryRejectionReason.DEPENDENCY_CYCLE,
                    "$.provides",
                    f"dependency cycle detected: {cycle_text}",
                    entry_id=staged.cycle[0],
                    cycle=staged.cycle,
                )
                return RegistrationResult(
                    namespace=staged.namespace,
                    published=False,
                    snapshot=current,
                    rejections=(rejection,),
                )

            generation = current.generation + 1
            namespace_snapshot = _namespace_snapshot(staged, generation)
            namespaces = dict(current.namespaces)
            namespaces[staged.namespace] = namespace_snapshot
            published = RegistrySnapshot(
                generation=generation,
                namespaces=MappingProxyType(namespaces),
            )
            # The only publication-visible mutation is this reference swap.
            self._snapshot = published
            return RegistrationResult(
                namespace=staged.namespace,
                published=True,
                snapshot=published,
                rejections=staged.rejections,
            )

__all__ = [
    "AdmissionInputs",
    "NamespaceSnapshot",
    "PluginRegistry",
    "RegistrationResult",
    "RegistryEntry",
    "RegistryEntryFamily",
    "RegistryRejection",
    "RegistryRejectionReason",
    "RegistrySnapshot",
]
