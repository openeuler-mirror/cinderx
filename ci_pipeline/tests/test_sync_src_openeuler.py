import gzip
import io
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import tarfile
from types import SimpleNamespace
import unittest
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from ci_pipeline.build_with_tag import sync_src_openeuler as s  # noqa: E402

PINS = {
    "fmt": ("https://github.com/fmtlib/fmt", "40626af88bd7df9a5fb80be7b25ac85b122d6c21"),
    "parallel-hashmap": ("https://github.com/greg7mdp/parallel-hashmap", "896f1a03e429c45d9fe9638e892fc1da73befadd"),
    "usdt": ("https://github.com/libbpf/usdt", "f4ea2f524efa80d062f4d586d78daafb83dc7d24"),
}
CONFIG = ("[core]\n\trepositoryformatversion = 0\n\tfilemode = true\n\tbare = false\n"
          "\tlogallrefupdates = true\n\tautocrlf = false\n[remote \"origin\"]\n\turl = {url}\n"
          "\tfetch = +refs/heads/*:refs/remotes/origin/*\n")


def deps_tar(top="cinderx-deps-ba5d5c5756f6", head=None, config_extra="", extra=()):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:xz") as tf:
        def add(name, data=b"", kind=tarfile.REGTYPE, link=""):
            ti = tarfile.TarInfo(name)
            ti.type, ti.linkname, ti.size = kind, link, len(data)
            tf.addfile(ti, io.BytesIO(data) if kind == tarfile.REGTYPE else None)
        for dep, (url, pin) in PINS.items():
            add(f"{top}/{dep}/.git/HEAD", ((head or {}).get(dep, pin) + "\n").encode())
            add(f"{top}/{dep}/.git/config", (CONFIG.format(url=url) + config_extra).encode())
            add(f"{top}/{dep}/.git/hooks/pre-commit.sample", b"#!/bin/sh\n")
            add(f"{top}/{dep}/README.md", b"x")
        for args in extra:
            add(*args)
    return buf.getvalue()


class PureHelpers(unittest.TestCase):
    def test_custom_tag_names_match_package_patterns_without_collisions(self):
        tags = ("v3.0.0.test", "2026-11-13", "v3.0.0-rc.1", "release/2026", "release_2026", "发版", "release`x")
        slugs = []
        for tag in tags:
            self.assertTrue(s.valid_tag(tag))
            slug = s.tag_slug(tag)
            slugs.append(slug)
            self.assertIsNotNone(s.SOURCE_RE.fullmatch(f"cinderx-{slug}.tar.gz"))
            self.assertIsNotNone(s.DEPS_RE.fullmatch(f"cinderx-deps-{slug}.tar.xz"))
        self.assertEqual(slugs[:3], list(tags[:3]))
        self.assertEqual(len(set(slugs)), len(tags))
        self.assertNotEqual(s.tag_slug(slugs[3]), slugs[3])
        self.assertFalse(s.valid_tag("release/../2026"))

    def test_custom_tag_upgrade_uses_source_history(self):
        upstream = mock.Mock()
        old, new = "a" * 40, "b" * 40
        spec = {"source_commit": old}
        upstream.includes_commit.return_value = True
        s.check_upgrade(upstream, spec, "v3.0.0", "release/drill", new)
        upstream.includes_commit.assert_called_once_with(new, old)
        upstream.includes_commit.return_value = False
        with self.assertRaisesRegex(s.Stop, "not an ancestor"):
            s.check_upgrade(upstream, spec, "v3.0.0", "release/drill", new)
        with self.assertRaisesRegex(s.Stop, "refusing to go back"):
            s.check_upgrade(upstream, spec, "v3.0.0", "v2.9.0", new)

    def test_version_from_epoch_is_utc_without_leading_zeros(self):
        self.assertEqual(s.version_from_epoch(1790063199), "2026.9.22.0")
        self.assertEqual(s.version_from_epoch(1767225600), "2026.1.1.0")  # 2026-01-01T00:00Z

    def test_tag_key_rejects_prereleases(self):
        self.assertEqual(s.tag_key("v2.10.3"), (2, 10, 3))
        self.assertGreater(s.tag_key("v2.10.0"), s.tag_key("v2.9.9"))
        for bad in ("v2.2.3-rc.1", "2.2.3", "v02.2.3"):
            with self.assertRaises(s.Stop):
                s.tag_key(bad)

    def test_gzip_like_git_has_fixed_header_and_round_trips(self):
        data = b"cinderx" * 5000
        packed = s.gzip_like_git(data)
        # No mtime, no file name, OS = Unix: nothing that varies between hosts.
        self.assertEqual(packed[:10], bytes.fromhex("1f8b0800000000000003"))
        self.assertEqual(gzip.decompress(packed), data)
        self.assertEqual(packed, s.gzip_like_git(data))
        self.assertEqual(gzip.decompress(s.gzip_like_git(b"")), b"")
        # verify compares the decompressed tar, whatever zlib compressed it.
        self.assertEqual(s.gunzip(gzip.compress(data, compresslevel=9, mtime=123)), data)
        self.assertEqual(s.gunzip(b"not gzip"), b"")

    def test_parse_pins(self):
        text = "\n".join(
            f"cinderx_declare_dependency(\n  {d}\n  {u}\n  {p} # note\n)" for d, (u, p) in PINS.items())
        self.assertEqual(s.parse_pins(text), PINS)
        with self.assertRaises(s.Stop):
            s.parse_pins("nothing here")

    def test_parse_spec(self):
        spec = ("%global source_commit c17fb3a1028e7dcf070d79eb7bea9c0884816fa4\n"
                "%global source_epoch 1790063199\n%global deps_dir cinderx-deps-ba5d5c5756f6\n"
                "Version:        2026.9.22.0\nSource0:        cinderx-v2.2.3.tar.gz\n"
                "Source1:        cinderx-deps-v2.2.3.tar.xz\n")
        got = s.parse_spec(spec)
        self.assertEqual(got["source_epoch"], "1790063199")
        self.assertEqual(got["Version"], "2026.9.22.0")
        self.assertEqual(got["Source1"], "cinderx-deps-v2.2.3.tar.xz")
        self.assertEqual(got["deps_dir"], "cinderx-deps-ba5d5c5756f6")

    def test_blob_sha_matches_git(self):
        self.assertEqual(s.git_blob_sha(b""), "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391")


class DepsTarball(unittest.TestCase):
    def test_good_tarball(self):
        self.assertEqual(s.inspect_deps_tarball(deps_tar(), PINS), "cinderx-deps-ba5d5c5756f6")

    def test_pin_mismatch_stops(self):
        with self.assertRaisesRegex(s.Stop, "fmt: .git/HEAD"):
            s.inspect_deps_tarball(deps_tar(head={"fmt": "0" * 40}), PINS)

    def test_unexpected_config_stops(self):
        with self.assertRaisesRegex(s.Stop, "unexpected .git/config"):
            s.inspect_deps_tarball(deps_tar(config_extra="[core]\n\tfsmonitor = evil\n"), PINS)

    def test_real_hook_stops(self):
        extra = [("cinderx-deps-ba5d5c5756f6/usdt/.git/hooks/post-checkout", b"#!/bin/sh\n")]
        with self.assertRaisesRegex(s.Stop, "non-sample hook"):
            s.inspect_deps_tarball(deps_tar(extra=extra), PINS)

    def test_escaping_symlink_stops(self):
        extra = [("cinderx-deps-ba5d5c5756f6/fmt/evil", b"", tarfile.SYMTYPE, "../../../etc/passwd")]
        with self.assertRaisesRegex(s.Stop, "escapes"):
            s.inspect_deps_tarball(deps_tar(extra=extra), PINS)

    def test_two_top_dirs_stop(self):
        extra = [("other/file", b"x")]
        with self.assertRaisesRegex(s.Stop, "one top-level dir"):
            s.inspect_deps_tarball(deps_tar(extra=extra), PINS)


BASE = {
    "cinderx-v2.2.3.tar.gz": "a" * 40,
    "cinderx-deps-v2.2.3.tar.xz": "d" * 40,
    "cinderx.spec": "5" * 40,
    "README.md": "r" * 40,
    "setuptools-80.9.0-py3-none-any.whl": "w" * 40,
}
NEW = ("cinderx-v2.2.4.tar.gz", "b" * 40, "cinderx-deps-v2.2.4.tar.xz", "d" * 40)


class PlanFileOps(unittest.TestCase):
    def test_fresh_branch(self):
        ops, old_src, old_deps = s.plan_file_ops(BASE, dict(BASE), *NEW)
        self.assertEqual(ops, [("create", "cinderx-v2.2.4.tar.gz"), ("delete", "cinderx-v2.2.3.tar.gz"),
                               ("create", "cinderx-deps-v2.2.4.tar.xz"), ("delete", "cinderx-deps-v2.2.3.tar.xz")])
        self.assertEqual((old_src, old_deps), ("cinderx-v2.2.3.tar.gz", "cinderx-deps-v2.2.3.tar.xz"))

    def test_rerun_after_partial_failure_only_does_the_rest(self):
        branch = dict(BASE)
        branch["cinderx-v2.2.4.tar.gz"] = "b" * 40
        del branch["cinderx-v2.2.3.tar.gz"]
        ops, _, _ = s.plan_file_ops(BASE, branch, *NEW)
        self.assertEqual(ops, [("create", "cinderx-deps-v2.2.4.tar.xz"), ("delete", "cinderx-deps-v2.2.3.tar.xz")])

    def test_complete_branch_with_human_spec_needs_nothing(self):
        branch = {k: v for k, v in BASE.items() if not k.startswith("cinderx-")}
        branch.update({"cinderx-v2.2.4.tar.gz": "b" * 40, "cinderx-deps-v2.2.4.tar.xz": "d" * 40,
                       "cinderx.spec": "6" * 40})
        ops, _, _ = s.plan_file_ops(BASE, branch, *NEW)
        self.assertEqual(ops, [])

    def test_wrong_content_is_overwritten(self):
        branch = dict(BASE)
        branch["cinderx-v2.2.4.tar.gz"] = "c" * 40
        ops, _, _ = s.plan_file_ops(BASE, branch, *NEW)
        self.assertEqual(ops[0], ("update", "cinderx-v2.2.4.tar.gz"))

    def test_other_files_touched_stops(self):
        branch = dict(BASE)
        branch["README.md"] = "x" * 40
        with self.assertRaisesRegex(s.Stop, "unexpected changes"):
            s.plan_file_ops(BASE, branch, *NEW)

    def test_two_old_tarballs_stop(self):
        base = dict(BASE)
        base["cinderx-v2.2.2.tar.gz"] = "e" * 40
        with self.assertRaisesRegex(s.Stop, "exactly one source tarball"):
            s.plan_file_ops(base, dict(base), *NEW)

    def test_same_version_stops(self):
        with self.assertRaises(s.Stop):
            s.plan_file_ops(BASE, dict(BASE), "cinderx-v2.2.3.tar.gz", "b" * 40, NEW[2], NEW[3])


COMMIT = "1" * 40


def pr(state, ref="sync/v2.2.4", commit=COMMIT, merged_at=None, repo="cinderx-release/cinderx"):
    return {"number": 7, "state": state, "merged_at": merged_at,
            "head": {"ref": ref, "repo": {"full_name": repo}},
            "body": f"x\n<!-- cinderx-sync tag=v2.2.4 commit={commit} -->"}


class PrState(unittest.TestCase):
    def decide(self, prs):
        return s.pr_state(prs, "sync/v2.2.4", "cinderx-release/cinderx", COMMIT)[0]

    def test_states(self):
        self.assertEqual(self.decide([]), "none")
        self.assertEqual(self.decide([pr("merged")]), "merged")
        self.assertEqual(self.decide([pr("closed", merged_at="2026-11-11")]), "merged")
        self.assertEqual(self.decide([pr("closed")]), "closed")
        self.assertEqual(self.decide([pr("open")]), "open")
        self.assertEqual(self.decide([pr("open", commit="2" * 40)]), "open-mismatch")

    def test_closed_pr_with_deleted_branch_restarts(self):
        prs = [pr("closed")]
        self.assertEqual(s.pr_state(prs, "sync/v2.2.4", "cinderx-release/cinderx", COMMIT,
                                    branch_exists=False)[0], "restart")
        merged = [pr("closed", merged_at="2026-11-11")]
        self.assertEqual(s.pr_state(merged, "sync/v2.2.4", "cinderx-release/cinderx", COMMIT,
                                    branch_exists=False)[0], "merged")

    def test_other_branches_and_forks_ignored(self):
        self.assertEqual(self.decide([pr("open", ref="sync/v2.2.3"), pr("open", repo="someone/cinderx")]), "none")


class PrBody(unittest.TestCase):
    def test_body_has_values_and_marker_and_no_host_info(self):
        ctx = {"tag": "v2.2.4", "commit": COMMIT, "version": "2026.11.1.0", "epoch": 1793500000,
               "upstream": "openeuler/cinderx", "new_src": "cinderx-v2.2.4.tar.gz", "src_sha256": "f" * 64,
               "tar_sha256": "e" * 64, "new_deps": "cinderx-deps-v2.2.4.tar.xz",
               "old_deps": "cinderx-deps-v2.2.3.tar.xz", "git_version": "2.47.1"}
        body = s.pr_body(ctx)
        self.assertRegex(body, s.MARKER_RE)
        for needle in (COMMIT, "f" * 64, "e" * 64, "2026.11.1.0", "1793500000", "tar.umask=0002"):
            self.assertIn(needle, body)
        for leak in ("/home/", "/var/", "/etc/", "192.168.", "/lgtm", "/approve", "--verify-pr"):
            self.assertNotIn(leak, body)


class ReleaseRecordedCommit(unittest.TestCase):
    def parse(self, body, target_commitish=""):
        api = s.Api("")
        with mock.patch.object(api, "call", return_value=(200, {
            "body": body, "target_commitish": target_commitish,
        })):
            return api.release_commit(s.DEFAULT_UPSTREAM, "v3.0.3")

    def test_prefers_target_commitish_over_changelog_quotation(self):
        body = f"## 完整变更日志\n- upstream fix: ## 构建信息 Commit: `{'a' * 40}`\n"
        self.assertEqual(self.parse(body, "b" * 40), "b" * 40)
        self.assertIsNone(self.parse(body))

    def test_falls_back_to_the_build_info_section_only(self):
        body = (f"## 完整变更日志\n- quoted: ## 构建信息 Commit: `{'a' * 40}`\n"
                f"## 构建信息\n- Commit: `{'c' * 40}`\n## Other\n- Commit: `{'b' * 40}`\n")
        self.assertEqual(self.parse(body), "c" * 40)

    def test_body_match_stops_at_the_next_section(self):
        body = f"## 构建信息\n- No record\n## Other\n- Commit: `{'a' * 40}`\n"
        self.assertIsNone(self.parse(body))

    def test_conflicting_records_stop_the_run(self):
        body = f"## 构建信息\n- Commit: `{'c' * 40}`\n"
        with self.assertRaisesRegex(s.Stop, "conflicting commits"):
            self.parse(body, "d" * 40)


class CrossCheckSourceBranch(unittest.TestCase):
    def setUp(self):
        self.head = "a" * 40
        self.branch = "actual/release"
        self.fork = mock.Mock(repo="other-owner/cinderx")
        self.api = mock.Mock()
        self.args = SimpleNamespace(src=s.DEFAULT_SRC, branch=s.DEFAULT_BRANCH, verify_pr=7, head_branch=None)
        self.api.call.return_value = (200, {
            "state": "open",
            "head": {"ref": self.branch, "repo": {"full_name": self.fork.repo}, "sha": self.head},
            "base": {"ref": self.args.branch, "repo": {"full_name": self.args.src}},
        })

    def test_only_the_actual_source_branch_is_checked(self):
        self.fork.remote_branch.side_effect = lambda ref: self.head if ref == self.branch else "b" * 40
        repo, branch = s.verification_source(self.api, self.args, self.head)
        self.assertEqual(repo, self.fork.repo)
        s.cross_check_source_branch(self.fork, branch, self.head)
        self.fork.remote_branch.assert_called_once_with(self.branch)

    def test_missing_actual_branch_cannot_be_confirmed_by_a_leftover_branch(self):
        self.fork.remote_branch.side_effect = lambda ref: None if ref == self.branch else self.head
        _, branch = s.verification_source(self.api, self.args, self.head)
        with self.assertRaisesRegex(s.Stop, "missing"):
            s.cross_check_source_branch(self.fork, branch, self.head)
        self.fork.remote_branch.assert_called_once_with(self.branch)

    def test_actual_branch_at_a_different_commit_stops(self):
        self.fork.remote_branch.return_value = "b" * 40
        with self.assertRaisesRegex(s.Stop, "differs"):
            s.cross_check_source_branch(self.fork, self.branch, self.head)

    def test_stale_pr_ref_stops(self):
        with self.assertRaisesRegex(s.Stop, "metadata and Git head disagree"):
            s.verification_source(self.api, self.args, "b" * 40)

    def test_explicit_branch_must_match_metadata(self):
        self.args.head_branch = "sync/v3.0.3"
        with self.assertRaisesRegex(s.Stop, "differs from the PR source branch"):
            s.verification_source(self.api, self.args, self.head)


class VerifyTargetBranchState(unittest.TestCase):
    def setUp(self):
        workdir = TemporaryDirectory()
        self.addCleanup(workdir.cleanup)
        root = Path(workdir.name)
        self.mirror = s.Mirror(root, "local/artifacts")
        self.repo = root / "worktree"
        s.git(["init", "-q", str(self.repo)])
        self.run_git("config", "user.name", "CinderX Test")
        self.run_git("config", "user.email", "test@example.invalid")
        self.write_release("v3.0.2", "base")
        self.base = self.commit()
        self.write_release("v3.0.3", "PR")
        self.head = self.commit()
        self.spec = s.parse_spec((self.repo / s.SPEC).read_text())
        self.run_git("checkout", "-q", "--detach", self.base)

    def run_git(self, *args):
        return s.git(["-C", str(self.repo), *args])

    def commit(self):
        self.run_git("add", "-A")
        self.run_git("commit", "-qm", "test change")
        self.mirror.g("fetch", "-q", str(self.repo), "HEAD")
        return self.run_git("rev-parse", "HEAD")

    def write_release(self, tag, note, version="2026.11.17.0"):
        for path in self.repo.glob("*.tar.*"):
            path.unlink()
        src = f"cinderx-{tag}.tar.gz"
        deps = f"cinderx-deps-{tag}.tar.xz"
        (self.repo / src).write_text(tag)
        (self.repo / deps).write_text("unchanged dependencies")
        (self.repo / s.SPEC).write_text(f"Source0: {src}\nSource1: {deps}\nSummary: {note}\nVersion: {version}\n")

    def verify(self, target):
        s.verify_target_branch_state(self.mirror, mock.Mock(), target, self.head, "v3.0.3", "a" * 40, self.spec)

    def test_unrelated_target_change_merges_cleanly(self):
        (self.repo / "README.md").write_text("target-only change")
        self.verify(self.commit())

    def test_spec_content_conflict_with_unchanged_target_version_stops(self):
        self.write_release("v3.0.2", "target")
        with self.assertRaisesRegex(s.Stop, "CONFLICT.*cinderx.spec"):
            self.verify(self.commit())

    def test_dependency_rename_conflict_stops(self):
        self.write_release("v3.0.1", "target")
        with self.assertRaisesRegex(s.Stop, "rename/rename"):
            self.verify(self.commit())

    def test_version_rollback_against_newer_target_stops(self):
        self.write_release("v3.1.0", "target")
        with self.assertRaisesRegex(s.Stop, "refusing to go back"):
            self.verify(self.commit())

    def test_rpm_version_rollback_stops_even_when_the_tag_increases(self):
        self.write_release("v3.0.2", "base", version="2026.11.18.0")
        with self.assertRaisesRegex(s.Stop, "RPM Version.*2026.11.18.0.*2026.11.17.0"):
            self.verify(self.commit())


if __name__ == "__main__":
    unittest.main()
