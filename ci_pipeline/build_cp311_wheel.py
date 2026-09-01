#!/usr/bin/env python3
"""Build and smoke-test the CPython 3.11 CinderX wheel.

The 3.11 counterpart of the cp314 fat-wheel driver, minus fat/manylinux by
design: the 3.11 product targets exactly the anchored openEuler 24.03-LTS-SP3
environment, so the deliverable is one ordinary cp311-cp311-linux_aarch64
wheel whose runnability is proven on a stock openEuler image instead of by
manylinux tags.

Flow:
  1. run the release builder image (scripts/build_cp311_wheel_in_container.sh):
     builder preflight, interpreter config snapshot, pip wheel with a static
     C++ runtime, then normalize_cp311_wheel.py -- deterministic zip
     metadata, structure and NEEDED-allowlist enforcement, embedded
     provenance;
  2. write a .sha256 sidecar (the release-upload convention of
     build_with_tag/gitcode_webhook.py) plus a hash-locked requirements
     file, and a host build manifest under <output-dir>/logs/;
  3. smoke-test the wheel in a stock openEuler container: distro python3
     rpm plus pip, no toolchain, repo not mounted, pip in hash-checking
     mode -- the kunpeng unit suite runs against the installed wheel
     (scripts/smoke_cp311_wheel_in_runtime.sh).

SOURCE_DATE_EPOCH defaults to the HEAD commit time so the wheel's zip layer
is deterministic per commit.  Tag-triggered publishing runs this through
ci_pipeline/build_release_wheels.py; build+smoke is the whole scope here.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path


DEFAULT_IMAGE = "cinderx-cp311-builder:v1"
DEFAULT_RUNTIME_IMAGE = "openeuler/openeuler:24.03-lts-sp3"
# Keeps fd-heavy tests fast: some docker daemons default nofile to 2**30,
# which turns close_fds-style loops into multi-minute crawls.
ULIMIT_NOFILE = "nofile=65536:65536"

REPO_ROOT = Path(__file__).resolve().parents[1]


def run(cmd: list[str], env: dict[str, str] | None = None) -> None:
    print("+", " ".join(cmd), flush=True)
    merged = dict(os.environ, **env) if env else None
    subprocess.run(cmd, check=True, env=merged)


def capture(cmd: list[str]) -> str | None:
    try:
        return subprocess.run(
            cmd, check=True, capture_output=True, text=True
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_date_epoch() -> str | None:
    explicit = os.environ.get("SOURCE_DATE_EPOCH")
    if explicit:
        return explicit
    return capture(["git", "-C", str(REPO_ROOT), "log", "-1", "--format=%ct"])


def build(image: str, out_dir: Path, git_sha: str, deps_cache: Path | None) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    # Scoped to this build's own tag: the release wheelhouse is shared with
    # the cp314 fat wheel, which must survive a cp311 rebuild.
    for stale in out_dir.glob("cinderx-*-cp311-*.whl*"):
        stale.unlink()

    env: dict[str, str] = {"CINDERX_GIT_SHA": git_sha, "CINDERX_BUILDER_IMAGE": image}
    epoch = source_date_epoch()
    if epoch:
        env["SOURCE_DATE_EPOCH"] = epoch
    cmd = [
        "docker", "run", "--rm",
        "--ulimit", ULIMIT_NOFILE,
        # A linked worktree's /src/.git points outside the mount. Git's
        # system-config command still probes the current directory, so start
        # outside /src; the build script enters its copied source explicitly.
        "--workdir", "/tmp",
        "-v", f"{REPO_ROOT}:/src:ro",
        "-v", f"{out_dir}:/out",
        # Pass-through knobs; docker omits any that are unset here.
        "-e", "CINDERX_VERSION_PATCH",
        "-e", "CINDERX_SKIP_BUILDER_CHECK",
        "-e", "CMAKE_BUILD_TYPE",
        "-e", "CMAKE_BUILD_PARALLEL_LEVEL",
        "-e", "SOURCE_DATE_EPOCH",
        "-e", "CINDERX_GIT_SHA",
        "-e", "CINDERX_BUILDER_IMAGE",
        "-e", "CINDERX_PYTHON3_NVR",
        "-e", "CINDERX_GIT_INSECURE",
        "-e", "CINDERX_CP311_PYTHON",
        "-e", "CC",
        "-e", "CXX",
    ]
    # Opt-in local FetchContent cache, the repo's documented offline
    # mechanism (README: CINDERX_LOCAL_DEPS).
    if deps_cache is not None:
        deps_cache.mkdir(parents=True, exist_ok=True)
        cmd += ["-v", f"{deps_cache}:/deps", "-e", "CINDERX_LOCAL_DEPS=/deps"]
    # Production git-mirror convention, same as the cp314 driver: mount the
    # host's dependency mirrors when they exist, so dependency clones never
    # have to reach the public hosts.
    if Path("/root/.gitconfig").is_file() and Path("/opt/cinderx-git-mirrors").is_dir():
        cmd += ["-v", "/root/.gitconfig:/root/.gitconfig:ro"]
        cmd += ["-v", "/opt/cinderx-git-mirrors:/opt/cinderx-git-mirrors:ro"]
    cmd += [image, "bash", "/src/ci_pipeline/scripts/build_cp311_wheel_in_container.sh"]
    run(cmd, env=env)
    wheels = sorted(out_dir.glob("cinderx-*-cp311-*.whl"))
    if not wheels:
        raise SystemExit(f"no cp311 cinderx wheel produced in {out_dir}")
    if len(wheels) > 1:
        raise SystemExit(f"expected exactly one cp311 wheel in {out_dir}, found: {wheels}")
    return wheels[0]


def write_upload_evidence(wheel: Path) -> str:
    digest = sha256_file(wheel)
    sidecar = wheel.with_name(f"{wheel.name}.sha256")
    sidecar.write_text(f"{digest}  {wheel.name}\n", encoding="utf-8")
    # Hash-locked requirements for the smoke's pip hash-checking mode; the
    # path is the wheel's location inside the smoke container.
    requirements = wheel.with_name(f"{wheel.name}.requirements.txt")
    requirements.write_text(f"/wheels/{wheel.name} --hash=sha256:{digest}\n", encoding="utf-8")
    return digest


def write_host_manifest(
    out_dir: Path, image: str, runtime_image: str, wheel: Path, digest: str, git_sha: str
) -> None:
    logs = out_dir / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    image_inspect = capture(["docker", "image", "inspect", image])
    manifest = {
        "wheel": wheel.name,
        "sha256": digest,
        "builder_image": image,
        "builder_image_inspect": json.loads(image_inspect) if image_inspect else None,
        "runtime_image": runtime_image,
        "git_sha": git_sha,
        "git_status": capture(["git", "-C", str(REPO_ROOT), "status", "--short"]),
        "source_date_epoch": source_date_epoch(),
    }
    (logs / "host-build-manifest-311.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def smoke(runtime_image: str, out_dir: Path) -> None:
    scripts = REPO_ROOT / "ci_pipeline" / "scripts"
    tests = REPO_ROOT / "cinderx" / "PythonLib" / "test_cinderx"
    run(
        [
            "docker", "run", "--rm",
            "--ulimit", ULIMIT_NOFILE,
            "-v", f"{out_dir}:/wheels:ro",
            "-v", f"{scripts / 'smoke_cp311_wheel_in_runtime.sh'}:/smoke/run.sh:ro",
            "-v", f"{tests}:/smoke/test_cinderx:ro",
            "-e", "CINDERX_PYTHON3_NVR",
            "-e", "CINDERX_CP311_PYTHON",
            "-w", "/smoke",
            runtime_image,
            "bash", "/smoke/run.sh",
        ]
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--image", default=DEFAULT_IMAGE, help="release builder image")
    parser.add_argument(
        "--runtime-image",
        default=DEFAULT_RUNTIME_IMAGE,
        help="stock image the wheel must run on",
    )
    parser.add_argument(
        "--output-dir",
        default=str(REPO_ROOT / "wheelhouse"),
        help="wheel output directory (the webhook uploads from wheelhouse/)",
    )
    parser.add_argument(
        "--deps-cache",
        default=None,
        help="optional host dir for the repo's CINDERX_LOCAL_DEPS FetchContent cache",
    )
    parser.add_argument("--skip-smoke", action="store_true", help="build only")
    args = parser.parse_args()

    out_dir = Path(args.output_dir).resolve()
    git_sha = capture(["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"]) or "unknown"
    deps_cache = Path(args.deps_cache).resolve() if args.deps_cache else None
    wheel = build(args.image, out_dir, git_sha, deps_cache)
    digest = write_upload_evidence(wheel)
    write_host_manifest(out_dir, args.image, args.runtime_image, wheel, digest, git_sha)
    print(f"[cp311-wheel] wheel  {wheel}")
    print(f"[cp311-wheel] sha256 {digest}")

    if args.skip_smoke:
        print("[cp311-wheel] smoke skipped")
        return
    smoke(args.runtime_image, out_dir)
    print("[cp311-wheel] smoke ok on", args.runtime_image)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        sys.exit(exc.returncode)
