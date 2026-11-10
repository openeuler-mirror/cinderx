"""Resource regressions independent of version-specific libregrtest imports."""

import ast
import dataclasses
import json
import os
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path
from typing import List
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "cinderx" / "TestScripts"


def load_test_log(path):
    # Execute the actual class without requiring a particular libregrtest version
    # or a native CinderX installation on the machine running these unit tests.
    tree = ast.parse(path.read_text(encoding="utf-8"))
    node = next(
        n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == "TestLog"
    )
    namespace = dict(tempfile=tempfile, json=json, shutil=shutil, os=os, List=List)
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(path), "exec"), namespace)
    return namespace["TestLog"]


class TestRunnerTempfiles(unittest.TestCase):
    def test_worker_config_closed_before_spawn(self):
        source = SCRIPTS / "cinder_test_runner312.py"
        tree = ast.parse(source.read_text(encoding="utf-8"))
        node = next(
            n
            for n in tree.body
            if isinstance(n, ast.FunctionDef) and n.name == "start_worker"
        )
        files = []
        original = tempfile.NamedTemporaryFile
        config = dataclasses.make_dataclass("Config", [("tests", list)])(["test_one"])
        with tempfile.TemporaryDirectory() as directory:

            def create(*args, **kwargs):
                result = original(*args, dir=directory, **kwargs)
                files.append(result)
                return result

            def spawn(command, **kwargs):
                self.assertTrue(files[0].closed)
                path = Path(command[-1])
                self.assertEqual(
                    json.loads(path.read_text(encoding="utf-8")),
                    dataclasses.asdict(config),
                )
                path.unlink()  # Match the real worker's ownership of the file.
                return object()

            namespace = dict(
                dataclasses=dataclasses,
                tempfile=tempfile,
                os=os,
                sys=sys,
                json=json,
                subprocess=types.SimpleNamespace(Popen=spawn),
                libregrtest_runtests=types.SimpleNamespace(RunTests=object),
                WorkSender=lambda *args: args,
                MessagePipe=lambda *args: args,
                WORKER_PATH=str(source),
                get_cinderjit_xargs=lambda: [],
                os_helper=types.SimpleNamespace(SAVEDCWD=directory),
            )
            exec(
                compile(ast.Module(body=[node], type_ignores=[]), str(source), "exec"),
                namespace,
            )
            real_pipe = os.pipe
            fds = []

            def pipe():
                pair = real_pipe()
                fds.extend(pair)
                return pair

            try:
                with (
                    mock.patch("tempfile.NamedTemporaryFile", side_effect=create),
                    mock.patch("os.pipe", side_effect=pipe),
                ):
                    namespace["start_worker"](config, 0, False)
            finally:
                for f in files:
                    f.close()
                for fd in fds:
                    try:
                        os.close(fd)
                    except OSError:
                        pass

    def check_log(self, source, shared):
        cls = load_test_log(source)
        files = []
        original = tempfile.NamedTemporaryFile
        with tempfile.TemporaryDirectory() as directory:

            def create(*args, **kwargs):
                kwargs["dir"] = directory
                result = original(*args, **kwargs)
                files.append(result)  # Prevent refcount cleanup hiding a leak.
                return result

            def publish(src, dst):
                self.assertTrue(all(f.closed for f in files))
                # Publication must expose complete JSON even before the writer
                # returns, including when a reader lives in another process.
                json.loads(Path(src).read_text(encoding="utf-8"))
                return original_publish(src, dst)

            original_publish = shutil.move if shared else os.replace
            target = "shutil.move" if shared else "os.replace"
            try:
                with mock.patch("tempfile.NamedTemporaryFile", side_effect=create):
                    with mock.patch(target, side_effect=publish):
                        log = cls(123) if shared else cls()
                        self.assertTrue(files[0].closed)
                        path = log.path
                        for name in ("test_one", "test_two", "test_one"):
                            log.add_test(name)
                            self.assertEqual(log.path, path)
                            self.assertTrue(all(f.closed for f in files))
                            data = json.loads(
                                subprocess.check_output(
                                    [
                                        sys.executable,
                                        "-c",
                                        "import json,sys; print(json.dumps(json.load(open(sys.argv[1]))))",
                                        path,
                                    ],
                                    text=True,
                                )
                            )
                            self.assertEqual(data["test_order"], log.test_order)
                        if shared:
                            replay = cls(path=path)
                            self.assertEqual(replay.pid, 123)
                            self.assertEqual(replay.test_order, log.test_order)
                        self.assertEqual(
                            list(Path(directory).iterdir()), [Path(path)]
                        )
            finally:
                for f in files:
                    f.close()

    def test_shared_log_closes_before_publication_and_replays(self):
        self.check_log(SCRIPTS / "common.py", shared=True)

    def test_kunpeng_log_closes_before_publication(self):
        self.check_log(
            SCRIPTS / "TestScriptsKunpeng" / "lib_test_dispatcher.py", shared=False
        )


if __name__ == "__main__":
    unittest.main()
