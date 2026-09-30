import ast
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


def _load_setup_function(name: str):
    setup_py = Path(__file__).resolve().parents[4] / "setup.py"
    setup_ast = ast.parse(setup_py.read_text(), filename=str(setup_py))
    for node in setup_ast.body:
        if isinstance(node, ast.FunctionDef) and node.name == name:
            module = ast.Module(body=[node], type_ignores=[])
            ast.fix_missing_locations(module)
            namespace: dict[str, object] = {
                "hashlib": hashlib,
                "json": json,
                "os": os,
            }
            exec(compile(module, str(setup_py), "exec"), namespace)
            return namespace[name]
    raise RuntimeError(f"{name} not found in {setup_py}")


resolve_pgo_workload = _load_setup_function("resolve_pgo_workload")
is_env_flag_enabled = _load_setup_function("is_env_flag_enabled")
pgo_cmake_options = _load_setup_function("pgo_cmake_options")
remove_stale_pgo_profiles = _load_setup_function("remove_stale_pgo_profiles")
require_nonempty_pgo_profiles = _load_setup_function(
    "require_nonempty_pgo_profiles"
)
pgo_profile_records = _load_setup_function("pgo_profile_records")
write_pgo_profile_manifest = _load_setup_function("write_pgo_profile_manifest")
write_pgo_profile_manifest.__globals__["pgo_profile_records"] = pgo_profile_records
pgo_workload_environment = _load_setup_function("pgo_workload_environment")
pgo_workload_timeout = _load_setup_function("pgo_workload_timeout")
ensure_cinderx_not_loaded = _load_setup_function("ensure_cinderx_not_loaded")

_REPOSITORY_WORKLOAD = str(
    Path(__file__).resolve().parents[4]
    / "cinderx"
    / "TestScripts"
    / "pgo_train_workloads.py"
)
# Twelve serial scenarios, 120s each by default (see pgo_train_workloads.py).
_WORST_CASE_DEFAULT = 12 * 120


class PgoWorkloadTests(unittest.TestCase):
    def test_cp311_uses_repository_workload_by_default(self) -> None:
        root = Path(__file__).resolve().parents[4]
        command = resolve_pgo_workload(
            None, "/usr/bin/python3", "3.11", str(root), "/tmp/build-lib"
        )
        self.assertEqual(command[:2], ["/usr/bin/python3", "-S"])
        self.assertEqual(Path(command[2]).name, "pgo_train_workloads.py")
        self.assertEqual(
            command[3:], ["--build-lib", os.path.abspath("/tmp/build-lib")]
        )

    def test_other_python_versions_keep_existing_default(self) -> None:
        self.assertIsNone(
            resolve_pgo_workload(
                None, "/usr/bin/python3", "3.14", "/checkout", "/tmp/build-lib"
            )
        )

    def test_custom_workload_is_absolute_and_uses_requested_python(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            workload = Path(directory) / "train.py"
            workload.touch()
            command = resolve_pgo_workload(
                str(workload),
                "/usr/bin/python3",
                "3.11",
                "/checkout",
                "/tmp/build-lib",
            )
            self.assertEqual(command[0], "/usr/bin/python3")
            self.assertEqual(command[1], "-S")
            self.assertTrue(Path(command[2]).samefile(workload))

    def test_other_python_versions_do_not_add_isolation_flag(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            workload = Path(directory) / "train.py"
            workload.touch()
            command = resolve_pgo_workload(
                str(workload),
                "/usr/bin/python3",
                "3.14",
                "/checkout",
                "/tmp/build-lib",
            )
            self.assertEqual(command[0], "/usr/bin/python3")
            self.assertTrue(Path(command[1]).samefile(workload))

    def test_missing_custom_workload_raises_an_error(self) -> None:
        with self.assertRaisesRegex(FileNotFoundError, "PGO workload does not exist"):
            resolve_pgo_workload(
                "missing-pgo-workload.py",
                "/usr/bin/python3",
                "3.11",
                "/checkout",
                "/tmp/build-lib",
            )

    def test_workload_environment_removes_external_python_paths(self) -> None:
        environment = pgo_workload_environment(
            {
                "PATH": "/bin",
                "PYTHONPATH": "/external",
                "PYTHONHOME": "/python-home",
                "PYTHONUSERBASE": "/user-base",
                "CINDERX_PLUGIN_ENABLE": "1",
            },
            "/tmp/build-lib",
        )
        self.assertEqual(environment["PATH"], "/bin")
        self.assertEqual(environment["PYTHONPATH"], os.path.abspath("/tmp/build-lib"))
        self.assertEqual(environment["PYTHONNOUSERSITE"], "1")
        self.assertEqual(environment["PYTHONHASHSEED"], "0")
        self.assertEqual(environment["CINDERX_PLUGIN_ENABLE"], "0")
        self.assertNotIn("PYTHONHOME", environment)
        self.assertNotIn("PYTHONUSERBASE", environment)

    def test_preloaded_extension_stops_the_build(self) -> None:
        ensure_cinderx_not_loaded({})
        with self.assertRaisesRegex(RuntimeError, "already loaded"):
            ensure_cinderx_not_loaded({"_cinderx": object()})

    def test_build_flags_only_enable_on_nonzero_values(self) -> None:
        for value in (None, "", "0"):
            with self.subTest(value=value):
                self.assertFalse(is_env_flag_enabled(value))
        for value in ("1", "2"):
            with self.subTest(value=value):
                self.assertTrue(is_env_flag_enabled(value))

    def test_disabled_pgo_resets_both_cmake_stages(self) -> None:
        self.assertEqual(
            pgo_cmake_options(generate=False, use=False),
            ["-DENABLE_PGO_GENERATE=OFF", "-DENABLE_PGO_USE=OFF"],
        )

    def test_generate_and_use_pgo_stages_are_explicit(self) -> None:
        self.assertEqual(
            pgo_cmake_options(generate=True, use=False),
            ["-DENABLE_PGO_GENERATE=ON", "-DENABLE_PGO_USE=OFF"],
        )
        self.assertEqual(
            pgo_cmake_options(
                generate=False,
                use=True,
                profile_path="/profiles/code.profdata",
            ),
            [
                "-DENABLE_PGO_GENERATE=OFF",
                "-DENABLE_PGO_USE=ON",
                "-DPGO_PROFILE_FILE=/profiles/code.profdata",
            ],
        )

    def test_generate_and_use_cannot_be_enabled_together(self) -> None:
        with self.assertRaisesRegex(ValueError, "mutually exclusive"):
            pgo_cmake_options(generate=True, use=True)


class PgoWorkloadTimeoutTests(unittest.TestCase):
    def test_repository_default_covers_worst_case(self) -> None:
        self.assertEqual(
            pgo_workload_timeout({}, "repository", _REPOSITORY_WORKLOAD),
            _WORST_CASE_DEFAULT,
        )

    def test_repository_scenario_timeout_scales_the_budget(self) -> None:
        self.assertEqual(
            pgo_workload_timeout(
                {"CINDERX_PGO_SCENARIO_TIMEOUT": "30"},
                "repository",
                _REPOSITORY_WORKLOAD,
            ),
            12 * 30,
        )

    def test_explicit_override_is_never_below_the_worst_case(self) -> None:
        clamped = pgo_workload_timeout(
            {"CINDERX_PGO_WORKLOAD_TIMEOUT": "600"},
            "repository",
            _REPOSITORY_WORKLOAD,
        )
        self.assertEqual(clamped, _WORST_CASE_DEFAULT)
        honored = pgo_workload_timeout(
            {"CINDERX_PGO_WORKLOAD_TIMEOUT": "5000"},
            "repository",
            _REPOSITORY_WORKLOAD,
        )
        self.assertEqual(honored, 5000)

    def test_non_repository_workload_keeps_the_fixed_default(self) -> None:
        self.assertEqual(pgo_workload_timeout({}, "custom", None), 600)
        self.assertEqual(
            pgo_workload_timeout(
                {"CINDERX_PGO_WORKLOAD_TIMEOUT": "900"}, "custom", None
            ),
            900,
        )


class PgoProfileTests(unittest.TestCase):
    def test_nonempty_profile_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory) / "module.gcda"
            profile.write_bytes(b"profile")
            self.assertEqual(
                require_nonempty_pgo_profiles([str(profile)], "GCC"),
                [str(profile)],
            )

    def test_missing_profile_raises_an_error(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "No PGO profile data generated.*GCC"):
            require_nonempty_pgo_profiles([], "GCC")

    def test_empty_profile_raises_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory) / "module.gcda"
            profile.touch()
            with self.assertRaisesRegex(RuntimeError, "profile data is empty.*GCC"):
                require_nonempty_pgo_profiles([str(profile)], "GCC")

    def test_stale_profiles_are_removed_without_touching_other_files(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stale = root / "old.gcda"
            keep = root / "module.gcno"
            stale.touch()
            keep.touch()
            self.assertEqual(
                remove_stale_pgo_profiles(str(root), (".gcda",)),
                [str(stale)],
            )
            self.assertFalse(stale.exists())
            self.assertTrue(keep.exists())

    def test_manifest_records_size_and_digest(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile = root / "data" / "module.gcda"
            profile.parent.mkdir()
            profile.write_bytes(b"profile")
            manifest = root / "metadata" / "manifest.json"
            records = write_pgo_profile_manifest(
                [str(profile)],
                str(root),
                str(manifest),
                {"workload_kind": "repository"},
            )
            self.assertEqual(records, pgo_profile_records([str(profile)], str(root)))
            self.assertEqual(records[0]["path"], os.path.join("data", "module.gcda"))
            self.assertEqual(records[0]["size"], 7)
            self.assertEqual(len(records[0]["sha256"]), 64)
            contents = json.loads(manifest.read_text())
            self.assertEqual(contents["profiles"], records)
            self.assertEqual(contents["workload_kind"], "repository")
            profile.write_bytes(b"changed")
            self.assertNotEqual(
                records, pgo_profile_records([str(profile)], str(root))
            )


class PgoCompilerValidationTests(unittest.TestCase):
    def test_profile_use_diagnostics_are_errors(self) -> None:
        cmake = (Path(__file__).resolve().parents[4] / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        for warning in (
            "-Werror=coverage-mismatch",
            "-Werror=profile-instr-out-of-date",
        ):
            with self.subTest(warning=warning):
                self.assertIn(warning, cmake)
        self.assertNotIn("-Werror=missing-profile", cmake)
        self.assertNotIn("-Werror=profile-instr-missing", cmake)


@unittest.skipUnless(shutil.which("gcc"), "GCC is required for PGO toolchain test")
class GccPgoToolchainTests(unittest.TestCase):
    SOURCE = """\
__attribute__((noinline)) int hot(int value) {
    if (value > 0) {
        return value + 1;
    }
    return value - 1;
}

int main(void) {
    return hot(1) == 2 ? 0 : 1;
}
"""

    MISMATCHED_SOURCE = SOURCE.replace(
        "    if (value > 0) {",
        "    if (value == 99) { return 0; }\n    if (value > 0) {",
    )

    def _compile(self, source: Path, executable: Path, profile_use: bool) -> None:
        command = [shutil.which("gcc"), str(source), "-O2", "-o", str(executable)]
        if profile_use:
            command.extend(
                [
                    "-fprofile-use",
                    "-fprofile-correction",
                    "-fprofile-partial-training",
                    "-Werror=coverage-mismatch",
                ]
            )
        else:
            command.append("-fprofile-generate")
        subprocess.run(command, cwd=source.parent, check=True, capture_output=True)

    def test_gcc_consumes_matching_profile(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "profile_test.c"
            executable = root / "profile_test"
            source.write_text(self.SOURCE, encoding="utf-8")
            self._compile(source, executable, profile_use=False)
            subprocess.run([executable], cwd=root, check=True)
            profiles = list(root.glob("*.gcda"))
            self.assertTrue(profiles)
            self.assertTrue(all(profile.stat().st_size > 0 for profile in profiles))
            self._compile(source, executable, profile_use=True)

    def test_gcc_rejects_mismatched_profile(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "profile_test.c"
            executable = root / "profile_test"
            source.write_text(self.SOURCE, encoding="utf-8")
            self._compile(source, executable, profile_use=False)
            subprocess.run([executable], cwd=root, check=True)
            source.write_text(self.MISMATCHED_SOURCE, encoding="utf-8")
            with self.assertRaises(subprocess.CalledProcessError) as raised:
                self._compile(source, executable, profile_use=True)
            self.assertIn(
                "coverage-mismatch",
                raised.exception.stderr.decode(errors="replace"),
            )


if __name__ == "__main__":
    unittest.main()
