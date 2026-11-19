#!/home/pybin/bin/python3.14
import argparse
import hashlib
import ipaddress
import json
import os
import re
import shlex
import shutil
import socket
import subprocess
import sys
import time
import zipfile
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

if __package__:
    from .release_tag import release_recorded_commit, valid_tag
else:
    from release_tag import release_recorded_commit, valid_tag


ZERO_SHA = "0" * 40


def env(name, default=""):
    return os.environ.get(name, default)


def env_bool(name, default=False):
    value = os.environ.get(name)
    if value is None:
        return default
    return value.lower() in {"1", "true", "yes", "on"}


OWNER = env("GITCODE_OWNER", "openeuler")
REPO = env("GITCODE_REPO", "cinderx")
REPO_URL = env("GITCODE_REPO_URL", f"https://gitcode.com/{OWNER}/{REPO}.git")
API_BASE = env("GITCODE_API_BASE", "https://api.gitcode.com/api/v5").rstrip("/")
ACCESS_TOKEN = env("GITCODE_ACCESS_TOKEN", "")
GIT_USERNAME = env("GITCODE_GIT_USERNAME", "oauth2")
USE_TOKEN_FOR_GIT = env_bool("GITCODE_USE_TOKEN_FOR_GIT", True)
API_HOST = (urllib.parse.urlparse(API_BASE).hostname or "").lower()
DEFAULT_UPLOAD_ALLOWED_HOSTS = ",".join(
    host for host in ("gitcode.com", ".gitcode.com", API_HOST) if host
)
UPLOAD_ALLOWED_HOSTS = {
    host.strip().lower()
    for host in env("GITCODE_UPLOAD_ALLOWED_HOSTS", DEFAULT_UPLOAD_ALLOWED_HOSTS).split(",")
    if host.strip()
}

WORK_BASE = Path(env("CINDERX_WEBHOOK_WORK_BASE", "/var/lib/cinderx-webhook"))
REPO_DIR = Path(env("CINDERX_REPO_DIR", str(WORK_BASE / "repo")))
LOG_DIR = WORK_BASE / "logs"
STATE_DIR = WORK_BASE / "state"
RUN_DIR = WORK_BASE / "run"

BUILD_COMMAND = env(
    "CINDERX_BUILD_COMMAND",
    "/home/pybin/bin/python3.14 ci_pipeline/build_release_wheels.py",
)
# The release wheel set is defined by this repo, not by the operator-pinned
# build command: when the pinned command leaves no cp311 wheel in the
# wheelhouse, the webhook builds it before publishing.  Set to 0/false to
# opt out (the setting survives in /etc/cinderx-webhook.env because the
# Jenkins job only rewrites its own keys).
RELEASE_CP311 = env_bool("CINDERX_RELEASE_CP311", True)
RELEASE_STATUS = env("GITCODE_RELEASE_STATUS", "latest")
STRICT_RELEASE_CREATE = env_bool("GITCODE_STRICT_RELEASE_CREATE", False)
HTTP_TIMEOUT = int(env("CINDERX_WEBHOOK_HTTP_TIMEOUT", "120"))
RELEASE_TAG_RE = re.compile(
    r"^v(?P<major>0|[1-9]\d*)\.(?P<minor>0|[1-9]\d*)\.(?P<patch>0|[1-9]\d*)"
    r"(?:-(?P<prerelease>alpha|beta|rc)\.(?P<pre_num>0|[1-9]\d*))?$"
)
PRERELEASE_ORDER = {"alpha": 0, "beta": 1, "rc": 2}


def log_line(fp, msg):
    stamp = time.strftime("%Y-%m-%d %H:%M:%S %z")
    line = f"[{stamp}] {msg}"
    print(line, flush=True)
    fp.write(line + "\n")
    fp.flush()


def run_cmd(args, cwd, log, env_overrides=None, timeout=None):
    log_line(log, "$ " + " ".join(shlex.quote(a) for a in args))
    child_env = os.environ.copy()
    child_env.pop("GITCODE_ACCESS_TOKEN", None)
    if env_overrides:
        child_env.update(env_overrides)
    proc = subprocess.Popen(
        args,
        cwd=str(cwd),
        env=child_env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    try:
        assert proc.stdout is not None
        with proc.stdout:
            for line in proc.stdout:
                log.write(line)
                log.flush()
                print(line, end="", flush=True)
        code = proc.wait(timeout=timeout)
    except Exception:
        proc.kill()
        proc.wait()
        raise
    if code != 0:
        raise RuntimeError(f"command failed with exit code {code}: {' '.join(args)}")


def check_output(args, cwd):
    return subprocess.check_output(
        args,
        cwd=str(cwd),
        text=True,
        encoding="utf-8",
        errors="replace",
    ).strip()


def validate_release_tag(tag):
    if not valid_tag(tag):
        raise RuntimeError(f"invalid Git tag: {tag!r}")
    return RELEASE_TAG_RE.fullmatch(tag)


def release_tag_key(tag):
    match = RELEASE_TAG_RE.fullmatch(tag)
    if match is None:
        return None
    prerelease = match.group("prerelease")
    pre_num = match.group("pre_num")
    if prerelease:
        release_rank = 0
        pre_key = (PRERELEASE_ORDER[prerelease], int(pre_num))
    else:
        release_rank = 1
        pre_key = (len(PRERELEASE_ORDER), 0)
    return (
        int(match.group("major")),
        int(match.group("minor")),
        int(match.group("patch")),
        release_rank,
        *pre_key,
    )


def make_git_env():
    git_env = {"GIT_TERMINAL_PROMPT": "0"}
    if ACCESS_TOKEN and USE_TOKEN_FOR_GIT:
        RUN_DIR.mkdir(parents=True, exist_ok=True)
        askpass = RUN_DIR / "git-askpass.sh"
        askpass.write_text(
            "#!/usr/bin/env bash\n"
            "case \"$1\" in\n"
            "  *Username*) printf '%s\\n' \"${GITCODE_GIT_USERNAME:-oauth2}\" ;;\n"
            "  *Password*) printf '%s\\n' \"${GITCODE_ACCESS_TOKEN:-}\" ;;\n"
            "  *) printf '%s\\n' \"${GITCODE_ACCESS_TOKEN:-}\" ;;\n"
            "esac\n",
            encoding="utf-8",
        )
        askpass.chmod(0o700)
        git_env.update(
            {
                "GIT_ASKPASS": str(askpass),
                "GITCODE_GIT_USERNAME": GIT_USERNAME,
                "GITCODE_ACCESS_TOKEN": ACCESS_TOKEN,
            }
        )
    return git_env


def ensure_existing_repo(log):
    if not (REPO_DIR / ".git").exists():
        raise RuntimeError(
            f"{REPO_DIR} is not a git repository; clone {REPO_URL} before running this script"
        )
    git_env = make_git_env()
    run_cmd(["git", "remote", "set-url", "origin", REPO_URL], REPO_DIR, log, git_env)
    return git_env


def fetch_tag(tag, sha, log):
    git_env = ensure_existing_repo(log)
    ref = f"refs/tags/{tag}"
    run_cmd(["git", "fetch", "--force", "--tags", "origin", f"+{ref}:{ref}"], REPO_DIR, log, git_env)
    tag_sha = check_output(["git", "rev-parse", "--verify", ref], REPO_DIR)
    commit = check_output(["git", "rev-parse", "--verify", f"{ref}^{{commit}}"], REPO_DIR)
    if sha and sha not in (commit, tag_sha):
        raise RuntimeError(f"event SHA {sha} differs from tag {tag!r} ({tag_sha}, commit {commit})")
    return tag_sha, commit


def checkout_tag(tag, sha, log):
    _, commit = fetch_tag(tag, sha, log)
    run_cmd(["git", "checkout", "--force", "--detach", commit], REPO_DIR, log)
    run_cmd(["git", "clean", "-ffdx"], REPO_DIR, log)
    checked_sha = check_output(["git", "rev-parse", "HEAD"], REPO_DIR)
    log_line(log, f"checked out {tag} at {checked_sha}")
    if checked_sha != commit:
        raise RuntimeError(f"checked out {checked_sha}; expected {commit}")
    return checked_sha


def api_url(path, params=None):
    query = urllib.parse.urlencode(params or {})
    suffix = f"?{query}" if query else ""
    return f"{API_BASE}{path}{suffix}"


def api_headers(extra=None):
    headers = {"Accept": "application/json"}
    if ACCESS_TOKEN:
        headers["PRIVATE-TOKEN"] = ACCESS_TOKEN
    if extra:
        headers.update(extra)
    return headers


def host_allowed(host):
    host = host.lower()
    for allowed in UPLOAD_ALLOWED_HOSTS:
        if allowed.startswith("."):
            if host.endswith(allowed):
                return True
        elif host == allowed:
            return True
    return False


def validate_upload_url(upload_url):
    parsed = urllib.parse.urlparse(upload_url)
    if parsed.scheme != "https":
        raise RuntimeError("upload_url must use https")
    if parsed.username or parsed.password:
        raise RuntimeError("upload_url must not contain credentials")
    if parsed.port not in (None, 443):
        raise RuntimeError("upload_url must use the default https port")

    host = (parsed.hostname or "").lower()
    if not host:
        raise RuntimeError("upload_url must include a host")
    if not host_allowed(host):
        raise RuntimeError(f"upload_url host is not allowed: {host}")

    try:
        addresses = socket.getaddrinfo(host, None, type=socket.SOCK_STREAM)
    except socket.gaierror as exc:
        raise RuntimeError(f"upload_url host cannot be resolved: {host}") from exc
    for addr in addresses:
        ip = ipaddress.ip_address(addr[4][0])
        if not ip.is_global or getattr(ip, "is_site_local", False):
            raise RuntimeError(f"upload_url resolves to disallowed address: {ip}")


def request_json(method, path, params=None, body=None, expected=(200, 201)):
    data = None
    headers = api_headers()
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(api_url(path, params), data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            payload = resp.read()
            text = payload.decode("utf-8", "replace") if payload else ""
            parsed = json.loads(text) if text else {}
            if resp.status not in expected:
                raise RuntimeError(f"{method} {path} returned {resp.status}: {text[:1000]}")
            return resp.status, parsed
    except urllib.error.HTTPError as exc:
        text = exc.read().decode("utf-8", "replace")
        raise RuntimeError(f"{method} {path} returned {exc.code}: {text[:1000]}") from exc


def existing_release_commit(tag):
    try:
        _, release = request_json("GET", f"/repos/{OWNER}/{REPO}/releases/{urllib.parse.quote(tag, safe='')}")
    except RuntimeError as exc:
        if " returned 404:" in str(exc):
            return None
        raise
    try:
        commit = release_recorded_commit(release)
    except ValueError as exc:
        raise RuntimeError(f"Release {tag!r}: {exc}") from exc
    if not commit:
        raise RuntimeError(f"Release {tag!r} does not record a commit")
    return commit


def previous_release_tag(current_tag, log):
    current_key = release_tag_key(current_tag)
    if current_key is None:
        return ""
    try:
        tags = check_output(
            ["git", "tag", "--merged", "HEAD"],
            REPO_DIR,
        ).splitlines()
    except subprocess.CalledProcessError as exc:
        log_line(log, f"warning: failed to list previous tags: {exc}")
        return ""
    candidates = []
    for tag in tags:
        tag = tag.strip()
        if not tag or tag == current_tag or not RELEASE_TAG_RE.fullmatch(tag):
            continue
        tag_key = release_tag_key(tag)
        if tag_key < current_key:
            candidates.append((tag_key, tag))
    if not candidates:
        return ""
    return max(candidates)[1]


def git_log_entries(previous_tag, log):
    if not previous_tag:
        output = check_output(
            ["git", "log", "--pretty=format:%h %s", "--no-merges", "-20"],
            REPO_DIR,
        )
        return [line for line in output.splitlines() if line.strip()]

    rev_range = f"{previous_tag}..HEAD"
    try:
        output = check_output(
            ["git", "log", "--pretty=format:%h %s", "--no-merges", rev_range],
            REPO_DIR,
        )
    except subprocess.CalledProcessError as exc:
        log_line(log, f"warning: failed to collect changelog from {rev_range}: {exc}")
        return []
    return [line for line in output.splitlines() if line.strip()]


def categorize_change(line):
    text = line.lower()
    if any(token in text for token in ("特性", "feature", "feat")):
        return "features"
    if any(token in text for token in ("修复", "fix", "bug")):
        return "fixes"
    if any(token in text for token in ("性能", "perf")):
        return "performance"
    return "chores"


def format_section(title, entries):
    lines = [f"## {title}"]
    if entries:
        lines.extend(f"- {entry}" for entry in entries)
    else:
        lines.append("- 暂无")
    return "\n".join(lines)


def parse_wheel_name(name):
    if not name.endswith(".whl"):
        raise RuntimeError(f"not a wheel filename: {name}")
    parts = name[:-4].split("-")
    if len(parts) not in (5, 6):
        raise RuntimeError(f"cannot parse wheel filename: {name}")
    return {
        "distribution": parts[0],
        "version": parts[1],
        "python_tag": parts[-3],
        "abi_tag": parts[-2],
        "platform_tag": parts[-1],
    }


def validate_release_manifest(wheels, log):
    """The release carries the exact expected wheel set or nothing.

    Exactly one cp314 manylinux fat wheel; exactly one cp311 linux wheel
    when CINDERX_RELEASE_CP311 is on (and none when it is off); every wheel
    the same distribution and version; nothing else.
    """
    parsed = [parse_wheel_name(wheel.name) for wheel in wheels]
    for meta in parsed:
        if meta["distribution"] != "cinderx":
            raise RuntimeError(f"unexpected distribution in wheelhouse: {meta}")
    versions = sorted({meta["version"] for meta in parsed})
    if len(versions) != 1:
        raise RuntimeError(f"wheel versions disagree: {versions}")
    cp314 = [meta for meta in parsed if meta["python_tag"] == "cp314"]
    cp311 = [meta for meta in parsed if meta["python_tag"] == "cp311"]
    others = [meta for meta in parsed if meta["python_tag"] not in ("cp311", "cp314")]
    if len(cp314) != 1:
        raise RuntimeError(f"expected exactly one cp314 wheel, found {len(cp314)}")
    if cp314[0]["abi_tag"] != "cp314":
        raise RuntimeError(f"cp314 wheel has unexpected abi: {cp314[0]['abi_tag']}")
    # Every dot-separated platform component must be a manylinux aarch64
    # tag; substring checks would wave fabricated platforms through.
    if not all(
        re.fullmatch(r"manylinux_\d+_\d+_aarch64", component)
        for component in cp314[0]["platform_tag"].split(".")
    ):
        raise RuntimeError(f"cp314 wheel has unexpected platform: {cp314[0]['platform_tag']}")
    if RELEASE_CP311:
        if len(cp311) != 1:
            raise RuntimeError(f"expected exactly one cp311 wheel, found {len(cp311)}")
        if cp311[0]["abi_tag"] != "cp311":
            raise RuntimeError(f"cp311 wheel has unexpected abi: {cp311[0]['abi_tag']}")
        if cp311[0]["platform_tag"] != "linux_aarch64":
            raise RuntimeError(f"cp311 wheel has unexpected platform: {cp311[0]['platform_tag']}")
    elif cp311:
        raise RuntimeError("cp311 wheels present although CINDERX_RELEASE_CP311 is off")
    if others:
        raise RuntimeError(f"unexpected wheels in wheelhouse: {others}")
    log_line(log, f"release manifest ok: {len(parsed)} wheels, version {versions[0]}")


def validate_fat_wheel_content(wheel_path, expected_version, log):
    """Prove the cp314 wheel really is the 3.14.0-3.14.3 fat wheel.

    An ordinary cp314 manylinux wheel carries the same filename tags, so the
    name-level manifest cannot tell them apart; a misconfigured build command
    (the historic default built exactly such a wheel) must not publish one.
    """
    with zipfile.ZipFile(wheel_path) as wheel:
        names = set(wheel.namelist())
        if "_cinderx.py" not in names:
            raise RuntimeError(f"{wheel_path.name} is missing the fat loader _cinderx.py")
        if "cinderx/_native/fat_wheel.json" not in names:
            raise RuntimeError(f"{wheel_path.name} is missing cinderx/_native/fat_wheel.json")
        variants = {
            name.split("/")[2]
            for name in names
            if name.startswith("cinderx/_native/") and name.endswith(".so")
        }
        expected_variants = {"py314_0", "py314_1", "py314_2", "py314_3"}
        if variants != expected_variants:
            raise RuntimeError(
                f"{wheel_path.name} native variants {sorted(variants)} != {sorted(expected_variants)}"
            )
        top_level_native = [
            name
            for name in names
            if "/" not in name and name.startswith("_cinderx") and name.endswith(".so")
        ]
        if top_level_native:
            raise RuntimeError(
                f"{wheel_path.name} carries top-level native extensions: {top_level_native}"
            )
        metadata_name = next(
            (name for name in names if name.endswith(".dist-info/METADATA")), None
        )
        if metadata_name is None:
            raise RuntimeError(f"{wheel_path.name} is missing METADATA")
        metadata = wheel.read(metadata_name).decode("utf-8", "replace")
        version_lines = [
            line for line in metadata.splitlines() if line.startswith("Version:")
        ]
        if [f"Version: {expected_version}"] != version_lines:
            raise RuntimeError(
                f"{wheel_path.name} METADATA version {version_lines} != filename "
                f"version {expected_version}"
            )
    log_line(log, f"fat wheel content ok: {wheel_path.name}")


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_sha256_sidecar(wheel_path, digest):
    sha_path = wheel_path.with_name(f"{wheel_path.name}.sha256")
    sha_path.write_text(f"{digest}  {wheel_path.name}\n", encoding="utf-8")
    return sha_path


def collect_wheel_assets(wheels):
    assets = []
    for wheel in wheels:
        digest = sha256_file(wheel)
        sidecar = write_sha256_sidecar(wheel, digest)
        assets.append(
            {
                "path": wheel,
                "name": wheel.name,
                "sha256": digest,
                "sha256_path": sidecar,
                "sha256_name": sidecar.name,
            }
        )
    return assets


def interpreter_for_python_tag(python_tag):
    if python_tag.startswith("cp3") and python_tag[3:].isdigit():
        return f"python3.{python_tag[3:]}"
    return "python3"


PYTHON_TAG_DESCRIPTIONS = {
    "cp314": "CPython 3.14（manylinux fat wheel，覆盖 3.14.0–3.14.3）",
    "cp311": "CPython 3.11（openEuler 24.03-LTS-SP3 wheel，`cp311` 标签）",
}


def python_support_line(wheel_assets):
    # Derived from the wheels actually being published, so a CP314-only
    # release does not advertise a CPython 3.11 wheel it does not carry.
    tags = sorted(
        {parse_wheel_name(asset["name"])["python_tag"] for asset in wheel_assets},
        reverse=True,
    )
    if not tags:
        return "- Python: 见本 Release 附件"
    return "- Python: " + "；".join(
        PYTHON_TAG_DESCRIPTIONS.get(tag, tag) for tag in tags
    )


def format_integrity_section(wheel_assets):
    lines = [
        "## 完整性校验",
        "请从本 Release 附件下载 `.whl` 和对应 `.sha256` 文件，安装前先校验：",
        "",
        "```bash",
        "sha256sum -c <wheel-file>.sha256",
        "```",
        "",
        "安装时可使用 pip hash-checking mode，让 pip 在安装过程中再次校验 wheel。"
        "按运行环境选择对应的 wheel 与解释器：",
        "",
    ]
    if wheel_assets:
        for asset in wheel_assets:
            interpreter = interpreter_for_python_tag(
                parse_wheel_name(asset["name"])["python_tag"]
            )
            lines.extend(
                [
                    "```bash",
                    f"printf '%s\\n' './{asset['name']} --hash=sha256:{asset['sha256']}' > requirements.txt",
                    f"{interpreter} -m pip install --no-deps --require-hashes -r requirements.txt",
                    "```",
                    "",
                ]
            )
    else:
        lines.extend(
            [
                "```text",
                "./<wheel-file>.whl --hash=sha256:<sha256>",
                "```",
                "",
            ]
        )
    lines.append("Release 附件 SHA256：")
    if wheel_assets:
        for asset in wheel_assets:
            lines.append(
                f"- `{asset['name']}`: `{asset['sha256']}` "
                f"(sidecar: `{asset['sha256_name']}`)"
            )
    else:
        lines.append("- 暂无")
    return "\n".join(lines)


def build_release_body(tag, sha, log, wheel_assets=None):
    wheel_assets = wheel_assets or []
    match = validate_release_tag(tag)
    previous_tag = previous_release_tag(tag, log) if match else ""
    entries = git_log_entries(previous_tag, log)
    grouped = {
        "features": [],
        "fixes": [],
        "performance": [],
        "chores": [],
    }
    for entry in entries:
        grouped[categorize_change(entry)].append(entry)

    today = time.strftime("%Y-%m-%d")
    compare_from = previous_tag or "recent commits"
    body = [
        f"# {tag} ({today})",
        "",
        format_section("新功能 (Features)", grouped["features"]),
        "",
        format_section("修复 (Bug Fixes)", grouped["fixes"]),
        "",
        format_section("性能优化 (Performance)", grouped["performance"]),
        "",
        format_section("其他变更 (Chores)", grouped["chores"]),
        "",
        "## 安装与部署",
        python_support_line(wheel_assets),
        "- Wheel: 请在本 Release 附件中下载与运行环境匹配的 `.whl`",
        "",
        format_integrity_section(wheel_assets),
        "",
        "## 完整变更日志",
        f"Changes since `{compare_from}`:",
    ]
    body.extend(f"- {entry}" for entry in entries)
    if not entries:
        body.append("- 暂无")
    if match and match.group("prerelease"):
        body.extend(
            [
                "",
                "## 预发布说明",
                "This is a pre-release version.",
                "",
                "## 已知问题",
                "- 暂无",
            ]
        )
    body.extend(
        [
            "",
            "## 构建信息",
            f"- Commit: `{sha}`",
        ]
    )
    return "\n".join(body) + "\n"


def create_release(tag, sha, log, wheel_assets=None):
    validate_release_tag(tag)
    body = {
        "tag_name": tag,
        "name": f"CinderX {tag}",
        "body": build_release_body(tag, sha, log, wheel_assets),
        "target_commitish": sha,
        "release_status": RELEASE_STATUS,
    }
    path = f"/repos/{OWNER}/{REPO}/releases"
    try:
        status, _ = request_json("POST", path, body=body)
        log_line(log, f"created release {tag}, status={status}")
    except Exception as exc:
        log_line(log, f"release create failed: {exc}")
        if STRICT_RELEASE_CREATE or existing_release_commit(tag) != sha:
            raise
        log_line(log, f"release {tag} already exists or is readable; continuing to upload assets")


def upload_asset(tag, file_path, log):
    name = file_path.name
    quoted_tag = urllib.parse.quote(tag, safe="")
    status, upload_info = request_json(
        "GET",
        f"/repos/{OWNER}/{REPO}/releases/{quoted_tag}/upload_url",
        params={"file_name": name},
    )
    log_line(log, f"got upload_url for {name}, status={status}")
    upload_url = upload_info["url"]
    validate_upload_url(upload_url)
    headers = upload_info.get("headers") or {}
    data = file_path.read_bytes()
    req = urllib.request.Request(upload_url, data=data, headers=headers, method="PUT")
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            text = resp.read().decode("utf-8", "replace")
            if resp.status < 200 or resp.status >= 300:
                raise RuntimeError(f"PUT returned {resp.status}: {text[:1000]}")
            log_line(log, f"uploaded {name}, status={resp.status}, bytes={len(data)}")
    except urllib.error.HTTPError as exc:
        text = exc.read().decode("utf-8", "replace")
        raise RuntimeError(f"PUT upload for {name} returned {exc.code}: {text[:1000]}") from exc


def build_and_publish(tag, sha, publish=True):
    validate_release_tag(tag)
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    RUN_DIR.mkdir(parents=True, exist_ok=True)
    safe_tag = hashlib.sha256(json.dumps([OWNER, REPO, tag]).encode()).hexdigest()
    log_path = LOG_DIR / f"{safe_tag}-{int(time.time())}.log"
    success_marker = STATE_DIR / f"{safe_tag}.success"
    running_marker = STATE_DIR / f"{safe_tag}.running"
    with log_path.open("a", encoding="utf-8") as log:
        log_line(log, f"job start tag={tag} sha={sha} publish={publish}")
        if publish and not ACCESS_TOKEN:
            raise RuntimeError("GITCODE_ACCESS_TOKEN is empty; fill /etc/cinderx-webhook.env before publishing")
        _, expected_commit = fetch_tag(tag, sha, log)
        if publish:
            released = existing_release_commit(tag)
            if released is not None and released != expected_commit:
                raise RuntimeError(f"Release {tag!r} records {released}; tag points to {expected_commit}")
            if released == expected_commit and success_marker.exists() and (
                success_marker.read_text(encoding="utf-8").strip() == expected_commit
            ):
                log_line(log, f"success marker exists for {tag}; skipping duplicate build")
                return str(log_path)
        running_marker.write_text(str(os.getpid()), encoding="utf-8")
        try:
            checked_sha = checkout_tag(tag, expected_commit, log)
            # One commit epoch drives every builder: both wheels then carry
            # the same date-derived version and deterministic zip metadata
            # (setup.py and the normalize step consume it).
            build_env = {}
            epoch = check_output(["git", "log", "-1", "--format=%ct", "HEAD"], REPO_DIR)
            if epoch:
                build_env["SOURCE_DATE_EPOCH"] = epoch
            wheelhouse = REPO_DIR / "wheelhouse"
            if wheelhouse.exists():
                shutil.rmtree(wheelhouse)
            build_command = BUILD_COMMAND
            default_entry = REPO_DIR / "ci_pipeline" / "build_release_wheels.py"
            if not env("CINDERX_BUILD_COMMAND") and not default_entry.is_file():
                # Tags predating the two-wheel flow do not carry the default
                # entry point; replaying them falls back to the legacy cp314
                # build they shipped with.  An operator-pinned command is
                # always respected as-is.
                build_command = (
                    f"{sys.executable} ci_pipeline/build_cp314_manylinux_fat_wheel.py "
                    f"--output-dir {WORK_BASE / 'build' / 'cp314-fat-wheel'}"
                )
                log_line(log, f"pre-cp311 tag: legacy build command: {build_command}")
            run_cmd(shlex.split(build_command), REPO_DIR, log, build_env)
            # Top up the release wheel set (see RELEASE_CP311): today this
            # builds the cp311 wheel while the operator-pinned command only
            # builds the cp314 fat wheel; once the pinned command moves to
            # ci_pipeline/build_release_wheels.py it becomes a no-op.  Tags
            # predating the cp311 flow carry no top-up script and are
            # published as before.
            topup_script = REPO_DIR / "ci_pipeline" / "build_cp311_wheel.py"
            if RELEASE_CP311 and not topup_script.is_file():
                log_line(log, "cp311 top-up script not in this tag; skipping top-up")
            elif RELEASE_CP311 and not list(wheelhouse.glob("cinderx-*-cp311-*.whl")):
                run_cmd(
                    [
                        sys.executable,
                        "ci_pipeline/build_cp311_wheel.py",
                        "--output-dir", str(wheelhouse),
                    ],
                    REPO_DIR,
                    log,
                    build_env,
                )
            wheels = sorted(wheelhouse.glob("*.whl"))
            if not wheels:
                raise RuntimeError(f"no wheels found in {wheelhouse}")
            if topup_script.is_file():
                validate_release_manifest(wheels, log)
                cp314_wheel = next(
                    wheel
                    for wheel in wheels
                    if parse_wheel_name(wheel.name)["python_tag"] == "cp314"
                )
                validate_fat_wheel_content(
                    cp314_wheel, parse_wheel_name(cp314_wheel.name)["version"], log
                )
            else:
                log_line(log, "pre-cp311 tag: skipping release manifest validation")
            log_line(log, "built wheels: " + ", ".join(w.name for w in wheels))
            wheel_assets = collect_wheel_assets(wheels)
            for asset in wheel_assets:
                log_line(log, f"sha256 {asset['name']} {asset['sha256']}")
            if not publish:
                log_line(log, f"publish skipped (rehearsal); wheels stay in {wheelhouse}")
                return str(log_path)
            fetch_tag(tag, checked_sha, log)
            create_release(tag, checked_sha, log, wheel_assets)
            for asset in wheel_assets:
                upload_asset(tag, asset["path"], log)
                upload_asset(tag, asset["sha256_path"], log)
            success_marker.write_text(checked_sha + "\n", encoding="utf-8")
            log_line(log, f"job success tag={tag}")
            return str(log_path)
        finally:
            running_marker.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description="Build CinderX wheels for a GitCode tag and publish a release.")
    parser.add_argument("--build-tag", required=True, help="tag name to build and publish")
    parser.add_argument("--sha", default="", help="expected commit sha from GitCode webhook")
    parser.add_argument("--repo-dir", help="existing local cinderx git repository")
    parser.add_argument(
        "--skip-publish",
        action="store_true",
        help="rehearsal mode: build and checksum everything, create no release "
        "and upload nothing (also via CINDERX_SKIP_PUBLISH=1)",
    )
    args = parser.parse_args()

    global REPO_DIR
    if args.repo_dir:
        REPO_DIR = Path(args.repo_dir)

    publish = not (args.skip_publish or env_bool("CINDERX_SKIP_PUBLISH", False))
    build_and_publish(args.build_tag, args.sha, publish=publish)


if __name__ == "__main__":
    main()
