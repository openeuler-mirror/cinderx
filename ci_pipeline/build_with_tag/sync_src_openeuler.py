#!/usr/bin/env python3
"""Sync a CinderX release's source tarball into src-openeuler/cinderx.

Three modes:

  sync      (default)  generate the source tarball from the released commit,
                       commit it to the Bot's fork with the GitCode API and
                       open a PR against src-openeuler/cinderx.
  --dry-run            do every computation and check, write nothing.
  --verify-pr N        optional re-check of PR N: rebuild the tarball from the
                       commit named in the PR's spec and compare the
                       uncompressed tar and check the merge into the target.
                       Public PRs need no token.

Only the Bot token is read, from $CINDERX_SYNC_BOT_TOKEN.  The release token
is never needed.  The Bot token is used only for API requests. Git push uses a separate credential.

Exit codes: 0 = done (or nothing to do), 2 = stopped, a human must look,
1 = unexpected error.
"""
import argparse
import tempfile
import gzip
import hashlib
import io
import json
import os
import re
import subprocess
import sys
import tarfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib
from pathlib import Path

if __package__:
    from .gitcode_response import read_error_excerpt, read_json_response_body
    from .release_tag import HASH_PREFIX, PACKAGE_TAG, release_recorded_commit, tag_slug, valid_tag
else:
    from gitcode_response import read_error_excerpt, read_json_response_body
    from release_tag import HASH_PREFIX, PACKAGE_TAG, release_recorded_commit, tag_slug, valid_tag

GIT_HOST = "https://gitcode.com"
API_BASE = os.environ.get("CINDERX_SYNC_API_BASE", "https://api.gitcode.com/api/v5").rstrip("/")
TOKEN_ENV = "CINDERX_SYNC_BOT_TOKEN"

DEFAULT_UPSTREAM = "openeuler/cinderx"
DEFAULT_SRC = "src-openeuler/cinderx"
DEFAULT_FORK = "cinderx-release/cinderx"
DEFAULT_BRANCH = "openEuler-24.03-LTS-Next"

# Environment self-check: v2.2.3 must rebuild byte-for-byte.
SELF_CHECK_TAG = "v2.2.3"
SELF_CHECK_COMMIT = "c17fb3a1028e7dcf070d79eb7bea9c0884816fa4"
SELF_CHECK_VERSION = "2026.9.22.0"
SELF_CHECK_SHA256 = "5281211dc2c7e21904594ddb667854d63fb33547fdae5882e2039813cdd436e1"
SELF_CHECK_TAR_SHA256 = "df9d058c3e59b9f452d9be90237faba9136c8397e251be517fb21c3a9b3f70ca"
# gzip member header written by `git archive --format=tar.gz` since git 2.38:
# deflate, no flags, mtime 0, no extra flags, OS = Unix.
GZIP_HEADER = b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03"

DEPS = ("fmt", "parallel-hashmap", "usdt")
FORMAL_TAG_RE = re.compile(r"^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")
SOURCE_RE = re.compile(rf"^cinderx-({PACKAGE_TAG})\.tar\.gz$")
DEPS_RE = re.compile(rf"^cinderx-deps-({PACKAGE_TAG})\.tar\.xz$")
SPEC = "cinderx.spec"
PIN_RE = re.compile(r"cinderx_declare_dependency\(\s*([\w.-]+)\s+(\S+)\s+([0-9a-f]{40})")
MARKER_RE = re.compile(r"<!--\s*cinderx-sync\s+tag=(\S+)\s+commit=([0-9a-f]{40})\s*-->")
ALLOWED_GIT_CONFIG = {
    "core": {"repositoryformatversion", "filemode", "bare", "logallrefupdates", "autocrlf"},
    'remote "origin"': {"url", "fetch"},
}
SYNC_POLL_SECONDS = 180


class Stop(Exception):
    """A check failed; a human has to look before anything else happens."""


def log(tag, msg):
    print(f"[{tag}] {msg}", flush=True)


# ---------------------------------------------------------------- pure helpers

def tag_key(tag):
    m = FORMAL_TAG_RE.fullmatch(tag)
    if not m:
        raise Stop(f"not a formal release tag: {tag}")
    return tuple(int(x) for x in m.groups())


def version_from_epoch(epoch):
    """Commit time -> package version: UTC date, patch 0, no leading zeros."""
    t = time.gmtime(int(epoch))
    return f"{t.tm_year}.{t.tm_mon}.{t.tm_mday}.0"


def gzip_like_git(tar_data):
    """Compress a tar exactly like `git archive --format=tar.gz` (git >= 2.38).

    git < 2.38 pipes the tar through an external gzip and yields other bytes,
    so the tarball is compressed here and no longer depends on the git version.
    The self-check catches a zlib build that deflates differently.
    """
    deflate = zlib.compressobj(zlib.Z_DEFAULT_COMPRESSION, zlib.DEFLATED, -zlib.MAX_WBITS)
    body = deflate.compress(tar_data) + deflate.flush()
    trailer = (zlib.crc32(tar_data) & 0xFFFFFFFF).to_bytes(4, "little")
    trailer += (len(tar_data) & 0xFFFFFFFF).to_bytes(4, "little")
    return GZIP_HEADER + body + trailer


def parse_pins(cmake_text):
    pins = {name: (url, sha) for name, url, sha in PIN_RE.findall(cmake_text)}
    missing = [d for d in DEPS if d not in pins]
    if missing:
        raise Stop(f"CMakeLists.txt has no pin for: {', '.join(missing)}")
    return {d: pins[d] for d in DEPS}


def parse_spec(text):
    out = {}
    for key in ("source_commit", "source_epoch", "deps_dir"):
        m = re.search(rf"^%global\s+{key}\s+(\S+)\s*$", text, re.M)
        out[key] = m.group(1) if m else None
    for key in ("Version", "Source0", "Source1"):
        m = re.search(rf"^{key}:\s*(\S+)\s*$", text, re.M)
        out[key] = m.group(1) if m else None
    return out


def git_blob_sha(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def parse_git_config(text):
    entries, section = [], None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if line.startswith("["):
            section = line.strip("[]").strip()
            continue
        key, _, value = line.partition("=")
        entries.append((section, key.strip(), value.strip()))
    return entries


def inspect_deps_tarball(data, pins):
    """Check the reused deps tarball against the CMake pins; return its top dir."""
    try:
        tf = tarfile.open(fileobj=io.BytesIO(data), mode="r:xz")
    except tarfile.TarError as exc:
        raise Stop(f"deps tarball is not a readable tar.xz: {exc}") from exc
    with tf:
        members = tf.getmembers()
        tops = {m.name.split("/", 1)[0] for m in members}
        if len(tops) != 1:
            raise Stop(f"deps tarball must have one top-level dir, found {sorted(tops)}")
        top = tops.pop()
        by_name = {}
        for m in members:
            parts = m.name.split("/")
            if m.name.startswith("/") or ".." in parts:
                raise Stop(f"unsafe path in deps tarball: {m.name}")
            if m.issym() or m.islnk():
                target = m.linkname
                if target.startswith("/") or ".." in target.split("/"):
                    raise Stop(f"link escapes the tree in deps tarball: {m.name} -> {target}")
            by_name[m.name.rstrip("/")] = m
        for dep in DEPS:
            url, pin = pins[dep]
            git_dir = f"{top}/{dep}/.git"

            def read(rel):
                member = by_name.get(f"{git_dir}/{rel}")
                if member is None or not member.isfile():
                    raise Stop(f"deps tarball lacks {git_dir}/{rel}")
                return tf.extractfile(member).read().decode("utf-8", "replace")

            head = read("HEAD").strip()
            if head != pin:
                raise Stop(f"{dep}: .git/HEAD {head} != CMakeLists.txt pin {pin}; the deps tarball must be rebuilt")
            for section, key, value in parse_git_config(read("config")):
                if key not in ALLOWED_GIT_CONFIG.get(section, set()):
                    raise Stop(f"{dep}: unexpected .git/config entry [{section}] {key}")
                if section == 'remote "origin"' and key == "url" and value != url:
                    raise Stop(f"{dep}: origin url {value} != CMakeLists.txt {url}")
            hook_prefix = f"{git_dir}/hooks/"
            for name, member in by_name.items():
                if name.startswith(hook_prefix) and member.isfile() and not name.endswith(".sample"):
                    raise Stop(f"{dep}: non-sample hook in deps tarball: {name}")
    return top


def find_one(files, pattern, what):
    hits = sorted(n for n in files if pattern.fullmatch(n))
    if len(hits) != 1:
        raise Stop(f"expected exactly one {what} in the target branch, found {hits}")
    return hits[0]


def plan_file_ops(base, branch, new_src, new_src_blob, new_deps, deps_blob):
    """Return the API operations that turn `branch` into the wanted tree.

    base/branch: {path: blob_sha} of the verified target-branch commit and of
    the current sync branch.  Only the two archive kinds may differ from base,
    plus cinderx.spec (humans push it); anything else stops the run.
    """
    old_src = find_one(base, SOURCE_RE, "source tarball")
    old_deps = find_one(base, DEPS_RE, "deps tarball")
    if old_src == new_src:
        raise Stop(f"{new_src} already exists in the target branch")
    want = dict(base)
    del want[old_src]
    want[new_src] = new_src_blob
    if old_deps != new_deps:
        del want[old_deps]
    want[new_deps] = deps_blob

    unexpected = sorted(
        p for p in set(branch) | set(want)
        if p not in (old_src, new_src, old_deps, new_deps, SPEC) and branch.get(p) != want.get(p)
    )
    if unexpected:
        raise Stop(f"sync branch has unexpected changes: {unexpected}")

    # Each step is skipped when an earlier (failed) run already did it.
    ops = []
    if branch.get(new_src) != new_src_blob:
        ops.append(("update" if new_src in branch else "create", new_src))
    if old_src in branch:
        ops.append(("delete", old_src))
    if branch.get(new_deps) != deps_blob:
        ops.append(("update" if new_deps in branch else "create", new_deps))
    if old_deps != new_deps and old_deps in branch:
        ops.append(("delete", old_deps))
    return ops, old_src, old_deps


def pr_state(prs, head_ref, fork_repo, commit, branch_exists=True):
    """Decide what to do from the PRs whose head is our sync branch.

    A closed PR stops the run while its branch is still there.  Deleting the
    branch as well is how a human asks for a fresh start.
    """
    ours = [p for p in prs
            if (p.get("head") or {}).get("ref") == head_ref
            and (((p.get("head") or {}).get("repo") or {}).get("full_name") or fork_repo) == fork_repo]
    if any(p.get("state") == "merged" or p.get("merged_at") for p in ours):
        return "merged", None
    open_prs = [p for p in ours if p.get("state") == "open"]
    if open_prs:
        pr = open_prs[0]
        m = MARKER_RE.search(pr.get("body") or "")
        if not m or m.group(2) != commit:
            return "open-mismatch", pr
        return "open", pr
    if ours and branch_exists:
        return "closed", ours[0]
    if ours:
        return "restart", ours[0]
    return "none", None


def pr_body(ctx):
    tag, commit, version = ctx["tag"], ctx["commit"], ctx["version"]
    return "\n".join([
        f"自动同步 cinderx {tag} 的源码包。源码包由发布流水线从下面的 commit 直接生成，未经人手。",
        "",
        "## 来源",
        f"- 上游：{GIT_HOST}/{ctx['upstream']}",
        f"- tag：`{tag}`",
        f"- commit：`{commit}`",
        f"- commit 时间戳：`{ctx['epoch']}`",
        "",
        "## 文件",
        f"- `{ctx['new_src']}`：tar.gz sha256 `{ctx['src_sha256']}`，tar sha256 `{ctx['tar_sha256']}`",
        f"- `{ctx['new_deps']}`：依赖包为复用，内容与制品仓现有的 `{ctx['old_deps']}` 相同，"
        "三个库的版本与该 commit 的 CMakeLists.txt 一致",
        f"- 打包环境：{ctx['git_version']}",
        "",
        "## 复现",
        "解压后的 tar 应与 git 直接打出的 tar 逐字节一致（压缩字节随 zlib 版本可能不同）：",
        "```",
        f"git -c tar.umask=0002 archive --format=tar --prefix=cinderx-{version}/ {commit} | sha256sum",
        f"gzip -dc {ctx['new_src']} | sha256sum",
        "```",
        "",
        "## spec 待人工更新（请推送到本 PR 分支）",
        f"- `source_commit`：`{commit}`",
        f"- `source_epoch`：`{ctx['epoch']}`",
        f"- `Version`：`{version}`",
        f"- `Source0`：`{ctx['new_src']}`",
        f"- `Source1`：`{ctx['new_deps']}`",
        "",
        f"<!-- cinderx-sync tag={tag} commit={commit} -->",
    ])


# ---------------------------------------------------------------------- git

def child_env():
    env = {k: v for k, v in os.environ.items() if k not in {TOKEN_ENV, "CINDERX_SYNC_GIT_TOKEN"}}
    env["GIT_TERMINAL_PROMPT"] = "0"
    return env


def git(args, binary=False):
    p = subprocess.run(["git", *args], capture_output=True, env=child_env())
    if p.returncode:
        raise RuntimeError(f"git {' '.join(args)} failed: {p.stderr.decode('utf-8', 'replace')[:2000]}")
    return p.stdout if binary else p.stdout.decode("utf-8", "replace").strip()


class Mirror:
    """A bare local cache of one GitCode repository, read through git only."""

    def __init__(self, work, repo):
        self.repo = repo
        self.url = f"{GIT_HOST}/{repo}.git"
        self.path = Path(work) / (repo.replace("/", "__") + ".git")
        if not (self.path / "HEAD").exists():
            self.path.mkdir(parents=True, exist_ok=True)
            git(["init", "-q", "--bare", str(self.path)])

    def g(self, *args, binary=False):
        return git(["-C", str(self.path), *args], binary=binary)

    def fetch(self, *refspecs):
        self.g("fetch", "-q", "--no-tags", "--force", self.url, *refspecs)

    def remote_tag_commit(self, tag):
        out = git(["ls-remote", self.url, f"refs/tags/{tag}", f"refs/tags/{tag}^{{}}"])
        refs = dict(reversed(line.split("\t")) for line in out.splitlines() if line)
        return refs.get(f"refs/tags/{tag}^{{}}") or refs.get(f"refs/tags/{tag}")

    def tag_for_slug(self, slug):
        refs = git(["ls-remote", "--tags", "--refs", self.url])
        for line in refs.splitlines():
            tag = line.split("\t", 1)[1].removeprefix("refs/tags/")
            if tag_slug(tag) == slug:
                return tag
        return None

    def includes_commit(self, commit, ancestor):
        result = subprocess.run(
            ["git", "-C", str(self.path), "merge-base", "--is-ancestor", ancestor, commit],
            env=child_env(), capture_output=True,
        )
        return result.returncode == 0

    def remote_branch(self, branch):
        out = git(["ls-remote", self.url, f"refs/heads/{branch}"])
        refs = dict(reversed(line.split("\t", 1)) for line in out.splitlines())
        return refs.get(f"refs/heads/{branch}")

    def merge_tree(self, target, head):
        # A temporary worktree supports the same merge checks on Git 2.33.
        with tempfile.TemporaryDirectory() as directory:
            worktree = str(Path(directory) / "merge")
            self.g("worktree", "add", "--quiet", "--detach", worktree, target)
            try:
                result = subprocess.run(
                    ["git", "-C", worktree, "-c", "user.name=CinderX PR verification",
                     "-c", "user.email=verify@example.invalid", "-c", "core.hooksPath=/dev/null",
                     "-c", "rerere.enabled=false", "merge", "--no-commit", "--no-ff", head],
                    env=child_env(), capture_output=True, encoding="utf-8", errors="replace",
                )
                if result.returncode:
                    raise Stop("PR 与最新目标分支无法试合并：\n"
                               + (result.stdout + result.stderr).strip())
                return git(["-C", worktree, "write-tree"])
            finally:
                self.g("worktree", "remove", "--force", worktree)

    def tree(self, rev):
        out = self.g("ls-tree", "-r", rev)
        files = {}
        for line in out.splitlines():
            meta, path = line.split("\t", 1)
            files[path] = meta.split()[2]
        return files

    def blob(self, rev, path):
        return self.g("cat-file", "blob", f"{rev}:{path}", binary=True)

    def archive_tar(self, commit, version):
        return self.g("-c", "tar.umask=0002", "archive", "--format=tar",
                      f"--prefix=cinderx-{version}/", commit, binary=True)

    def archive(self, commit, version):
        """The source tarball: (tar.gz bytes, tar bytes)."""
        tar_data = self.archive_tar(commit, version)
        return gzip_like_git(tar_data), tar_data


def self_check(upstream):
    """The tar must rebuild byte-for-byte; the gzip bytes may differ with zlib."""
    env = f"{git(['--version'])}, zlib {zlib.ZLIB_RUNTIME_VERSION}"
    upstream.fetch(f"+refs/tags/{SELF_CHECK_TAG}:refs/tags/{SELF_CHECK_TAG}")
    gz_data, tar_data = upstream.archive(SELF_CHECK_COMMIT, SELF_CHECK_VERSION)
    got_tar, got_gz = sha256(tar_data), sha256(gz_data)
    if got_tar != SELF_CHECK_TAR_SHA256:
        raise Stop(f"self-check failed with {env}: {SELF_CHECK_TAG} tar rebuilt to {got_tar}, "
                   f"expected {SELF_CHECK_TAR_SHA256}")
    gz_note = "tar.gz 也一致" if got_gz == SELF_CHECK_SHA256 else "tar.gz 字节与制品仓不同（压缩库差异，不影响内容）"
    log("自检", f"{env}；重打 {SELF_CHECK_TAG} tar → {got_tar[:8]}…{got_tar[-8:]}  与基准一致；{gz_note}")
    return env.replace("git version ", "git ")


def gunzip(data):
    """Decompress a tar.gz from the package repository; b"" if it is not gzip."""
    try:
        return gzip.decompress(data)
    except (OSError, EOFError, zlib.error):
        return b""


# ---------------------------------------------------------------------- API

class Api:
    def __init__(self, token):
        self.token = token

    def call(self, method, path, params=None, body=None, ok=(200, 201, 204), allow=()):
        url = f"{API_BASE}{path}"
        if params:
            url += "?" + urllib.parse.urlencode(params)
        headers = {"Accept": "application/json"}
        if self.token:
            headers["PRIVATE-TOKEN"] = self.token
        data = None
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(url, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=120) as resp:
                status = resp.status
                if status in allow:
                    return status, {}
                if status not in ok:
                    text = read_error_excerpt(resp, 500)
                    raise RuntimeError(f"{method} {path} returned {status}: {text}")
                text = read_json_response_body(resp, f"{method} {path} returned {status}")
        except urllib.error.HTTPError as exc:
            with exc:
                if exc.code in allow:
                    return exc.code, {}
                text = read_error_excerpt(exc, 500)
            raise RuntimeError(f"{method} {path} returned {exc.code}: {text}") from exc
        try:
            return status, (json.loads(text) if text else {})
        except json.JSONDecodeError:
            return status, {"raw": text[:500]}

    def release_commit(self, repo, tag):
        status, rel = self.call("GET", f"/repos/{repo}/releases/{urllib.parse.quote(tag, safe='')}", allow=(404,))
        if status == 404:
            return None
        try:
            return release_recorded_commit(rel)
        except ValueError as exc:
            raise Stop(f"Release {tag!r}: {exc}") from exc

    def pulls(self, repo):
        out = []
        for page in range(1, 21):
            _, items = self.call("GET", f"/repos/{repo}/pulls",
                                 params={"state": "all", "per_page": 100, "page": page})
            if not items:
                break
            out.extend(items)
        return out


# ------------------------------------------------------------------- stages

def check_upgrade(upstream, spec, current_slug, tag, commit):
    if current_slug and FORMAL_TAG_RE.fullmatch(current_slug) and FORMAL_TAG_RE.fullmatch(tag):
        if tag_key(current_slug) > tag_key(tag):
            raise Stop(f"target branch is already at {current_slug}; refusing to go back to {tag}")
        return
    previous = spec.get("source_commit") or ""
    if not re.fullmatch(r"[0-9a-f]{40}", previous):
        raise Stop("target spec needs a full source_commit to compare custom tags")
    if not upstream.includes_commit(commit, previous):
        raise Stop(f"target source commit {previous} is not an ancestor of {commit}")


def prepare(args, upstream, src, api):
    """Everything that only reads: shared by sync and dry-run."""
    tag = args.tag
    if not valid_tag(tag):
        raise Stop(f"invalid Git tag: {tag!r}")
    slug = tag_slug(tag)
    git_version = self_check(Mirror(args.work_dir, DEFAULT_UPSTREAM))

    tag_commit = upstream.remote_tag_commit(tag)
    if not tag_commit:
        raise Stop(f"tag {tag} does not exist in {upstream.repo}")
    release_commit = api.release_commit(upstream.repo, tag) if api.token else None
    commit = args.commit or release_commit
    if args.require_release and not release_commit:
        raise Stop(f"Release {tag!r} must record the built commit")
    if args.commit and release_commit and args.commit != release_commit:
        raise Stop(f"--commit {args.commit} != Release commit {release_commit}")
    if not commit:
        if not args.dry_run:
            raise Stop(f"cannot read the commit recorded by Release {tag}; pass --commit")
        log("提示", "演练且没有 token：commit 取 tag 当前指向")
        commit = tag_commit
    if commit != tag_commit:
        raise Stop(f"tag {tag} now points to {tag_commit}, Release recorded {commit}: the tag was re-cut")
    upstream.fetch(f"+refs/tags/{tag}:refs/tags/{tag}")
    if upstream.g("rev-parse", f"refs/tags/{tag}^{{commit}}") != commit:
        raise Stop(f"tag {tag!r} moved during fetch")
    epoch = int(upstream.g("log", "-1", "--format=%ct", commit))
    version = version_from_epoch(epoch)
    log("版本", f"commit {commit[:12]}… 提交时间 {epoch} → {version}")

    src_data, tar_data = upstream.archive(commit, version)
    new_src = f"cinderx-{slug}.tar.gz"
    log("源码包", f"git archive → {new_src}  sha256 {sha256(src_data)}")

    base = src.remote_branch(args.branch)
    if not base:
        raise Stop(f"{src.repo} has no branch {args.branch}")
    src.fetch(f"+refs/heads/{args.branch}:refs/heads/{args.branch}")
    base_files = src.tree(base)
    spec = parse_spec(src.blob(base, SPEC).decode("utf-8", "replace"))
    cur_src = spec.get("Source0") or ""
    m = SOURCE_RE.fullmatch(cur_src)
    if m and m.group(1) == slug:
        if spec.get("source_commit") != commit:
            raise Stop(f"target spec records a different commit for {tag!r}")
        log("完成", f"目标分支已经是 {tag}（已同步并合入），无事可做")
        return None
    check_upgrade(upstream, spec, m.group(1) if m else None, tag, commit)

    pins = parse_pins(upstream.g("show", f"{commit}:CMakeLists.txt"))
    old_deps = find_one(base_files, DEPS_RE, "deps tarball")
    deps_data = src.blob(base, old_deps)
    top = inspect_deps_tarball(deps_data, pins)
    if spec.get("deps_dir") and spec["deps_dir"] != top:
        raise Stop(f"deps tarball top dir {top} != spec deps_dir {spec['deps_dir']}")
    log("依赖包", " / ".join(f"{d} {pins[d][1][:7]}" for d in DEPS) + "  与 CMakeLists.txt 一致")

    return {
        "tag": tag, "slug": slug, "commit": commit, "epoch": epoch, "version": version,
        "git_version": git_version, "upstream": upstream.repo,
        "new_src": new_src, "src_data": src_data, "src_blob": git_blob_sha(src_data),
        "src_sha256": sha256(src_data), "tar_sha256": sha256(tar_data),
        "base": base, "base_files": base_files,
        "old_deps": old_deps, "new_deps": f"cinderx-deps-{slug}.tar.xz",
        "deps_data": deps_data, "deps_blob": git_blob_sha(deps_data),
    }


def sync_fork(api, fork, src_base, branch, dry_run):
    fork_sha = fork.remote_branch(branch)
    if fork_sha == src_base:
        log("fork", f"{branch} 与制品仓一致（{src_base[:12]}）")
        return
    if dry_run:
        log("fork", f"落后（fork {str(fork_sha)[:12]} / 制品仓 {src_base[:12]}）；正式运行时会调 sync_repo 追平")
        return
    owner_repo = fork.repo
    api.call("PUT", f"/repos/{owner_repo}/sync_repo", body={"branch": branch})
    deadline = time.time() + SYNC_POLL_SECONDS
    while time.time() < deadline:
        time.sleep(5)
        if fork.remote_branch(branch) == src_base:
            log("fork", f"已调 sync_repo 追平到 {src_base[:12]}")
            return
    raise Stop(f"fork {branch} did not reach {src_base} within {SYNC_POLL_SECONDS}s after sync_repo")


# This function is embedded into the existing synchronization module.
def push_artifact_commit(src, fork, ctx, ops, head_ref):
    username = os.environ.get("CINDERX_SYNC_GIT_USERNAME", "")
    password = os.environ.get("CINDERX_SYNC_GIT_TOKEN", "")
    name = os.environ.get("CINDERX_SYNC_GIT_AUTHOR_NAME", "")
    email = os.environ.get("CINDERX_SYNC_GIT_AUTHOR_EMAIL", "")
    if not all((username, password, name, email)):
        raise Stop("missing Git push credential or commit identity for the artifact fork")
    if any(c in name + email for c in "\r\n<>"):
        raise Stop("invalid Git commit identity")
    ref = f"refs/heads/{head_ref}"
    data = {ctx["new_src"]: ctx["src_data"], ctx["new_deps"]: ctx["deps_data"]}
    env = child_env()
    env.pop("CINDERX_SYNC_GIT_TOKEN", None)
    env.update(GIT_AUTHOR_NAME=name, GIT_AUTHOR_EMAIL=email,
               GIT_COMMITTER_NAME=name, GIT_COMMITTER_EMAIL=email)

    def run(*args, input_data=None, push_env=None):
        result = subprocess.run(
            ["git", "-C", str(src.path), *args], input=input_data,
            capture_output=True, env=env if push_env is None else push_env, timeout=180,
        )
        if result.returncode:
            detail = result.stderr.decode("utf-8", "replace")[:1000] if push_env is None else ""
            raise Stop(f"git {args[0]} failed (exit {result.returncode}) {detail}")
        return result.stdout.decode("utf-8").strip()

    with tempfile.TemporaryDirectory(prefix="cinderx-commit-") as tmp:
        env["GIT_INDEX_FILE"] = str(Path(tmp) / "index")
        run("read-tree", ctx["base"])
        for op, path in ops:
            if op == "delete":
                # A mode-zero record removes the path without consulting a work tree.
                record = f"0 {'0' * len(ctx['base'])}\t{path}\0".encode()
                run("update-index", "-z", "--index-info", input_data=record)
            else:
                blob = run("hash-object", "-w", "--stdin", input_data=data[path])
                run("update-index", "--add", "--cacheinfo", f"100644,{blob},{path}")
        tree = run("write-tree")
        msg = f"Update CinderX to {ctx['tag']}\n\nSigned-off-by: {name} <{email}>\n"
        commit = run("commit-tree", tree, "-p", ctx["base"], input_data=msg.encode())
        askpass = Path(tmp) / "askpass"
        askpass.write_bytes(b'#!/bin/sh\ncase "$1" in *Username*) printf "%s\\n" "$SYNC_PUSH_USER";; *) printf "%s\\n" "$SYNC_PUSH_SECRET";; esac\n')
        askpass.chmod(0o700)
        push_env = {k: v for k, v in env.items() if not k.startswith("GIT_TRACE") and k != "GIT_CURL_VERBOSE"}
        push_env.update(GIT_ASKPASS=str(askpass), SYNC_PUSH_USER=username,
                        SYNC_PUSH_SECRET=password, LC_ALL="C")
        run("-c", "credential.helper=", "-c", "http.followRedirects=false",
            "-c", "core.hooksPath=/dev/null", "push", "--porcelain",
            f"--force-with-lease={ref}:", fork.url, f"{commit}:{ref}", push_env=push_env)
    if fork.remote_branch(head_ref) != commit:
        raise Stop(f"cannot verify the new fork branch {head_ref} at {commit}")
    log("提交", f"{head_ref}: {commit}（单条制品提交）")


def run_sync(args):
    token = os.environ.get(TOKEN_ENV, "")
    if not token and not args.dry_run:
        raise Stop(f"${TOKEN_ENV} is empty")
    api = Api(token)
    work = Path(args.work_dir)
    upstream, src, fork = Mirror(work, args.upstream), Mirror(work, args.src), Mirror(work, args.fork)
    ctx = prepare(args, upstream, src, api)
    if ctx is None:
        return 0
    head_ref = f"sync-one/{ctx['slug']}"
    if token:
        prs = [p for p in api.pulls(args.src) if (p.get("base") or {}).get("ref") == args.branch]
        legacy, _ = pr_state(prs, f"sync/{ctx['slug']}", args.fork, ctx["commit"])
        if legacy in {"open", "open-mismatch", "merged"}:
            raise Stop("a legacy PR for this tag is still open or was merged")

    state, pr = ("none", None)
    if token:
        state, pr = pr_state(prs, head_ref, args.fork, ctx["commit"],
                             branch_exists=fork.remote_branch(head_ref) is not None)
    else:
        log("提示", "演练且没有 token：不查已有 PR")
    log("状态", f"{head_ref}: {state}" + (f"（PR !{pr.get('number')}）" if pr else ""))
    if state == "merged":
        log("完成", f"{ctx['tag']} 的 PR 已合入，无事可做")
        return 0
    if state == "closed":
        raise Stop(f"PR !{pr.get('number')} for {ctx['tag']} was closed without merging; not re-opening "
                   f"(delete {args.fork}:{head_ref} as well to start over)")
    if state == "restart":
        log("状态", f"PR !{pr.get('number')} 已关闭且 {head_ref} 分支已删除，重新开始")
    if state == "open-mismatch":
        raise Stop(f"open PR !{pr.get('number')} was made for another commit: the tag was re-cut")

    sync_fork(api, fork, ctx["base"], args.branch, args.dry_run)

    branch_sha = fork.remote_branch(head_ref)
    plan_base = ctx["base_files"]
    if branch_sha:
        fork.fetch(f"+refs/heads/{head_ref}:refs/heads/{head_ref}")
        branch_files = fork.tree(branch_sha)
        # Compare against where the branch started, so later merges into the
        # target branch do not look like unexpected changes on our branch.
        if not args.dry_run:
            fork.fetch(f"+refs/heads/{args.branch}:refs/heads/{args.branch}")
            plan_base = fork.tree(fork.g("merge-base", branch_sha, f"refs/heads/{args.branch}"))
    else:
        branch_files = dict(ctx["base_files"])
    ops, old_src, old_deps = plan_file_ops(plan_base, branch_files, ctx["new_src"],
                                           ctx["src_blob"], ctx["new_deps"], ctx["deps_blob"])
    for op, name in ops:
        log("提交", f"{'+' if op != 'delete' else '-'} {name}" + ("（内容与旧文件相同，只是改名）" if name == ctx["new_deps"] else ""))
    body = pr_body({**ctx, "old_deps": old_deps})

    if args.dry_run:
        log("演练", f"不写入任何东西。PR 将从 {args.fork}:{head_ref} 提到 {args.src}:{args.branch}，描述如下：")
        print(body)
        return 0

    if branch_sha:
        if ops or fork.g("rev-list", "--count", f"{ctx['base']}..{branch_sha}") != "1":
            raise Stop(f"{head_ref} does not contain exactly one complete artifact commit")
    else:
        if state == "open":
            raise Stop("the open PR source branch is missing")
        push_artifact_commit(src, fork, ctx, ops, head_ref)

    if state == "open":
        log("完成", f"PR !{pr.get('number')} 已存在，保留源分支")
        return 0
    _, created = api.call("POST", f"/repos/{args.src}/pulls", body={
        "title": f"【自动同步】cinderx {ctx['tag']} 源码包",
        "head": f"{args.fork.split('/')[0]}:{head_ref}",
        "fork_path": args.fork,
        "base": args.branch,
        "body": body,
    })
    number = created.get("number") or created.get("iid")
    log("PR", f"已向 {args.src} 的 {args.branch} 提 PR !{number}")
    if args.notify and number:
        mentions = " ".join(f"@{u}" for u in args.notify)
        api.call("POST", f"/repos/{args.src}/pulls/{number}/comments",
                 body={"body": f"{mentions} 请按 PR 描述更新 cinderx.spec，并入 {args.fork} 的 {head_ref} 分支上已有的提交。"})
        log("通知", f"已 @ {mentions}")
    return 0


def verification_source(api, args, head):
    try:
        _, pr = api.call("GET", f"/repos/{args.src}/pulls/{args.verify_pr}")
    except (RuntimeError, OSError) as exc:
        raise Stop(f"cannot read PR !{args.verify_pr} metadata: {exc}") from exc
    source = pr.get("head") or {}
    target = pr.get("base") or {}
    if pr.get("state") != "open" or (target.get("repo") or {}).get("full_name") != args.src or target.get("ref") != args.branch:
        raise Stop(f"PR !{args.verify_pr} is not open against {args.src}:{args.branch}")
    repo = (source.get("repo") or {}).get("full_name") or ""
    branch = source.get("ref") or ""
    if not re.fullmatch(r"[A-Za-z0-9_-][A-Za-z0-9_.-]*/[A-Za-z0-9_-][A-Za-z0-9_.-]*", repo) or not branch:
        raise Stop(f"PR !{args.verify_pr} does not identify a source repository and branch")
    if args.head_branch and args.head_branch != branch:
        raise Stop(f"--head-branch {args.head_branch!r} differs from the PR source branch {branch!r}")
    if source.get("sha") != head:
        raise Stop("PR metadata and Git head disagree; wait for the PR to update and verify again")
    return repo, branch


def cross_check_source_branch(fork, branch, head):
    tip = fork.remote_branch(branch)
    if tip is None:
        raise Stop(f"PR source branch {fork.repo}:{branch} is missing")
    if tip != head:
        raise Stop(f"PR head {head[:12]} differs from {fork.repo}:{branch} at {tip[:12]}; "
                   "wait for the PR to update and verify again")


def verify_target_branch_state(src, upstream, target, head, tag, commit, spec):
    target_spec = parse_spec(src.blob(target, SPEC).decode("utf-8", "replace"))
    target_src = SOURCE_RE.fullmatch(target_spec.get("Source0") or "")
    if not target_src:
        raise Stop("目标分支的 spec 缺少有效的 Source0，无法判断是否回退")
    check_upgrade(upstream, target_spec, target_src.group(1), tag, commit)
    target_version = target_spec.get("Version") or ""
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){3}", target_version):
        raise Stop("目标分支的 spec 缺少有效的 RPM Version，无法判断是否回退")
    if tuple(map(int, target_version.split("."))) > tuple(map(int, spec["Version"].split("."))):
        raise Stop(f"合入会使 RPM Version 从 {target_version} 回退到 {spec['Version']}")

    merged = src.merge_tree(target, head)
    merged_spec = parse_spec(src.blob(merged, SPEC).decode("utf-8", "replace"))
    for key in ("Source0", "Source1", "Version", "source_commit", "source_epoch", "deps_dir"):
        if merged_spec.get(key) != spec.get(key):
            raise Stop(f"与目标分支合并后 spec 的 {key} 与已复核的 PR 不一致")
    merged_files = src.tree(merged)
    head_files = src.tree(head)
    for key, pattern, label in (("Source0", SOURCE_RE, "源码包"), ("Source1", DEPS_RE, "依赖包")):
        name = find_one(merged_files, pattern, label)
        if name != spec[key] or merged_files[name] != head_files[spec[key]]:
            raise Stop(f"与目标分支合并后的{label}与已复核的 PR 不一致")
    target_files = src.tree(target)
    target_deps = find_one(target_files, DEPS_RE, "目标分支依赖包")
    if target_files[target_deps] != merged_files[spec["Source1"]]:
        raise Stop("合入会替换目标分支现有的依赖包内容")


def run_verify(args):
    work = Path(args.work_dir)
    upstream, src = Mirror(work, args.upstream), Mirror(work, args.src)
    self_check(Mirror(work, DEFAULT_UPSTREAM))
    n = args.verify_pr
    pr_ref = f"refs/pr/{n}"
    try:
        src.fetch(f"+refs/merge-requests/{n}/head:{pr_ref}")
    except RuntimeError as exc:
        raise Stop(f"cannot fetch PR !{n} head from {src.repo}: {exc}") from exc
    src.fetch(f"+refs/heads/{args.branch}:refs/heads/{args.branch}")
    head = src.g("rev-parse", pr_ref)
    api = Api(os.environ.get(TOKEN_ENV, ""))
    source_repo, source_branch = verification_source(api, args, head)
    fork = Mirror(work, source_repo)
    cross_check_source_branch(fork, source_branch, head)
    target = src.g("rev-parse", f"refs/heads/{args.branch}")
    base = src.g("merge-base", target, head)
    log("复核", f"PR !{n} 最新 commit {head}（请与 PR 页面上最新的 commit 核对），基于 {base[:12]}")

    failures = []
    changes = [line.split("\t") for line in src.g("diff", "--no-renames", "--name-status", base, head).splitlines() if line]
    spec = parse_spec(src.blob(head, SPEC).decode("utf-8", "replace"))
    src_name, deps_name = spec.get("Source0") or "", spec.get("Source1") or ""
    base_files = src.tree(base)
    allowed = {SPEC, src_name, deps_name}
    allowed |= {p for p in base_files if SOURCE_RE.fullmatch(p) or DEPS_RE.fullmatch(p)}
    extra = sorted(path for _, path in changes if path not in allowed)
    (failures.append(f"除 spec 和两个压缩包外还改了：{extra}") if extra
     else log("复核", "其他文件：除 cinderx.spec 和压缩包外没有改动  通过"))

    m = SOURCE_RE.fullmatch(src_name)
    commit = spec.get("source_commit") or ""
    slug = m.group(1) if m else None
    tag = args.tag or (slug if slug and not slug.startswith(HASH_PREFIX) else None)
    if not tag and slug:
        tag = upstream.tag_for_slug(slug)
    if not tag or not valid_tag(tag) or tag_slug(tag) != slug:
        failures.append(f"无法核对 Source0 {src_name!r} 对应的 tag，可通过 --tag 指定原始 tag")
    else:
        if deps_name != f"cinderx-deps-{slug}.tar.xz":
            failures.append(f"Source1 does not match the tag: {deps_name!r}")
        tag_commit = upstream.remote_tag_commit(tag)
        if tag_commit != commit:
            failures.append(f"spec source_commit {commit} != {tag} 当前指向 {tag_commit}")
        else:
            upstream.fetch(f"+refs/tags/{tag}:refs/tags/{tag}")
            if upstream.g("rev-parse", f"refs/tags/{tag}^{{commit}}") != commit:
                raise Stop(f"tag {tag!r} moved during verification")
            epoch = upstream.g("log", "-1", "--format=%ct", commit)
            version = version_from_epoch(epoch)
            if spec.get("source_epoch") != epoch or spec.get("Version") != version:
                failures.append(f"spec source_epoch/Version {spec.get('source_epoch')}/{spec.get('Version')} "
                                f"!= {epoch}/{version}")
            rebuilt_gz, rebuilt = upstream.archive(commit, version)
            try:
                in_pr_gz = src.blob(head, src_name)
            except RuntimeError:
                in_pr_gz = b""
            in_pr = gunzip(in_pr_gz)
            if in_pr == rebuilt:
                same_gz = "，压缩包也逐字节一致" if in_pr_gz == rebuilt_gz else ""
                log("复核", f"源码包：解压后的 tar 与 {commit[:12]}… 重新打包的结果逐字节一致{same_gz}  通过")
            else:
                failures.append(f"源码包 {src_name} 解压后与从 {commit[:12]} 重新打包的 tar 不一致 "
                                f"（PR tar {sha256(in_pr)[:12]} / 重打 tar {sha256(rebuilt)[:12]}）")
    try:
        old_deps = find_one(base_files, DEPS_RE, "deps tarball")
        if src.tree(head).get(deps_name) == base_files[old_deps]:
            log("复核", "依赖包：与制品仓现有文件内容相同  通过")
        else:
            failures.append(f"依赖包 {deps_name} 与制品仓现有的 {old_deps} 内容不同")
    except Stop as exc:
        failures.append(str(exc))

    if failures:
        for f in failures:
            log("复核", "不通过：" + f)
        raise Stop(f"PR !{n} 复核不通过，不要合入")
    verify_target_branch_state(src, upstream, target, head, tag, commit, spec)
    cross_check_source_branch(fork, source_branch, head)
    if src.remote_branch(args.branch) != target:
        raise Stop("复核期间目标分支已更新，请重新复核")
    log("复核", "结论：可以合入")
    return 0


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--tag", help="release tag to sync, e.g. v2.2.4")
    p.add_argument("--commit", help="explicit release commit for a manual recovery")
    p.add_argument("--require-release", action="store_true", help="require Release commit even on dry-run")
    p.add_argument("--dry-run", action="store_true", help="compute and check everything, write nothing")
    p.add_argument("--verify-pr", type=int, metavar="N", help="optional: re-check src-openeuler PR N by rebuilding its tarball")
    p.add_argument("--upstream", default=DEFAULT_UPSTREAM)
    p.add_argument("--src", default=DEFAULT_SRC)
    p.add_argument("--fork", default=DEFAULT_FORK)
    p.add_argument("--branch", default=DEFAULT_BRANCH)
    p.add_argument("--head-branch", help="--verify-pr: assert the source branch reported by GitCode")
    p.add_argument("--notify", nargs="*", default=[], help="GitCode logins to @ in the PR")
    p.add_argument("--work-dir", default=os.environ.get("CINDERX_SYNC_WORK_DIR",
                                                        str(Path.home() / ".cache" / "cinderx-sync")))
    args = p.parse_args(argv)
    try:
        if args.verify_pr:
            return run_verify(args)
        if not args.tag:
            p.error("--tag is required unless --verify-pr is given")
        return run_sync(args)
    except Stop as exc:
        log("停止", f"{exc}（需要人处理）")
        return 2


if __name__ == "__main__":
    sys.exit(main())
