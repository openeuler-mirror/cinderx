#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Train CPython 3.11 CinderX without third-party benchmark packages.

The management process starts one ``-S`` child process for each scenario.  A
child adds only the current build output to ``sys.path``, verifies the loaded
extension, initializes the requested JIT configuration, and runs one bounded,
deterministic workload.  Dependency installation and performance measurement
are deliberately outside this program.

The scenario structures were rewritten from repository tests and benchmarks:

* ``ci_pipeline/jit311/corpus/corpus_{operators,ic_mutation,calls}.py``
* ``ci_pipeline/jit311/corpus/corpus_{hotloops,controlflow}.py``
* ``ci_pipeline/jit311/corpus/corpus_{generators,frames,deopt_resume}.py``
* ``cinderx/benchmarks/{binary_trees,compile_time}.py``
* ``test_cinderx/test_kunpeng/test_canary_execute_311.py``

No code imports those programs, their test helpers, pyperf, or pyperformance.
"""

import argparse
import asyncio
from contextlib import contextmanager
import gc
import io
import json
import logging
import os
from pathlib import Path
import pickle
import subprocess
import sys
import time
from typing import Callable, Iterator, MutableMapping
import weakref


# The 3.11 delivery compiles with normal frames only
# (ENABLE_LIGHTWEIGHT_FRAMES is rejected at configure time), so no
# lightweight-frame flag is pinned here; doing so would fail CinderX init.
JIT_TRAINING_ENVIRONMENT = {
    "CINDERX_EVAL_MODE": "cinder",
    "CINDERX_JIT_MODE": "execute",
    "PYTHONJITAUTO": "2",
    "PYTHONJITGENERATOR": "1",
    "CINDERX_OSR_ENABLED": "0",
    "PYTHONHASHSEED": "0",
}
AUTO_JIT_THRESHOLD = int(JIT_TRAINING_ENVIRONMENT["PYTHONJITAUTO"])
DEFAULT_SCENARIO_TIMEOUT = 120

# These counts are fixed for reproducibility.  They are intentionally kept
# separate so calibration on the assigned host can change the amount of work
# without changing scenario behavior.
TRAINING_REPETITIONS = {
    "batch_records": 350_000,
    "call_shapes": 600_000,
    "record_processing": 220_000,
    "stdlib_boundaries": 4_000,
    "generator_pipeline": 35_000,
    "generator_cleanup": 12_000,
    "invalid_inputs": 350_000,
    "runtime_changes": 120_000,
    "startup_imports": 4,
    "asyncio_tasks": 250,
    "temporary_objects": 30_000,
    "cycle_cleanup": 4_000,
}


def configure_jit_environment(
    environ: MutableMapping[str, str] | None = None,
    modules: MutableMapping[str, object] | None = None,
) -> dict[str, str]:
    if environ is None:
        environ = os.environ
    if modules is None:
        modules = sys.modules
    if "_cinderx" in modules:
        raise RuntimeError(
            "_cinderx was imported before the training configuration was set"
        )
    environ.update(JIT_TRAINING_ENVIRONMENT)
    return dict(JIT_TRAINING_ENVIRONMENT)


def _is_within(path: Path, directory: Path) -> bool:
    try:
        path.relative_to(directory)
    except ValueError:
        return False
    return True


def verify_extension_path(module_file: str, build_lib: str) -> Path:
    extension = Path(module_file).resolve()
    expected_directory = Path(build_lib).resolve()
    if not _is_within(extension, expected_directory):
        raise RuntimeError(
            f"loaded _cinderx from {extension}, expected a file under "
            f"{expected_directory}"
        )
    return extension


def _training_probe(value: int) -> int:
    return value + 1


def initialize_jit(build_lib: str):
    if sys.version_info[:2] != (3, 11):
        raise RuntimeError(
            "the repository PGO workloads require CPython 3.11"
        )
    configure_jit_environment()
    resolved_build_lib = str(Path(build_lib).resolve())
    if resolved_build_lib not in sys.path:
        sys.path.insert(0, resolved_build_lib)

    import _cinderx
    import cinderjit
    import cinderx

    verify_extension_path(_cinderx.__file__, resolved_build_lib)
    cinderx.init()
    if not _cinderx.is_frame_evaluator_installed():
        _cinderx.install_frame_evaluator()
    if not cinderjit.is_enabled():
        raise RuntimeError("CinderX JIT is not enabled during PGO training")
    for _ in range(AUTO_JIT_THRESHOLD + 1):
        _training_probe(1)
    if not cinderjit.is_jit_compiled(_training_probe):
        raise RuntimeError(
            "CinderX JIT did not compile the training probe with "
            f"PYTHONJITAUTO={AUTO_JIT_THRESHOLD}"
        )
    return _cinderx, cinderjit


class _Record:
    def __init__(self, identity: int, quantity: int, price: float) -> None:
        self.identity = identity
        self.quantity = quantity
        self.price = price
        self.label = ""

    def update(self, amount: int) -> None:
        self.quantity += amount

    def total(self) -> float:
        return self.quantity * self.price


class _SlottedRecord:
    __slots__ = ("identity", "quantity", "price", "label")

    def __init__(self, identity: int, quantity: int, price: float) -> None:
        self.identity = identity
        self.quantity = quantity
        self.price = price
        self.label = ""

    def update(self, amount: int) -> None:
        self.quantity += amount

    def total(self) -> float:
        return self.quantity * self.price


class _PriorityRecord(_SlottedRecord):
    __slots__ = ()

    def total(self) -> float:
        return super().total() + self.quantity


def train_batch_records(repetitions: int) -> str:
    checksum = 0.0
    processed = 0
    for start in range(0, repetitions, 128):
        batch = []
        for index in range(start, min(start + 128, repetitions)):
            if index % 5 == 0:
                cls = _PriorityRecord
            elif index % 2 == 0:
                cls = _SlottedRecord
            else:
                cls = _Record
            record = cls(index, index % 11, 1.5 + index % 7)
            record.update(index % 3)
            record.label = f"record-{index}"
            batch.append(record)
        for record in batch:
            checksum += record.total() + record.identity % 13
            processed += 1
    if processed != repetitions or (repetitions and not checksum):
        raise AssertionError("record processing did not complete")
    return f"records={processed} checksum={checksum:.1f}"


def _plain_call(left: int, right: int = 3, *, scale: int = 2) -> int:
    return (left + right) * scale


class _Callable:
    def __call__(self, value: int) -> int:
        return value - 1


def _call_chain(value: int, callback: Callable[[int], int]) -> int:
    return callback(_plain_call(value, scale=1))


def _recursive_sum(value: int) -> int:
    return value if value < 2 else value + _recursive_sum(value - 1)


def train_call_shapes(repetitions: int) -> str:
    callback = _Callable()
    checksum = 0
    for index in range(repetitions):
        checksum += _plain_call(index % 17)
        checksum += _plain_call(index % 13, 4, scale=3)
        checksum += _call_chain(index % 19, callback)
        if index % 64 == 0:
            checksum += _recursive_sum(8)
    if repetitions and checksum <= 0:
        raise AssertionError("function calls produced no result")
    return f"calls={repetitions} checksum={checksum}"


def _parse_line(line: str) -> tuple[str, int, float, set[str]]:
    name, count, price, tags = line.split("|")
    return name, int(count), float(price), set(tags.split(","))


def train_record_processing(repetitions: int) -> str:
    grouped: dict[str, float] = {}
    accepted = 0
    lines = [
        f"item-{index % 23}|{index % 31}|{1.25 + index % 9}|a,b,{index % 5}"
        for index in range(96)
    ]
    lines.extend(
        [
            "empty|0|0.0|",
            "negative|-3|-1.5|a,edge",
            f"large|{2 ** 40}|2.0|a,large",
        ]
    )
    for index in range(repetitions):
        name, count, price, tags = _parse_line(lines[index % len(lines)])
        if count % 3 and "a" in tags:
            grouped[name] = grouped.get(name, 0.0) + count * price
            accepted += 1
        if index % 257 == 0:
            sorted(grouped.items(), key=lambda item: (item[1], item[0]))[-5:]
    sample = tuple(grouped)[::3]
    if repetitions and (accepted == 0 or not sample):
        raise AssertionError("record parsing and grouping produced no output")
    return f"records={repetitions} accepted={accepted} groups={len(grouped)}"


def train_stdlib_boundaries(repetitions: int) -> str:
    value = {"items": list(range(20)), "name": "cinderx", "active": True}
    encoded_json = json.dumps(value, sort_keys=True)
    encoded_pickle = pickle.dumps(value, protocol=pickle.HIGHEST_PROTOCOL)
    stream = io.StringIO()
    handler = logging.StreamHandler(stream)
    handler.setFormatter(logging.Formatter("%(levelname)s:%(message)s"))
    logger = logging.getLogger("cinderx-pgo-workload")
    logger.handlers[:] = [handler]
    logger.propagate = False
    logger.setLevel(logging.INFO)
    checksum = 0
    for index in range(repetitions):
        checksum += len(json.loads(encoded_json)["items"])
        checksum += len(pickle.loads(encoded_pickle)["items"])
        if index % 32 == 0:
            logger.info("batch %d", index)
    handler.flush()
    if "INFO:batch" not in stream.getvalue() or checksum != repetitions * 40:
        raise AssertionError("standard-library conversions produced wrong results")
    logger.handlers.clear()
    return f"round_trips={repetitions} checksum={checksum}"


def _pipeline_step(value: int) -> int:
    return value * 2 if value % 3 else 0


def _source_values(count: int) -> Iterator[int]:
    for value in range(count):
        yield value


def _transformed(values: Iterator[int]) -> Iterator[int]:
    for value in values:
        stepped = _pipeline_step(value)
        if stepped:
            yield stepped


def _delegated(count: int) -> Iterator[int]:
    yield from _transformed(_source_values(count))


def _returning_source(count: int):
    for value in range(count):
        yield value
    return count


def _capture_generator_return(count: int):
    returned = yield from _returning_source(count)
    yield returned


def train_generator_pipeline(repetitions: int) -> str:
    expected = sum(value * 2 for value in range(24) if value % 3)
    checksum = 0
    consumed = 0
    for _ in range(repetitions):
        for value in _delegated(24):
            checksum += value
            consumed += 1
    if checksum != repetitions * expected or consumed != repetitions * 16:
        raise AssertionError("generator pipeline was not fully consumed")
    left = _delegated(6)
    right = _delegated(6)
    alternating = [next(left), next(right), next(left), next(right)]
    if alternating != [2, 2, 4, 4]:
        raise AssertionError("alternating generators resumed in the wrong order")
    if list(_capture_generator_return(4)) != [0, 1, 2, 3, 4]:
        raise AssertionError("yield-from lost the child generator return value")
    return f"pipelines={repetitions} values={consumed}"


@contextmanager
def _cleanup_context(events: list[str]):
    try:
        yield
    finally:
        events.append("context")


def train_generator_cleanup(repetitions: int) -> str:
    events: list[str] = []

    def child():
        try:
            while True:
                try:
                    yield 1
                except ValueError:
                    events.append("throw")
        finally:
            events.append("child")

    def parent():
        with _cleanup_context(events):
            yield from child()

    for index in range(repetitions):
        generator = parent()
        next(generator)
        if index % 2:
            generator.throw(ValueError)
        generator.close()
    expected = repetitions * 2 + repetitions // 2
    if len(events) != expected or events.count("context") != repetitions:
        raise AssertionError("generator cleanup did not run all finalizers")
    return f"generators={repetitions} cleanup_events={len(events)}"


class _InputError(Exception):
    pass


def _convert_value(value: object) -> int:
    if value == "custom":
        raise _InputError("custom input")
    return int(value)


def train_invalid_inputs(repetitions: int) -> str:
    values: list[object] = ["7", 9, "bad", None, "custom"]
    caught = 0
    checksum = 0
    for index in range(repetitions):
        value = values[index % len(values)]
        try:
            converted = _convert_value(value)
            checksum += 100 // (converted if index % 41 else 0)
            [converted][index % 7]
            {"value": converted}["value" if index % 43 else "missing"]
        except (
            ValueError,
            TypeError,
            ZeroDivisionError,
            IndexError,
            KeyError,
            _InputError,
        ):
            caught += 1
    if repetitions and (caught == 0 or checksum == 0):
        raise AssertionError("invalid inputs did not exercise recovery paths")
    return f"inputs={repetitions} exceptions={caught} checksum={checksum}"


def _runtime_add(value):
    return value + 1


class _ChangingMethod:
    def transform(self, value: int) -> int:
        return value + 2


def train_runtime_changes(repetitions: int) -> str:
    instance = _ChangingMethod()
    first = sum(
        _runtime_add(index) + instance.transform(index)
        for index in range(repetitions)
    )
    original = _ChangingMethod.transform
    _ChangingMethod.transform = lambda self, value: value * 2
    try:
        second = sum(
            _runtime_add(index + 0.5) + instance.transform(index)
            for index in range(repetitions // 4)
        )
    finally:
        _ChangingMethod.transform = original
    if repetitions and (first <= 0 or second <= 0):
        raise AssertionError("changed runtime conditions produced no result")
    return f"iterations={repetitions} checksum={first + second:.1f}"


def _startup_code(build_lib: str) -> str:
    return f"""
import os, sys
sys.path.insert(0, {build_lib!r})
import _cinderx, cinderx
from pathlib import Path
extension = Path(_cinderx.__file__).resolve()
extension.relative_to(Path({build_lib!r}).resolve())
cinderx.init()
if not _cinderx.is_frame_evaluator_installed():
    _cinderx.install_frame_evaluator()
import csv, difflib, fractions, statistics, textwrap
assert statistics.mean([1, 2, 3, 4]) == 2.5
print(extension)
"""


def train_startup_imports(
    repetitions: int,
    build_lib: str | None = None,
    timeout: int = DEFAULT_SCENARIO_TIMEOUT,
) -> str:
    if build_lib is None:
        # Unit tests cover the deterministic import work without requiring a
        # built extension.  Formal training always supplies build_lib.
        for _ in range(repetitions):
            __import__("csv")
            __import__("fractions")
        return f"starts={repetitions}"
    for _ in range(repetitions):
        subprocess.run(
            [sys.executable, "-S", "-c", _startup_code(build_lib)],
            check=True,
            capture_output=True,
            text=True,
            # Honor the configurable per-scenario timeout instead of a fixed
            # constant so grandchild startups follow the same budget as their
            # parent scenario.
            timeout=timeout,
            env=os.environ.copy(),
        )
    return f"starts={repetitions}"


async def _async_unit(value: int, delay: bool = False) -> int:
    if delay:
        await asyncio.sleep(0)
    return value * 2


async def _async_batch(size: int) -> tuple[int, int]:
    tasks = [
        asyncio.create_task(_async_unit(index, index % 3 == 0))
        for index in range(size)
    ]
    cancelled = asyncio.create_task(asyncio.sleep(10))
    cancelled.cancel()
    try:
        await cancelled
    except asyncio.CancelledError:
        cancelled_count = 1
    values = await asyncio.gather(*tasks)
    return sum(values), cancelled_count


def train_asyncio_tasks(repetitions: int) -> str:
    checksum = 0
    cancelled = 0
    for _ in range(repetitions):
        subtotal, count = asyncio.run(_async_batch(12))
        checksum += subtotal
        cancelled += count
    if checksum != repetitions * 132 or cancelled != repetitions:
        raise AssertionError("async tasks did not complete or cancel as expected")
    return f"runs={repetitions} cancelled={cancelled}"


def _temporary_checksum(seed: int) -> int:
    values = [(seed + offset, str(seed + offset)) for offset in range(32)]
    return sum(value for value, _text in values)


def train_temporary_objects(repetitions: int) -> str:
    checksum = 0
    for start in range(0, repetitions, 64):
        batch = [
            _temporary_checksum(index)
            for index in range(start, min(start + 64, repetitions))
        ]
        checksum += sum(batch)
        del batch
    if repetitions and checksum <= 0:
        raise AssertionError("temporary objects produced no result")
    return f"batches={(repetitions + 63) // 64} checksum={checksum}"


class _Cycle:
    __slots__ = ("other", "__weakref__")


def _cycle_touch(value: int) -> int:
    return value + 1


def train_cycle_cleanup(repetitions: int) -> str:
    references: list[weakref.ReferenceType[_Cycle]] = []
    touched = 0
    for index in range(repetitions):
        touched += _cycle_touch(index)
        left = _Cycle()
        right = _Cycle()
        left.other = right
        right.other = left
        references.append(weakref.ref(left))
        del left, right
        if len(references) >= 128:
            gc.collect()
            if any(reference() is not None for reference in references):
                raise AssertionError("cycle batch remained reachable")
            references.clear()
    gc.collect()
    if repetitions and touched <= 0:
        raise AssertionError("cycle_cleanup helper produced no result")
    if any(reference() is not None for reference in references):
        raise AssertionError("the final cycle batch remained reachable")

    events: list[str] = []

    def suspended():
        try:
            yield object()
        finally:
            events.append("generator")

    generator = suspended()
    next(generator)
    generator_reference = weakref.ref(generator)
    del generator
    gc.collect()
    if generator_reference() is not None or events != ["generator"]:
        raise AssertionError("suspended generator was not released")
    return f"cycles={repetitions}"


SCENARIOS: dict[str, Callable[..., str]] = {
    "batch_records": train_batch_records,
    "call_shapes": train_call_shapes,
    "record_processing": train_record_processing,
    "stdlib_boundaries": train_stdlib_boundaries,
    "generator_pipeline": train_generator_pipeline,
    "generator_cleanup": train_generator_cleanup,
    "invalid_inputs": train_invalid_inputs,
    "runtime_changes": train_runtime_changes,
    "startup_imports": train_startup_imports,
    "asyncio_tasks": train_asyncio_tasks,
    "temporary_objects": train_temporary_objects,
    "cycle_cleanup": train_cycle_cleanup,
}

JIT_TARGETS = {
    "call_shapes": _plain_call,
    "record_processing": _parse_line,
    "generator_pipeline": _pipeline_step,
    "invalid_inputs": _convert_value,
    "runtime_changes": _runtime_add,
    "temporary_objects": _temporary_checksum,
    "cycle_cleanup": _cycle_touch,
}


def validate_repetitions(repetitions: dict[str, int]) -> None:
    missing = sorted(set(SCENARIOS) - set(repetitions))
    extra = sorted(set(repetitions) - set(SCENARIOS))
    nonpositive = sorted(name for name, count in repetitions.items() if count <= 0)
    if missing or extra or nonpositive:
        raise ValueError(
            f"invalid training counts: missing={missing}, extra={extra}, "
            f"nonpositive={nonpositive}"
        )


def total_training_timeout(scenario_timeout: int = DEFAULT_SCENARIO_TIMEOUT) -> int:
    """Worst-case wall clock for running every scenario serially.

    Each scenario runs as its own child process bounded by ``scenario_timeout``,
    so the whole trainer can legitimately take ``len(SCENARIOS) * scenario_timeout``
    seconds.  Callers that wrap the trainer with an outer timeout (for example
    ``setup.py``) must budget at least this much, otherwise a healthy run on a
    slow or loaded host can be killed before it finishes.
    """
    if scenario_timeout <= 0:
        raise ValueError("scenario timeout must be positive")
    return len(SCENARIOS) * scenario_timeout


def run_scenario(
    name: str,
    repetitions: int,
    build_lib: str | None = None,
    scenario_timeout: int = DEFAULT_SCENARIO_TIMEOUT,
) -> str:
    if name not in SCENARIOS:
        raise ValueError(f"unknown training scenario: {name}")
    if repetitions <= 0:
        raise ValueError(f"training count must be positive for {name}")
    workload = SCENARIOS[name]
    if name == "startup_imports":
        return workload(repetitions, build_lib, scenario_timeout)
    return workload(repetitions)


def peak_rss_mib() -> float | None:
    try:
        import resource
    except ImportError:
        return None
    peak = max(
        resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
        resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss,
    )
    # Linux reports KiB. macOS reports bytes; Linux is the supported build
    # environment for this trainer.
    return peak / 1024


def _warmup_jit_target(name: str, target: Callable[..., object]) -> None:
    for _ in range(AUTO_JIT_THRESHOLD + 1):
        if name == "record_processing":
            target("item|1|1.0|a")
        elif name == "invalid_inputs":
            target("7")
        elif name == "call_shapes":
            target(1, 2, scale=3)
        else:
            target(1)


def _run_child(
    name: str,
    repetitions: int,
    build_lib: str,
    scenario_timeout: int = DEFAULT_SCENARIO_TIMEOUT,
) -> None:
    _cinderx, cinderjit = initialize_jit(build_lib)
    before = _cinderx._get_trigger_stats()["machine_code_entries"]
    deopt_before = _cinderx._get_trigger_stats()["organic_deopt_hits"]
    target = JIT_TARGETS.get(name)
    if target is not None:
        _warmup_jit_target(name, target)
        if not cinderjit.is_jit_compiled(target):
            raise RuntimeError(f"JIT did not compile the target function for {name}")
    started = time.perf_counter()
    detail = run_scenario(name, repetitions, build_lib, scenario_timeout)
    elapsed = time.perf_counter() - started

    after = _cinderx._get_trigger_stats()["machine_code_entries"]
    if name not in {"startup_imports", "asyncio_tasks"} and after <= before:
        raise RuntimeError(f"scenario {name} did not enter newly compiled code")
    if name == "runtime_changes":
        deopt_after = _cinderx._get_trigger_stats()["organic_deopt_hits"]
        if deopt_after <= deopt_before:
            raise RuntimeError("runtime_changes did not record a deoptimization")
    peak_memory = peak_rss_mib()
    memory_detail = "" if peak_memory is None else f", peak_rss={peak_memory:.1f} MiB"
    print(f"{name}: {detail} ({elapsed:.2f}s{memory_detail})")


def run_training(
    build_lib: str,
    repetitions: dict[str, int] | None = None,
    timeout: int = DEFAULT_SCENARIO_TIMEOUT,
) -> None:
    counts = dict(TRAINING_REPETITIONS if repetitions is None else repetitions)
    validate_repetitions(counts)
    script = str(Path(__file__).resolve())
    failures = []
    started = time.perf_counter()
    print(
        f"PGO training budget: {len(SCENARIOS)} scenarios x {timeout}s "
        f"= up to {total_training_timeout(timeout)}s wall clock"
    )
    for name in SCENARIOS:
        command = [
            sys.executable,
            "-S",
            script,
            "--scenario",
            name,
            "--repetitions",
            str(counts[name]),
            "--build-lib",
            str(Path(build_lib).resolve()),
            "--timeout",
            str(timeout),
        ]
        try:
            result = subprocess.run(
                command,
                check=True,
                capture_output=True,
                text=True,
                timeout=timeout,
                env=os.environ.copy(),
            )
        except subprocess.CalledProcessError as error:
            detail = (error.stderr or error.stdout or str(error)).strip()
            failures.append(f"{name}: {detail}")
        except subprocess.TimeoutExpired as error:
            failures.append(f"{name}: {error}")
        else:
            print(result.stdout.strip())
    if failures:
        raise RuntimeError("PGO training failed:\n" + "\n".join(failures))
    print(f"PGO training completed in {time.perf_counter() - started:.2f}s")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-lib", required=True)
    parser.add_argument("--scenario", choices=tuple(SCENARIOS))
    parser.add_argument("--repetitions", type=int)
    parser.add_argument(
        "--timeout",
        type=int,
        default=int(
            os.environ.get(
                "CINDERX_PGO_SCENARIO_TIMEOUT", DEFAULT_SCENARIO_TIMEOUT
            )
        ),
    )
    return parser.parse_args()


def main() -> None:
    arguments = parse_args()
    if arguments.scenario is None:
        run_training(arguments.build_lib, timeout=arguments.timeout)
    else:
        if arguments.repetitions is None:
            raise SystemExit("--repetitions is required with --scenario")
        _run_child(
            arguments.scenario,
            arguments.repetitions,
            arguments.build_lib,
            arguments.timeout,
        )


if __name__ == "__main__":
    main()
