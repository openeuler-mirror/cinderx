# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict
"""Static declarations for the CinderX plugin framework."""

from .discovery import (
    DiscoveryIssue,
    DiscoveryIssueCode,
    MANIFEST_FILENAME,
    PluginDiscoveryResult,
    discover,
    normalize_distribution_name,
)
from .manifest import (
    CURRENT_SPI_VERSION,
    MAX_ENTRIES_PER_SECTION,
    MAX_IDENTIFIER_BYTES,
    MAX_MANIFEST_BYTES,
    MAX_PROVIDE_ENTRIES,
    MAX_TARGET_CAPABILITIES,
    ManifestIssue,
    ManifestIssueCode,
    ManifestValidationResult,
    PluginAdapter,
    PluginManifest,
    PluginProvides,
    RuntimeABI,
    parse_manifest,
    validate_manifest,
)

__all__ = [
    "CURRENT_SPI_VERSION",
    "DiscoveryIssue",
    "DiscoveryIssueCode",
    "MANIFEST_FILENAME",
    "MAX_ENTRIES_PER_SECTION",
    "MAX_IDENTIFIER_BYTES",
    "MAX_MANIFEST_BYTES",
    "MAX_PROVIDE_ENTRIES",
    "MAX_TARGET_CAPABILITIES",
    "ManifestIssue",
    "ManifestIssueCode",
    "ManifestValidationResult",
    "PluginAdapter",
    "PluginDiscoveryResult",
    "PluginManifest",
    "PluginProvides",
    "RuntimeABI",
    "discover",
    "normalize_distribution_name",
    "parse_manifest",
    "validate_manifest",
]
