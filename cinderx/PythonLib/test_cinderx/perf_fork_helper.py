#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.

import multiprocessing
import os
from pathlib import Path
import sys

import cinderx.jit


def compute():
    n = 0
    for i in range(10):
        n += i
    return n


def child1():
    print(f"child1({os.getpid()}) computed {compute()}")


def child2():
    print(f"child2({os.getpid()}) computed {compute()}")


def parent():
    print(f"parent({os.getpid()}) computed {compute()}")


def main():
    queue = multiprocessing.Queue(2)

    pid1 = os.fork()
    if pid1 == 0:
        queue.put("child1", False)
        sys.exit(child1())

    pid2 = os.fork()
    if pid2 == 0:
        queue.put("child2", False)
        sys.exit(child2())

    read = {queue.get(True, timeout=2), queue.get(True, timeout=2)}
    if read != {"child1", "child2"}:
        raise RuntimeError(f"Unexpected message queue contents: {read}")

    parent()
    os.waitpid(pid1, 0)
    os.waitpid(pid2, 0)


@cinderx.jit.jit_suppress
def schedule_compilation() -> None:
    """
    Set up functions to be compiled when they're first called.  Processes that don't
    call them will leave them uncompiled.
    """

    cinderx.jit.lazy_compile(main)
    cinderx.jit.lazy_compile(parent)
    cinderx.jit.lazy_compile(child1)
    cinderx.jit.lazy_compile(child2)
    cinderx.jit.lazy_compile(compute)


def parent_only(n):
    return n + 2


def child_only(n):
    return n + 1


def check_cp311_pid_maps():
    import cinderjit

    def compile_symbol(func):
        if not cinderjit.force_compile(func) or not cinderjit.is_jit_compiled(func):
            raise RuntimeError(f"JIT compilation failed for {func.__name__}")

    def map_path():
        return Path(f"/tmp/perf-{os.getpid()}.map")

    def check_fork(inherited, compile_in_child=False):
        pid = os.fork()
        if pid == 0:
            path = map_path()
            try:
                if path.read_bytes() != inherited:
                    raise RuntimeError("Child map lacks the inherited entries")
                if compile_in_child:
                    compile_symbol(child_only)
                    entries = path.read_bytes()
                    if not entries.startswith(inherited):
                        raise RuntimeError("Child compilation replaced the inherited entries")
                    appended = entries[len(inherited):]
                    if b"__CINDER_JIT:__main__:child_only\n" not in appended:
                        raise RuntimeError("Child map lacks the newly compiled symbol")
                    check_fork(entries)
            finally:
                path.unlink(missing_ok=True)
            os._exit(0)
        _, status = os.waitpid(pid, 0)
        if status != 0:
            raise RuntimeError(f"Child {pid} exited with wait status {status}")
        if map_path().read_bytes() != inherited:
            raise RuntimeError("Child compilation changed the parent map")

    path = map_path()
    try:
        compile_symbol(parent_only)
        entries = path.read_bytes()
        if b"__CINDER_JIT:__main__:parent_only\n" not in entries:
            raise RuntimeError("Parent map lacks the pre-fork symbol")
        check_fork(entries, compile_in_child=True)
    finally:
        path.unlink(missing_ok=True)


if __name__ == "__main__":
    if sys.argv[1:] == ["--cp311"]:
        check_cp311_pid_maps()
        sys.exit(0)
    schedule_compilation()
    sys.exit(main())
