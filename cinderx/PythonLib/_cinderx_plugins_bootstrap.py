# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Lightweight startup scheduling for static CinderX plugin discovery."""

import importlib
from importlib import metadata
from itertools import chain
import os
from pathlib import PurePosixPath


MANIFEST_FILENAME = "cinderx_plugin.json"
MAX_MANIFEST_BYTES = 64 * 1024
DISCOVERY_DISABLE_ENV = "CINDERX_PLUGIN_DISCOVERY_DISABLE"

_discovery_started = False
_discovery_results = ()
_MAX_CONSECUTIVE_ENUMERATION_ERRORS = 1
_BOUNDED_READ_UNAVAILABLE = object()


class _ManifestCandidate:
    def __init__(self, distribution, payload, read_error=None):
        self._distribution = distribution
        self._payload = payload
        self._read_error = read_error

    @property
    def metadata(self):
        return self._distribution.metadata

    def read_text(self, filename):
        if filename != MANIFEST_FILENAME:
            return self._distribution.read_text(filename)
        if self._read_error is not None:
            raise self._read_error
        return self._payload


def _env_flag_enabled(name):
    return os.environ.get(name, "0").lower() in ("1", "true", "yes", "on")


def _discovery_disabled():
    return _env_flag_enabled(DISCOVERY_DISABLE_ENV) or _env_flag_enabled(
        "CINDERX_DISABLE"
    )


def _read_located_manifest(distribution):
    if isinstance(distribution, metadata.PathDistribution):
        metadata_path = getattr(distribution, "_path", None)
        if metadata_path is None:
            return _BOUNDED_READ_UNAVAILABLE
        normalized_path = PurePosixPath(
            str(metadata_path).replace("\\", "/")
        )
        if not normalized_path.name.endswith(".dist-info"):
            return None
        try:
            with metadata_path.joinpath(MANIFEST_FILENAME).open("rb") as source:
                return source.read(MAX_MANIFEST_BYTES + 1)
        except FileNotFoundError:
            return None

    try:
        files = distribution.files
    except Exception:
        return _BOUNDED_READ_UNAVAILABLE
    if files is None:
        return _BOUNDED_READ_UNAVAILABLE

    for filename in files:
        path = PurePosixPath(str(filename).replace("\\", "/"))
        if (
            path.name != MANIFEST_FILENAME
            or not path.parent.name.endswith(".dist-info")
        ):
            continue
        located = distribution.locate_file(filename)
        with located.open("rb") as source:
            return source.read(MAX_MANIFEST_BYTES + 1)
    return None


def _read_manifest(distribution):
    payload = _read_located_manifest(distribution)
    if payload is _BOUNDED_READ_UNAVAILABLE:
        return distribution.read_text(MANIFEST_FILENAME)
    return payload


def _candidate_distributions():
    installed = iter(metadata.distributions())
    consecutive_errors = 0
    while True:
        try:
            distribution = next(installed)
        except StopIteration:
            return
        except Exception:
            consecutive_errors += 1
            if consecutive_errors > _MAX_CONSECUTIVE_ENUMERATION_ERRORS:
                return
            continue
        consecutive_errors = 0
        try:
            payload = _read_manifest(distribution)
        except Exception as error:
            yield _ManifestCandidate(distribution, None, error)
        else:
            if payload is not None:
                yield _ManifestCandidate(distribution, payload)


def bootstrap():
    """Schedule one deterministic discovery pass when a candidate exists."""

    global _discovery_results, _discovery_started

    if _discovery_started:
        return _discovery_results
    _discovery_started = True

    if _discovery_disabled():
        return _discovery_results

    try:
        candidates = iter(_candidate_distributions())
        first = next(candidates, None)
        if first is None:
            return _discovery_results
        plugins = importlib.import_module("cinderx.plugins")
        _discovery_results = plugins.discover(
            distributions=chain((first,), candidates)
        )
    except Exception:
        # Startup remains usable if metadata enumeration or the framework
        # itself is unavailable. Per-distribution read/validation failures are
        # represented by discover() and do not reach this boundary.
        _discovery_results = ()
    return _discovery_results
