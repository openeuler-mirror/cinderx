import hashlib
import io
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest import mock
from urllib.error import HTTPError
import zipfile


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from ci_pipeline.build_with_tag import gitcode_response as responses  # noqa: E402
from ci_pipeline.build_with_tag import gitcode_webhook as webhook  # noqa: E402


class ResponseLimitTests(unittest.TestCase):
    def test_json_response_is_bounded_without_content_length(self):
        for payload, too_large in ((b'{"ok":true}', False), (b'{"ok":true} ', True)):
            with self.subTest(payload=payload):
                body = io.BytesIO(payload)
                body.status = 200
                with mock.patch.object(responses, "MAX_JSON_RESPONSE_BYTES", 11), \
                     mock.patch.object(webhook.urllib.request, "urlopen", return_value=body), \
                     mock.patch.object(body, "read", wraps=body.read) as read:
                    if too_large:
                        with self.assertRaisesRegex(RuntimeError, "response exceeds 11 bytes"):
                            webhook.request_json("GET", "/release")
                    else:
                        self.assertEqual(webhook.request_json("GET", "/release"), (200, {"ok": True}))
                    read.assert_called_once_with(12)
                self.assertTrue(body.closed)

    def test_missing_release_closes_error_after_bounded_read(self):
        body = io.BytesIO(b"large error response")
        error = HTTPError("https://example.com", 404, "missing", {}, body)
        with mock.patch.object(responses, "MAX_ERROR_RESPONSE_BYTES", 4), \
             mock.patch.object(webhook.urllib.request, "urlopen", side_effect=error), \
             mock.patch.object(body, "read", wraps=body.read) as read:
            self.assertIsNone(webhook.existing_release_commit("v1.2.3"))
            read.assert_called_once_with(4)
        self.assertTrue(body.closed)

    def test_upload_uses_status_on_success_and_reads_bounded_error_excerpt(self):
        path = mock.Mock(spec=Path, name="asset")
        path.name = "test.whl"
        path.read_bytes.return_value = b"wheel"
        for status in (204, 500):
            with self.subTest(status=status):
                body = io.BytesIO(b"" if status == 204 else b"large error response")
                body.status = status
                urlopen = (
                    {"return_value": body} if status == 204 else
                    {"side_effect": HTTPError("https://example.com", status, "error", {}, body)}
                )
                with mock.patch.object(webhook, "request_json", return_value=(200, {"url": "https://example.com"})), \
                     mock.patch.object(webhook, "validate_upload_url"), \
                     mock.patch.object(responses, "MAX_ERROR_RESPONSE_BYTES", 4), \
                     mock.patch.object(webhook.urllib.request, "urlopen", **urlopen), \
                     mock.patch.object(body, "read", wraps=body.read) as read:
                    log = io.StringIO()
                    if status == 204:
                        webhook.upload_asset("v1.2.3", path, log)
                        self.assertIn("status=204", log.getvalue())
                        read.assert_not_called()
                    else:
                        with self.assertRaisesRegex(RuntimeError, r"returned 500: larg$"):
                            webhook.upload_asset("v1.2.3", path, log)
                        read.assert_called_once_with(4)
                self.assertTrue(body.closed)


class GitCodeWebhookTests(unittest.TestCase):
    def setUp(self):
        self._previous_release_tag = webhook.previous_release_tag
        self._git_log_entries = webhook.git_log_entries
        webhook.previous_release_tag = lambda tag, log: "v1.2.2"
        webhook.git_log_entries = lambda previous_tag, log: [
            "abc123 feat(jit): add fast path",
        ]

    def tearDown(self):
        webhook.previous_release_tag = self._previous_release_tag
        webhook.git_log_entries = self._git_log_entries

    def test_custom_tags_use_recent_commits(self):
        for tag in ("v3.0.0.test", "2026-11-13", "release/2026-11-13", "v3.0.0-rc.1"):
            with self.subTest(tag=tag):
                body = webhook.build_release_body(tag, "a" * 40, io.StringIO())
                self.assertIn(f"# {tag}", body)
                previous = "v1.2.2" if "-rc." in tag else "recent commits"
                self.assertIn(f"Changes since `{previous}`:", body)
        with self.assertRaisesRegex(RuntimeError, "invalid Git tag"):
            webhook.validate_release_tag("bad..tag")

    def test_fetch_tag_peels_annotated_tag_and_rejects_stale_event(self):
        with tempfile.TemporaryDirectory() as tmp:
            origin, checkout = Path(tmp) / "origin", Path(tmp) / "checkout"

            def git(cwd, *args):
                return subprocess.check_output(
                    ["git", "-C", str(cwd), "-c", "user.name=Tester",
                     "-c", "user.email=test@example.invalid", *args],
                    stderr=subprocess.PIPE, text=True,
                ).strip()

            origin.mkdir()
            checkout.mkdir()
            git(origin, "init")
            git(origin, "commit", "--allow-empty", "-m", "fixture")
            sha = git(origin, "rev-parse", "HEAD")
            git(origin, "tag", "v3.0.0.test")
            git(origin, "tag", "-a", "release/drill", "-m", "fixture")
            annotated = git(origin, "rev-parse", "refs/tags/release/drill")
            git(checkout, "init")
            git(checkout, "remote", "add", "origin", str(origin))
            with mock.patch.multiple(webhook, REPO_DIR=checkout, REPO_URL=str(origin), ACCESS_TOKEN=""):
                self.assertEqual(webhook.fetch_tag("v3.0.0.test", sha, io.StringIO()), (sha, sha))
                self.assertEqual(webhook.fetch_tag("release/drill", annotated, io.StringIO()), (annotated, sha))
                with self.assertRaisesRegex(RuntimeError, "event SHA"):
                    webhook.fetch_tag("release/drill", "0" * 40, io.StringIO())

    def test_duplicate_publish_requires_matching_live_release(self):
        tag, sha = "release/drill", "a" * 40
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            key = hashlib.sha256(webhook.json.dumps([webhook.OWNER, webhook.REPO, tag]).encode()).hexdigest()
            (path / f"{key}.success").write_text(sha, encoding="utf-8")
            with (
                mock.patch.multiple(webhook, LOG_DIR=path, STATE_DIR=path, RUN_DIR=path, ACCESS_TOKEN="fixture"),
                mock.patch.object(webhook, "fetch_tag", return_value=(sha, sha)),
                mock.patch.object(webhook, "checkout_tag", side_effect=RuntimeError("rebuild required")) as checkout,
                mock.patch.object(webhook, "existing_release_commit", return_value=sha) as release,
            ):
                webhook.build_and_publish(tag, sha)
                checkout.assert_not_called()
                with self.assertRaisesRegex(RuntimeError, "rebuild required"):
                    webhook.build_and_publish("another-tag", sha)
                release.return_value = None
                with self.assertRaisesRegex(RuntimeError, "rebuild required"):
                    webhook.build_and_publish(tag, sha)
                release.return_value = "b" * 40
                with self.assertRaisesRegex(RuntimeError, "tag points to"):
                    webhook.build_and_publish(tag, sha)

    def test_collect_wheel_assets_writes_sha256_sidecar(self):
        with tempfile.TemporaryDirectory() as tmp:
            wheel = Path(tmp) / "cinderx-1.2.3-cp314-cp314-manylinux.whl"
            data = b"wheel bytes"
            wheel.write_bytes(data)

            assets = webhook.collect_wheel_assets([wheel])

            expected = hashlib.sha256(data).hexdigest()
            self.assertEqual(len(assets), 1)
            self.assertEqual(assets[0]["name"], wheel.name)
            self.assertEqual(assets[0]["sha256"], expected)
            sidecar = Path(assets[0]["sha256_path"])
            self.assertEqual(sidecar.name, f"{wheel.name}.sha256")
            self.assertEqual(sidecar.read_text(encoding="utf-8"), f"{expected}  {wheel.name}\n")

    def test_release_manifest_accepts_the_expected_pair(self):
        wheels = [
            Path("cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"),
            Path("cinderx-1.2.3-cp311-cp311-linux_aarch64.whl"),
        ]
        webhook.validate_release_manifest(wheels, io.StringIO())

    def test_release_manifest_rejects_a_missing_cp314_wheel(self):
        wheels = [Path("cinderx-1.2.3-cp311-cp311-linux_aarch64.whl")]
        with self.assertRaisesRegex(RuntimeError, "exactly one cp314"):
            webhook.validate_release_manifest(wheels, io.StringIO())

    def test_release_manifest_rejects_duplicate_cp311_wheels(self):
        wheels = [
            Path("cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"),
            Path("cinderx-1.2.3-cp311-cp311-linux_aarch64.whl"),
            Path("cinderx-1.2.3-1-cp311-cp311-linux_aarch64.whl"),
        ]
        with self.assertRaisesRegex(RuntimeError, "exactly one cp311"):
            webhook.validate_release_manifest(wheels, io.StringIO())

    def test_release_manifest_rejects_version_mismatch(self):
        wheels = [
            Path("cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"),
            Path("cinderx-1.2.4-cp311-cp311-linux_aarch64.whl"),
        ]
        with self.assertRaisesRegex(RuntimeError, "versions disagree"):
            webhook.validate_release_manifest(wheels, io.StringIO())

    def test_release_manifest_rejects_wrong_cp311_platform(self):
        wheels = [
            Path("cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"),
            Path("cinderx-1.2.3-cp311-cp311-manylinux_2_28_aarch64.whl"),
        ]
        with self.assertRaisesRegex(RuntimeError, "unexpected platform"):
            webhook.validate_release_manifest(wheels, io.StringIO())

    def test_release_manifest_rejects_foreign_wheels(self):
        wheels = [
            Path("cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"),
            Path("cinderx-1.2.3-cp311-cp311-linux_aarch64.whl"),
            Path("cinderx-1.2.3-cp312-cp312-linux_aarch64.whl"),
        ]
        with self.assertRaisesRegex(RuntimeError, "unexpected wheels"):
            webhook.validate_release_manifest(wheels, io.StringIO())

    def test_release_manifest_rejects_wrong_abi(self):
        for wheels in (
            [
                Path("cinderx-1.2.3-cp314-abi3-manylinux_2_28_aarch64.whl"),
                Path("cinderx-1.2.3-cp311-cp311-linux_aarch64.whl"),
            ],
            [
                Path("cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"),
                Path("cinderx-1.2.3-cp311-abi3-linux_aarch64.whl"),
            ],
        ):
            with self.assertRaisesRegex(RuntimeError, "unexpected abi"):
                webhook.validate_release_manifest(wheels, io.StringIO())

    def test_release_manifest_rejects_fabricated_platform(self):
        wheels = [
            Path("cinderx-1.2.3-cp314-cp314-notmanylinux_but_aarch64.whl"),
            Path("cinderx-1.2.3-cp311-cp311-linux_aarch64.whl"),
        ]
        with self.assertRaisesRegex(RuntimeError, "unexpected platform"):
            webhook.validate_release_manifest(wheels, io.StringIO())

    @staticmethod
    def _write_fat_wheel(path, version="1.2.3", variants=("py314_0", "py314_1", "py314_2", "py314_3"),
                         loader=True, top_level_so=False):
        with zipfile.ZipFile(path, "w") as wheel:
            if loader:
                wheel.writestr("_cinderx.py", "# loader")
            wheel.writestr("cinderx/_native/fat_wheel.json", "{}")
            for variant in variants:
                wheel.writestr(f"cinderx/_native/{variant}/_cinderx.so", b"native")
            if top_level_so:
                wheel.writestr("_cinderx.so", b"native")
            wheel.writestr(
                f"cinderx-{version}.dist-info/METADATA",
                f"Metadata-Version: 2.1\nName: cinderx\nVersion: {version}\n",
            )

    def test_fat_wheel_content_accepts_a_real_fat_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            wheel = Path(tmp) / "cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"
            self._write_fat_wheel(wheel)
            webhook.validate_fat_wheel_content(wheel, "1.2.3", io.StringIO())

    def test_fat_wheel_content_rejects_an_ordinary_wheel(self):
        with tempfile.TemporaryDirectory() as tmp:
            wheel = Path(tmp) / "cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"
            with zipfile.ZipFile(wheel, "w") as zf:
                zf.writestr("_cinderx.so", b"native")
                zf.writestr(
                    "cinderx-1.2.3.dist-info/METADATA",
                    "Metadata-Version: 2.1\nName: cinderx\nVersion: 1.2.3\n",
                )
            with self.assertRaisesRegex(RuntimeError, "fat loader"):
                webhook.validate_fat_wheel_content(wheel, "1.2.3", io.StringIO())

    def test_fat_wheel_content_rejects_a_missing_variant(self):
        with tempfile.TemporaryDirectory() as tmp:
            wheel = Path(tmp) / "cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"
            self._write_fat_wheel(wheel, variants=("py314_0", "py314_1", "py314_2"))
            with self.assertRaisesRegex(RuntimeError, "native variants"):
                webhook.validate_fat_wheel_content(wheel, "1.2.3", io.StringIO())

    def test_fat_wheel_content_rejects_version_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            wheel = Path(tmp) / "cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"
            self._write_fat_wheel(wheel, version="1.2.4")
            with self.assertRaisesRegex(RuntimeError, "METADATA version"):
                webhook.validate_fat_wheel_content(wheel, "1.2.3", io.StringIO())

    def test_release_body_python_line_follows_the_actual_assets(self):
        cp314_only = [
            {
                "name": "cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl",
                "sha256": "a" * 64,
                "sha256_name": "cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl.sha256",
            }
        ]
        body = webhook.build_release_body("v1.2.3", "b" * 40, io.StringIO(), cp314_only)
        self.assertIn("CPython 3.14", body)
        self.assertNotIn("CPython 3.11", body)

    def test_release_manifest_rejects_cp311_when_disabled(self):
        wheels = [
            Path("cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"),
            Path("cinderx-1.2.3-cp311-cp311-linux_aarch64.whl"),
        ]
        previous = webhook.RELEASE_CP311
        webhook.RELEASE_CP311 = False
        try:
            with self.assertRaisesRegex(RuntimeError, "CINDERX_RELEASE_CP311 is off"):
                webhook.validate_release_manifest(wheels, io.StringIO())
        finally:
            webhook.RELEASE_CP311 = previous

    def test_release_body_includes_hash_checked_install_instructions(self):
        digest = "a" * 64
        wheel_name = "cinderx-1.2.3-cp314-cp314-manylinux.whl"
        body = webhook.build_release_body(
            "v1.2.3",
            "b" * 40,
            io.StringIO(),
            [
                {
                    "name": wheel_name,
                    "sha256": digest,
                    "sha256_name": f"{wheel_name}.sha256",
                }
            ],
        )

        self.assertIn("## 完整性校验", body)
        self.assertIn("sha256sum -c <wheel-file>.sha256", body)
        self.assertIn("--require-hashes", body)
        self.assertIn(f"./{wheel_name} --hash=sha256:{digest}", body)
        self.assertIn(f"`{wheel_name}`: `{digest}`", body)




class ExistingReleaseCommitTests(unittest.TestCase):
    def _release(self, body, target=""):
        rel = {"body": body}
        if target:
            rel["target_commitish"] = target
        return rel

    def test_generated_changelog_quotation_is_never_mistaken_for_the_release_commit(self):
        release_commit = "b" * 40
        entry = f"123abc fix: quotes ## 构建信息 and Commit: `{'a' * 40}`"
        with mock.patch.object(webhook, "previous_release_tag", return_value=""), \
             mock.patch.object(webhook, "git_log_entries", return_value=[entry]):
            body = webhook.build_release_body("v3.0.1", release_commit, None)
        self.assertIn(entry, body)
        for target in (release_commit, ""):
            with self.subTest(target=target), \
                 mock.patch.object(webhook, "request_json", return_value=(200, self._release(body, target))):
                self.assertEqual(webhook.existing_release_commit("v3.0.1"), release_commit)

    def test_target_commitish_wins_without_a_body_section(self):
        with mock.patch.object(webhook, "request_json", return_value=(200, self._release("", "b" * 40))):
            self.assertEqual(webhook.existing_release_commit("v3.0.1"), "b" * 40)

    def test_body_build_info_section_is_the_fallback(self):
        body = "changelog with Commit: `" + "a" * 40 + "` quoted\n\n## 构建信息\n- Commit: `" + "c" * 40 + "`\n"
        with mock.patch.object(webhook, "request_json", return_value=(200, self._release(body))):
            self.assertEqual(webhook.existing_release_commit("v3.0.1"), "c" * 40)

    def test_changelog_only_body_has_no_record(self):
        body = "## 完整变更日志\n- upstream fix: Commit: `" + "a" * 40 + "`\n"
        with mock.patch.object(webhook, "request_json", return_value=(200, self._release(body))):
            with self.assertRaisesRegex(RuntimeError, "does not record a commit"):
                webhook.existing_release_commit("v3.0.1")

    def test_conflicting_target_and_body_records_raise(self):
        body = "## 构建信息\n- Commit: `" + "c" * 40 + "`\n"
        with mock.patch.object(webhook, "request_json", return_value=(200, self._release(body, "d" * 40))):
            with self.assertRaisesRegex(RuntimeError, "conflicting commits"):
                webhook.existing_release_commit("v3.0.1")

    def test_missing_release_returns_none(self):
        exc = RuntimeError("GET /x returned 404: nope")
        with mock.patch.object(webhook, "request_json", side_effect=exc):
            self.assertIsNone(webhook.existing_release_commit("v3.0.9"))


if __name__ == "__main__":
    unittest.main()
