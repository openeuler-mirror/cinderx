#!/usr/bin/env python3
"""Run CPython Lib/test modules with reusable Kunpeng gate workers."""

from __future__ import annotations

import argparse
import dataclasses
import gc
# Match regrtest's import environment for stdlib tests that access
# importlib.util after only importing importlib.
import importlib.util
import json
import multiprocessing
import os
import pickle
import queue
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest
from pathlib import Path
from typing import Any, Iterable

import test.libregrtest.findtests as libregrtest_findtests
import test.libregrtest.logger as libregrtest_logger
import test.libregrtest.result as libregrtest_result
import test.libregrtest.results as libregrtest_results
import test.libregrtest.runtests as libregrtest_runtests
import test.libregrtest.setup as libregrtest_setup
import test.libregrtest.single as libregrtest_single
from test import support
from test.support import os_helper


MAX_WORKERS = 64
WORKER_RESPAWN_INTERVAL = 10
WORKER_PATH = Path(__file__).resolve()


class Message:
    pass


@dataclasses.dataclass(frozen=True)
class RunTest(Message):
    test_name: str


@dataclasses.dataclass(frozen=True)
class TestStarted(Message):
    worker_pid: int
    test_name: str
    test_log: str


@dataclasses.dataclass(frozen=True)
class TestComplete(Message):
    test_name: str
    result: Any


class ShutdownWorker(Message):
    pass


class WorkerDone(Message):
    pass


class MessagePipe:
    def __init__(self, read_fd: int, write_fd: int) -> None:
        self.infile = os.fdopen(read_fd, "rb")
        self.outfile = os.fdopen(write_fd, "wb")

    def __enter__(self) -> "MessagePipe":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> bool:
        self.close()
        return False

    def close(self) -> None:
        self.infile.close()
        self.outfile.close()

    def recv(self) -> Message:
        return pickle.load(self.infile)

    def send(self, message: Message) -> None:
        pickle.dump(message, self.outfile)
        self.outfile.flush()


class TestLog:
    def __init__(self) -> None:
        with tempfile.NamedTemporaryFile(
            prefix="testgate-dispatcher-", suffix=".json", delete=False
        ) as f:
            self.path = f.name
        self.test_order: list[str] = []
        self._serialize()

    def add_test(self, test_name: str) -> None:
        self.test_order.append(test_name)
        self._serialize()

    def _serialize(self) -> None:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", delete=False) as f:
            json.dump({"test_order": self.test_order}, f)
            temp_name = f.name
        os.replace(temp_name, self.path)


class WorkSender:
    def __init__(self, pipe: MessagePipe, popen: subprocess.Popen[bytes]) -> None:
        self.pipe = pipe
        self.popen = popen
        self.ncompleted = 0
        self.test_log = TestLog()

    @property
    def pid(self) -> int:
        return self.popen.pid

    def send(self, msg: Message) -> None:
        if isinstance(msg, RunTest):
            self.test_log.add_test(msg.test_name)
        self.pipe.send(msg)

    def recv(self) -> Message:
        msg = self.pipe.recv()
        if isinstance(msg, TestComplete):
            self.ncompleted += 1
        return msg

    def shutdown(self) -> None:
        self.pipe.send(ShutdownWorker())
        self.wait()

    def wait(self) -> None:
        self.popen.wait()
        self.pipe.close()


def read_lines(path: Path) -> list[str]:
    with path.open(encoding="utf-8") as f:
        return [
            line.strip()
            for line in f
            if line.strip() and not line.lstrip().startswith("#")
        ]


def current_jit_xargs() -> list[str]:
    args: list[str] = []
    for key, value in sys._xoptions.items():
        if not key.startswith("jit"):
            continue
        if value is True:
            args.extend(["-X", key])
        else:
            args.extend(["-X", f"{key}={value}"])
    return args


def default_num_workers() -> int:
    return min(multiprocessing.cpu_count(), MAX_WORKERS)


def parse_num_workers(value: str | None) -> int:
    if value in (None, "auto"):
        return default_num_workers()
    workers = int(value)
    if workers <= 0:
        raise argparse.ArgumentTypeError("--num-workers must be positive or 'auto'")
    return workers


def patch_libregrtest_to_use_loadTestsFromName() -> None:
    def patched_run_unittest(test_name):
        loader = libregrtest_single.unittest.TestLoader()
        tests = loader.loadTestsFromName(test_name, None)
        for error in loader.errors:
            print(error, file=sys.stderr)
        if loader.errors:
            raise Exception("errors while loading tests")
        libregrtest_single._filter_suite(tests, libregrtest_single.match_test)
        return libregrtest_single._run_suite(tests)

    def patched__load_run_test(
        result: libregrtest_single.TestResult, runtests: libregrtest_single.RunTests
    ) -> None:
        test_name = result.test_name

        def test_func():
            return patched_run_unittest(test_name)

        try:
            libregrtest_single.regrtest_runner(result, test_func, runtests)
        finally:
            support.gc_collect()
            libregrtest_single.remove_testfn(test_name, runtests.verbose)

        if gc.garbage:
            support.environment_altered = True
            libregrtest_single.print_warning(
                f"{test_name} created {len(gc.garbage)} uncollectable object(s)"
            )
            libregrtest_single.GC_GARBAGE.extend(gc.garbage)
            gc.garbage.clear()

        support.reap_children()

    libregrtest_single._load_run_test = patched__load_run_test


def start_worker(
    runtests_config: libregrtest_runtests.RunTests, worker_timeout: int
) -> WorkSender:
    dispatcher_read, worker_write = os.pipe()
    worker_read, dispatcher_write = os.pipe()
    for fd in (dispatcher_read, worker_write, worker_read, dispatcher_write):
        os.set_inheritable(fd, True)

    pipe = MessagePipe(dispatcher_read, dispatcher_write)
    config_file = tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", delete=False
    )
    try:
        json.dump(dataclasses.asdict(runtests_config), config_file)
        config_file.close()
        command = [
            sys.executable,
            *current_jit_xargs(),
            str(WORKER_PATH),
            "worker",
            str(worker_read),
            str(worker_write),
            config_file.name,
        ]
        if worker_timeout > 0 and os.name != "nt":
            command = ["timeout", "--foreground", f"{worker_timeout}s", *command]
        popen = subprocess.Popen(
            command,
            pass_fds=(worker_read, worker_write),
            cwd=os_helper.SAVEDCWD,
            env=os.environ.copy(),
        )
    finally:
        os.close(worker_read)
        os.close(worker_write)

    return WorkSender(pipe, popen)


def manage_worker(
    runtests_config: libregrtest_runtests.RunTests,
    testq: queue.Queue[Message],
    resultq: queue.Queue[Message],
    worker_timeout: int,
    worker_respawn_interval: int,
) -> None:
    worker = start_worker(runtests_config, worker_timeout)
    result: Message | None = None
    while not isinstance(result, WorkerDone):
        msg = testq.get()
        if isinstance(msg, RunTest):
            resultq.put(TestStarted(worker.pid, msg.test_name, worker.test_log.path))
        try:
            worker.send(msg)
            result = worker.recv()
        except (BrokenPipeError, EOFError):
            if isinstance(msg, ShutdownWorker):
                resultq.put(WorkerDone())
                break
            if isinstance(msg, RunTest):
                test_result = libregrtest_result.TestResult(
                    msg.test_name, state=libregrtest_result.State.WORKER_FAILED
                )
                result = TestComplete(msg.test_name, test_result)
                resultq.put(result)
                worker.wait()
                worker = start_worker(runtests_config, worker_timeout)
        else:
            resultq.put(result)
            if (
                isinstance(result, TestComplete)
                and worker.ncompleted >= worker_respawn_interval
            ):
                worker.shutdown()
                worker = start_worker(runtests_config, worker_timeout)
    worker.wait()


class Dispatcher:
    def __init__(
        self,
        *,
        tests: Iterable[str],
        ignore_patterns: Iterable[str],
        num_workers: int,
        worker_timeout: int,
        worker_respawn_interval: int,
        json_summary_file: Path,
    ) -> None:
        self._tests = tuple(tests)
        self._num_workers = num_workers
        self._worker_timeout = worker_timeout
        self._worker_respawn_interval = worker_respawn_interval
        self._json_summary_file = json_summary_file
        self._results = libregrtest_results.TestResults()
        self._logger = libregrtest_logger.Logger(self._results, False, False)
        self._worker_runs: dict[int, list[str]] = {}

        extra_opts = {}
        if sys.version_info >= (3, 14):
            extra_opts["coverage"] = False
            extra_opts["parallel_threads"] = num_workers

        self._runtests_config = libregrtest_runtests.RunTests(
            tests=self._tests,
            fail_fast=False,
            fail_env_changed=True,
            match_tests=[(pattern, False) for pattern in ignore_patterns],
            match_tests_dict={},
            rerun=False,
            forever=False,
            pgo=False,
            pgo_extended=False,
            output_on_failure=True,
            timeout=float(worker_timeout) if worker_timeout else None,
            verbose=0,
            quiet=False,
            hunt_refleak=None,
            test_dir=None,
            use_junit=False,
            memory_limit=None,
            gc_threshold=None,
            use_resources={},
            python_cmd=None,
            randomize=False,
            random_seed=1,
            **extra_opts,
        )

    def _run_tests_with_n_workers(self, tests: Iterable[str], n_workers: int) -> None:
        runtests_config = self._runtests_config.copy(tests=())
        resultq: queue.Queue[Message] = queue.Queue()
        testq: queue.Queue[Message] = queue.Queue()
        remaining = 0
        for test in tests:
            remaining += 1
            testq.put(RunTest(test))
        for _ in range(n_workers):
            testq.put(ShutdownWorker())

        threads = [
            threading.Thread(
                target=manage_worker,
                args=(
                    runtests_config,
                    testq,
                    resultq,
                    self._worker_timeout,
                    self._worker_respawn_interval,
                ),
            )
            for _ in range(n_workers)
        ]
        for thread in threads:
            thread.start()

        active_tests: dict[str, tuple[int, float, str]] = {}
        while remaining:
            if not any(thread.is_alive() for thread in threads):
                raise RuntimeError("no dispatcher workers alive with tests remaining")
            try:
                msg = resultq.get(timeout=10)
            except queue.Empty:
                self._print_running_tests(active_tests)
                continue
            if isinstance(msg, TestStarted):
                active_tests[msg.test_name] = (
                    msg.worker_pid,
                    time.monotonic(),
                    msg.test_log,
                )
                self._worker_runs.setdefault(msg.worker_pid, []).append(msg.test_name)
                continue
            if isinstance(msg, TestComplete):
                remaining -= 1
                result = msg.result
                self._results.accumulate_result(result, self._runtests_config)
                self._logger.display_progress(
                    len(self._results.get_executed()), msg.test_name
                )
                active_tests.pop(msg.test_name, None)

        for thread in threads:
            thread.join()

    def _print_running_tests(
        self, active_tests: dict[str, tuple[int, float, str]]
    ) -> None:
        if not active_tests:
            print("No tests running", flush=True)
            return
        now = time.monotonic()
        parts = ["Running tests:"]
        for test in sorted(active_tests):
            pid, started, _log = active_tests[test]
            parts.append(f"{test} ({int(now - started)}s, pid {pid})")
        print(" ".join(parts), flush=True)

    def run(self) -> int:
        self._logger.set_tests(self._runtests_config)
        self._run_tests_with_n_workers(self._tests, self._num_workers)

        if self._results.need_rerun():
            rerun_tests, rerun_match_tests_dict = self._results.prepare_rerun()
            print()
            self._logger.log(f"Re-running {len(rerun_tests)} failed tests serially.")
            self._runtests_config = self._runtests_config.copy(
                tests=rerun_tests,
                rerun=True,
                verbose=True,
                forever=False,
                fail_fast=False,
                match_tests_dict=rerun_match_tests_dict,
                output_on_failure=False,
            )
            self._logger.set_tests(self._runtests_config)
            self._run_tests_with_n_workers(rerun_tests, 1)

        self._results.display_result(self._tests, False, False)
        self._write_summary()
        if self._results.no_tests_run():
            return 0
        return self._results.get_exitcode(False, False)

    def _write_summary(self) -> None:
        payload = {
            "bad": sorted(self._results.bad),
            "good": sorted(self._results.good),
            "skipped": sorted(self._results.skipped),
            "resource_denied": sorted(self._results.resource_denied),
            "run_no_tests": sorted(self._results.run_no_tests),
            "worker_respawn_interval": self._worker_respawn_interval,
            "num_workers": self._num_workers,
            "worker_process_count": len(self._worker_runs),
            "worker_runs": {
                str(pid): tests for pid, tests in sorted(self._worker_runs.items())
            },
        }
        self._json_summary_file.write_text(
            json.dumps(payload, indent=2) + "\n", encoding="utf-8"
        )


def worker_main(args: argparse.Namespace) -> int:
    patch_libregrtest_to_use_loadTestsFromName()
    libregrtest_setup.setup_process()
    with open(args.runtest_config_json_file, encoding="utf-8") as f:
        worker_runtests_dict = json.load(f)
    os.unlink(args.runtest_config_json_file)
    worker_runtests = libregrtest_runtests.RunTests(**worker_runtests_dict)
    with MessagePipe(args.cmd_fd, args.result_fd) as pipe:
        with os_helper.temp_cwd(name=f"{tempfile.gettempdir()}/testgate-worker-{os.getpid()}"):
            msg = pipe.recv()
            while not isinstance(msg, ShutdownWorker):
                if isinstance(msg, RunTest):
                    result = libregrtest_single.run_single_test(
                        msg.test_name, worker_runtests
                    )
                    pipe.send(TestComplete(msg.test_name, result))
                msg = pipe.recv()
            pipe.send(WorkerDone())
    return 0


def dispatcher_main(args: argparse.Namespace) -> int:
    patch_libregrtest_to_use_loadTestsFromName()
    libregrtest_setup.setup_process()
    tests = read_lines(Path(args.test_list_file))
    ignore_patterns = read_lines(Path(args.ignorefile)) if args.ignorefile else []
    dispatcher = Dispatcher(
        tests=tests,
        ignore_patterns=ignore_patterns,
        num_workers=parse_num_workers(args.num_workers),
        worker_timeout=args.worker_timeout,
        worker_respawn_interval=args.worker_respawn_interval,
        json_summary_file=Path(args.json_summary_file),
    )
    return dispatcher.run()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command")

    worker_parser = subparsers.add_parser("worker")
    worker_parser.add_argument("cmd_fd", type=int)
    worker_parser.add_argument("result_fd", type=int)
    worker_parser.add_argument("runtest_config_json_file")
    worker_parser.set_defaults(func=worker_main)

    dispatcher_parser = subparsers.add_parser("dispatcher")
    dispatcher_parser.add_argument("--test-list-file", required=True)
    dispatcher_parser.add_argument("--ignorefile")
    dispatcher_parser.add_argument("--json-summary-file", required=True)
    dispatcher_parser.add_argument("--num-workers", default="auto")
    dispatcher_parser.add_argument("--worker-timeout", type=int, default=20 * 60)
    dispatcher_parser.add_argument(
        "--worker-respawn-interval", type=int, default=WORKER_RESPAWN_INTERVAL
    )
    dispatcher_parser.set_defaults(func=dispatcher_main)

    args = parser.parse_args(argv)
    if not hasattr(args, "func"):
        parser.error("missing command")
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
