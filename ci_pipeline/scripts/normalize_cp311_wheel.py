#!/usr/bin/env python3
"""Normalize and validate the ordinary cp311 CinderX wheel.

The 3.11 counterpart of the fat repack's consistency layer, sharing its
implementation (build_cp314_fat_wheel.py): deterministic zip metadata driven
by SOURCE_DATE_EPOCH, sorted entries, fixed permissions, a regenerated
RECORD, and an embedded provenance file.  As with the 3.14 flow, the
determinism promise covers the zip layer, not bit-reproducibility of the
native extension itself.

On top of that it enforces the 3.11 product contract:

* wheel tags are exactly cp311-cp311-linux_aarch64;
* exactly one top-level _cinderx native extension;
* Requires-Python metadata is present;
* the extension's NEEDED set stays inside a glibc-family allowlist --
  in particular libstdc++ must be absent, proving the statically linked
  C++ runtime survived the build (a dynamic libstdc++ dependency would
  strand the wheel on the stock image's GCC 12 runtime).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_cp314_fat_wheel as wheel_lib  # noqa: E402


EXPECTED_TAGS = {"python_tag": "cp311", "abi_tag": "cp311", "platform_tag": "linux_aarch64"}
# The wheel supports exactly the anchored interpreter; pip's resolver should
# refuse other 3.11 micro versions instead of the native guard refusing at
# import time.  The project-wide pyproject range stays untouched.
REQUIRES_PYTHON_311 = ">=3.11.6,<3.11.7"
NEEDED_ALLOWLIST = {
    "libc.so.6",
    "libm.so.6",
    "libpthread.so.0",
    "libdl.so.2",
    "librt.so.1",
    "libgcc_s.so.1",
    "libz.so.1",
    # glibc's own dynamic loader; aarch64 links commonly list it as NEEDED.
    "ld-linux-aarch64.so.1",
}
PROVENANCE_PATH = "cinderx/_native/build_info_311.json"
NEEDED_RE = re.compile(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]")
DECLARE_RE = re.compile(r"cinderx_declare_dependency\(\s*([\w-]+)\s+(\S+)\s+(\S+)", re.S)
# The release provenance contract covers what ships in the wheel; the
# RuntimeTests manifest holds test-only dependencies (googletest), which
# deliberately stay on a tag until the mirror lineages are unified
# (issue #18) and therefore are not held to the commit-SHA rule.
DEPENDENCY_MANIFESTS = ("CMakeLists.txt",)


class NormalizeError(Exception):
    pass


def read_needed(so_bytes: bytes) -> list[str]:
    with tempfile.NamedTemporaryFile(suffix=".so", delete=False) as handle:
        handle.write(so_bytes)
        so_path = handle.name
    try:
        output = subprocess.run(
            ["readelf", "-d", so_path], check=True, capture_output=True, text=True
        ).stdout
    finally:
        os.unlink(so_path)
    return NEEDED_RE.findall(output)


def declared_dependencies(source_dir: Path) -> dict[str, dict[str, str]]:
    """Collect the FetchContent dependency pins, insisting on immutability.

    Every cinderx_declare_dependency must reference a full commit SHA: a
    mutable tag would let the dependency host rewrite what this release was
    built from, so reintroducing one fails the release build here.
    """
    deps: dict[str, dict[str, str]] = {}
    for manifest in DEPENDENCY_MANIFESTS:
        path = source_dir / manifest
        if not path.is_file():
            continue
        for name, repository, ref in DECLARE_RE.findall(path.read_text(encoding="utf-8")):
            if not re.fullmatch(r"[0-9a-fA-F]{40}", ref):
                raise NormalizeError(
                    f"dependency {name} in {manifest} is pinned to {ref!r}, "
                    "not an immutable commit SHA"
                )
            deps[name] = {"repository": repository, "commit": ref.lower()}
    if not deps:
        raise NormalizeError(f"no dependency declarations found under {source_dir}")
    return deps


def check_needed(needed: list[str]) -> None:
    if "libstdc++.so.6" in needed:
        raise NormalizeError(
            "extension depends on a dynamic libstdc++; the release wheel must "
            "link the C++ runtime statically to run on the stock image"
        )
    unexpected = sorted(set(needed) - NEEDED_ALLOWLIST)
    if unexpected:
        raise NormalizeError(f"NEEDED entries outside the allowlist: {unexpected}")


def normalize(wheel_path: Path, git_sha: str, builder_image: str, source_dir: Path) -> None:
    entries, parsed = wheel_lib.read_wheel(wheel_path)
    # The hash of the wheel as built, taken before any entry is touched so
    # it can be re-derived from the ordinary input wheel.
    source_sha = wheel_lib.stable_wheel_content_sha256(entries)

    if parsed["distribution"] != "cinderx":
        raise NormalizeError(f"unexpected distribution: {parsed['distribution']}")
    for key, expected in EXPECTED_TAGS.items():
        if parsed[key] != expected:
            raise NormalizeError(f"wheel {key}={parsed[key]!r}, expected {expected!r}")
    if PROVENANCE_PATH in entries:
        raise NormalizeError(f"{wheel_path} is already normalized ({PROVENANCE_PATH} present)")

    dist_info = wheel_lib.find_dist_info(entries)
    metadata_path = f"{dist_info}/METADATA"
    record_path = f"{dist_info}/RECORD"
    if metadata_path not in entries:
        raise NormalizeError(f"wheel is missing {metadata_path}")
    entries[metadata_path] = wheel_lib.update_requires_python(
        entries[metadata_path], REQUIRES_PYTHON_311
    )
    metadata_text = entries[metadata_path].decode("utf-8")
    if f"Requires-Python: {REQUIRES_PYTHON_311}" not in metadata_text.splitlines():
        raise NormalizeError("wheel METADATA did not take the 3.11.6 Requires-Python pin")

    native_name = wheel_lib.find_native_extension(entries, wheel_path)
    needed = read_needed(entries[native_name])
    check_needed(needed)
    print(f"[normalize-cp311] native {native_name} NEEDED: {', '.join(sorted(needed))}")

    provenance = {
        "format": "cinderx-cp311-wheel-v1",
        "wheel": wheel_path.name,
        "git_sha": git_sha,
        "builder_image": builder_image,
        "gcc": subprocess.run(
            [os.environ.get("CXX", "g++"), "-dumpfullversion"],
            check=True, capture_output=True, text=True,
        ).stdout.strip(),
        "python3_nvr": subprocess.run(
            ["rpm", "-q", "python3"], check=True, capture_output=True, text=True
        ).stdout.strip(),
        "source_date_epoch": os.environ.get("SOURCE_DATE_EPOCH"),
        "source_content_sha256": source_sha,
        "native_needed": sorted(needed),
        "requires_python": REQUIRES_PYTHON_311,
        "declared_dependencies": declared_dependencies(source_dir),
    }
    entries[PROVENANCE_PATH] = (
        json.dumps(provenance, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    entries[record_path] = wheel_lib.render_record(entries, record_path)

    zip_date_time = wheel_lib.wheel_zip_date_time()
    tmp_wheel = wheel_path.with_suffix(wheel_path.suffix + ".tmp")
    if tmp_wheel.exists():
        tmp_wheel.unlink()
    with zipfile.ZipFile(tmp_wheel, "w", compression=zipfile.ZIP_DEFLATED) as out:
        for name in sorted(entries):
            out.writestr(wheel_lib.wheel_zip_info(name, zip_date_time), entries[name])
    os.replace(tmp_wheel, wheel_path)

    normalized_entries, _ = wheel_lib.read_wheel(wheel_path)
    print(f"[normalize-cp311] source content sha256 {source_sha}")
    print(
        "[normalize-cp311] normalized content sha256 "
        f"{wheel_lib.stable_wheel_content_sha256(normalized_entries)}"
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wheel", required=True, type=Path)
    parser.add_argument("--git-sha", default="unknown")
    parser.add_argument("--builder-image", default="unknown")
    parser.add_argument(
        "--source-dir",
        type=Path,
        default=Path.cwd(),
        help="source checkout the wheel was built from (dependency manifests)",
    )
    args = parser.parse_args(argv)
    try:
        normalize(args.wheel, args.git_sha, args.builder_image, args.source_dir)
    except (NormalizeError, wheel_lib.WheelError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
