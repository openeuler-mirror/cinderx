#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

from collections.abc import Mapping
import os
import platform
import sys


def compute_py_version() -> str:
    return f"{sys.version_info.major}.{sys.version_info.minor}"


def validate_running_python() -> None:
    if sys.version_info[:2] == (3, 11) and sys.version_info[:3] != (3, 11, 6):
        running = ".".join(str(part) for part in sys.version_info[:3])
        raise RuntimeError(
            "CinderX CPython 3.11 support is pinned to exactly 3.11.6; "
            f"the build is running with {running}"
        )


def cmake_value(value: object) -> str:
    if type(value) == bool:
        value = int(value)
    if type(value) == int:
        value = str(value)
    if type(value) != str:
        raise ValueError(f"Not sure what to do with default value {value}")
    return value


def should_enable_lightweight_frames(
    py_version: str,
    *,
    meta_python: bool,
    machine: str | None = None,
) -> bool:
    if meta_python and py_version == "3.12":
        return True
    # CPython 3.11 uses the materialized _PyInterpreterFrame as its only frame
    # mode; lightweight frames stay off and CMakeLists.txt rejects turning
    # them on.
    if py_version not in {"3.14", "3.15"}:
        return False
    if machine is None:
        machine = platform.machine()
    return machine.lower() in {"aarch64", "arm64"}


def cmake_feature_options(
    *,
    py_version: str | None = None,
    python_root: str | None = None,
    env: Mapping[str, str] | None = None,
) -> dict[str, str]:
    validate_running_python()
    if env is None:
        env = os.environ
    if py_version is None:
        py_version = compute_py_version()

    meta_python = "+meta" in sys.version
    linux = sys.platform == "linux"
    mac = sys.platform == "darwin"
    meta_312 = meta_python and py_version == "3.12"
    is_314plus = py_version in {"3.14", "3.15"}
    is_stock_311 = py_version == "3.11" and not meta_python

    options: dict[str, str] = {
        "PY_VERSION": py_version,
    }
    if python_root is not None:
        options["Python_ROOT_DIR"] = python_root

    def set_option(var: str, default: object) -> None:
        options[var] = env.get(var, cmake_value(default))

    set_option("META_PYTHON", meta_python)
    set_option("ENABLE_ADAPTIVE_STATIC_PYTHON", meta_312)
    set_option("ENABLE_DISASSEMBLER", not is_stock_311)
    set_option("ENABLE_ELF_READER", linux)
    set_option("ENABLE_EVAL_HOOK", meta_312)
    set_option("ENABLE_FUNC_EVENT_MODIFY_QUALNAME", meta_312)
    set_option("ENABLE_GENERATOR_AWAITER", meta_312)
    set_option("ENABLE_INTERPRETER_LOOP", is_stock_311 or meta_312 or is_314plus)
    set_option("ENABLE_LAZY_IMPORTS", meta_312)
    set_option(
        "ENABLE_LIGHTWEIGHT_FRAMES",
        should_enable_lightweight_frames(py_version, meta_python=meta_python),
    )
    set_option("ENABLE_PARALLEL_GC", meta_312)
    set_option("ENABLE_PEP523_HOOK", is_stock_311 or meta_312 or is_314plus)
    set_option("ENABLE_PERF_TRAMPOLINE", meta_312)
    set_option("ENABLE_SYMBOLIZER", linux and not is_stock_311)
    set_option("ENABLE_USDT", linux)
    set_option("ENABLE_XXCLASSLOADER", False)
    set_option("ENABLE_ZLIB", linux or mac)

    return options
