# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# pyre-strict

from contextlib import contextmanager
from typing import Any, AsyncGenerator, Callable, Coroutine, Generator, TypeVar
from warnings import catch_warnings, simplefilter, warn


# The JIT compiles arbitrary Python functions.  Ideally this type would exclude native
# functions, but that doesn't seem possible yet.
#
FuncAny = Callable[..., Any]


_CINDERJIT_NAMES = (
    "_deopt_gen",
    "append_jit_list",
    "auto",
    "clear_runtime_stats",
    "compile_after_n_calls",
    "count_interpreted_calls",
    "disable",
    "disable_emit_type_annotation_guards",
    "disable_hir_inliner",
    "disable_specialized_opcodes",
    "disassemble",
    "enable",
    "enable_emit_type_annotation_guards",
    "enable_hir_inliner",
    "enable_specialized_opcodes",
    "force_compile",
    "force_uncompile",
    "get_allocator_stats",
    "get_and_clear_inline_cache_stats",
    "get_and_clear_runtime_stats",
    "get_compilation_time",
    "get_compile_after_n_calls",
    "get_compiled_functions",
    "get_compiled_size",
    "get_compiled_spill_stack_size",
    "get_compiled_stack_size",
    "get_function_compilation_time",
    "get_function_hir_opcode_counts",
    "get_inlined_functions_stats",
    "get_jit_list",
    "get_num_inlined_functions",
    "is_enabled",
    "is_hir_inliner_enabled",
    "is_inline_cache_stats_collection_enabled",
    "is_jit_compiled",
    "is_lightweight_frames_enabled",
    "jit_suppress",
    "jit_unsuppress",
    "lazy_compile",
    "mlock_profiler_dependencies",
    "multithreaded_compile_test",
    "page_in_profiler_dependencies",
    "precompile_all",
    "read_jit_list",
    "set_max_code_size",
)

try:
    import cinderjit as _CINDERJIT_API
except ImportError:
    _CINDERJIT_API = None

try:
    from cinderjit import (
        _deopt_gen,
        append_jit_list,
        auto,
        clear_runtime_stats,
        compile_after_n_calls,
        count_interpreted_calls,
        disable,
        disable_emit_type_annotation_guards,
        disable_hir_inliner,
        disable_specialized_opcodes,
        disassemble,
        enable,
        enable_emit_type_annotation_guards,
        enable_hir_inliner,
        enable_specialized_opcodes,
        force_compile,
        force_uncompile,
        get_allocator_stats,
        get_and_clear_inline_cache_stats,
        get_and_clear_runtime_stats,
        get_compilation_time,
        get_compile_after_n_calls,
        get_compiled_functions,
        get_compiled_size,
        get_compiled_spill_stack_size,
        get_compiled_stack_size,
        get_function_compilation_time,
        get_function_hir_opcode_counts,
        get_inlined_functions_stats,
        get_jit_list,
        get_num_inlined_functions,
        is_enabled,
        is_hir_inliner_enabled,
        is_lightweight_frames_enabled,
        is_inline_cache_stats_collection_enabled,
        is_jit_compiled,
        jit_suppress,
        jit_unsuppress,
        lazy_compile,
        mlock_profiler_dependencies,
        multithreaded_compile_test,
        page_in_profiler_dependencies,
        precompile_all,
        read_jit_list,
        set_max_code_size,
    )

except ImportError:
    TDeoptGenYield = TypeVar("TDeoptGenYield")
    TDeoptGenSend = TypeVar("TDeoptGenSend")
    TDeoptGenReturn = TypeVar("TDeoptGenReturn")

    def _deopt_gen(
        gen: Generator[TDeoptGenYield, TDeoptGenSend, TDeoptGenReturn]
        | AsyncGenerator[TDeoptGenYield, TDeoptGenSend]
        | Coroutine[TDeoptGenYield, TDeoptGenSend, TDeoptGenReturn],
    ) -> bool:
        return False

    def append_jit_list(entry: str) -> None:
        return None

    def auto() -> None:
        return None

    def clear_runtime_stats() -> None:
        return None

    def compile_after_n_calls(calls: int) -> None:
        return None

    def count_interpreted_calls(func: FuncAny) -> int:
        return 0

    def disable(deopt_all: bool = False) -> None:
        return None

    def disable_emit_type_annotation_guards() -> None:
        return None

    def disable_hir_inliner() -> None:
        return None

    def disable_specialized_opcodes() -> None:
        return None

    def disassemble(func: FuncAny) -> None:
        return None

    def enable() -> None:
        # Warn here because users might think this function is how to enable the JIT
        # when it is not installed.
        warn(
            "Cinder JIT is not installed, calling cinderx.jit.enable() is doing nothing"
        )

    def enable_emit_type_annotation_guards() -> None:
        return None

    def enable_hir_inliner() -> None:
        return None

    def enable_specialized_opcodes() -> None:
        return None

    def force_compile(func: FuncAny) -> bool:
        return False

    def force_uncompile(func: FuncAny) -> bool:
        return False

    def get_allocator_stats() -> dict[str, int]:
        return {}

    def get_and_clear_inline_cache_stats() -> dict[str, object]:
        return {}

    def get_and_clear_runtime_stats() -> dict[str, object]:
        return {}

    def get_compilation_time() -> int:
        return 0

    def get_compile_after_n_calls() -> int | None:
        return None

    def get_compiled_functions() -> list[FuncAny]:
        return []

    def get_compiled_size(func: FuncAny) -> int:
        return 0

    def get_compiled_spill_stack_size(func: FuncAny) -> int:
        return 0

    def get_compiled_stack_size(func: FuncAny) -> int:
        return 0

    def get_function_compilation_time(func: FuncAny) -> int:
        return 0

    def get_function_hir_opcode_counts(func: FuncAny) -> dict[str, int] | None:
        return {}

    def get_inlined_functions_stats(func: FuncAny) -> dict[str, object]:
        return {}

    def get_jit_list() -> tuple[dict[str, set[str]], dict[str, dict[str, set[int]]]]:
        return ({}, {})

    def get_num_inlined_functions(func: FuncAny) -> int:
        return 0

    def is_enabled() -> bool:
        return False

    def is_hir_inliner_enabled() -> bool:
        return False

    def is_lightweight_frames_enabled() -> bool:
        return False

    def is_inline_cache_stats_collection_enabled() -> bool:
        return False

    def is_jit_compiled(func: FuncAny) -> bool:
        return False

    def jit_suppress(func: FuncAny) -> FuncAny:
        return func

    def jit_unsuppress(func: FuncAny) -> FuncAny:
        return func

    def lazy_compile(func: FuncAny) -> bool:
        return False

    def mlock_profiler_dependencies() -> None:
        return None

    def multithreaded_compile_test() -> None:
        return None

    def page_in_profiler_dependencies() -> list[str]:
        return []

    def precompile_all(workers: int = 0) -> bool:
        return False

    def read_jit_list(path: str) -> None:
        return None

    def set_max_code_size(max_code_size: int) -> None:
        return None


# A capability-gated build publishes only the part of the control plane its
# milestone has accepted -- the CPython 3.11 canary, for one, withholds the
# batch, uncompile and speculation APIs that later milestones own.  The bulk
# import above is all-or-nothing, so on such a build every name above falls
# back to a stub and this module would report a JIT that is demonstrably
# executing machine code as absent.  Bind back whatever the module actually
# provides; the rest keep their stubs.
if _CINDERJIT_API is not None:

    def _withheld(name):
        def raise_withheld(*args, **kwargs):
            raise RuntimeError(
                f"cinderjit.{name} is not available in this build; the "
                f"stub would have reported success without doing anything"
            )

        return raise_withheld

    for _api_name in _CINDERJIT_NAMES:
        _api = getattr(_CINDERJIT_API, _api_name, None)
        if _api is not None:
            globals()[_api_name] = _api
        else:
            # A no-op stub is only honest when there is no JIT at all.  With
            # the module present, a silently-succeeding stub misrepresents a
            # withheld control API as one that worked -- which is how
            # @jit_suppress stopped suppressing.
            globals()[_api_name] = _withheld(_api_name)
    del _api_name, _api



def force_compile_cold(func) -> bool:
    """Compile a function that has never run.

    The development plan names cold and warm compilation as separate
    entry points because they exercise different inputs: cold sees the
    unquickened bytecode the compiler was written against, warm sees the
    specialized forms the interpreter rewrote in place.  Leaving the
    distinction to whether a caller happened to warm the function first
    makes it an accident; these two make it a contract, and each refuses
    rather than quietly compiling the other timing.
    """
    if _is_quickened(func):
        raise RuntimeError(
            f"{func.__qualname__} has already run: its bytecode carries "
            f"specialized forms, so this would be a warm compile"
        )
    return force_compile(func)


def force_compile_warm(func) -> bool:
    """Compile a function after the interpreter has quickened its bytecode.

    What "warm" buys today is the warmed interpreter state, not the
    specialized instructions: the executing mode compiles from the
    unspecialized forms until MR-07 lands guard and deopt metadata, because
    consuming a specialized instruction is what creates a speculative
    guard.  The entry therefore measures compilation and execution against
    a function the interpreter has already rewritten in place -- it does
    not yet feed those rewrites to the compiler.
    """
    if not _is_quickened(func):
        raise RuntimeError(
            f"{func.__qualname__} has not been specialized yet: run it "
            f"until the interpreter quickens it, or use "
            f"force_compile_cold()"
        )
    return force_compile(func)


def _is_quickened(func) -> bool:
    # The adaptive view differs from the plain one exactly where the
    # interpreter has rewritten an instruction in place.
    import dis

    code = func.__code__
    return any(
        plain.opname != adaptive.opname
        for plain, adaptive in zip(
            dis.get_instructions(code),
            dis.get_instructions(code, adaptive=True),
        )
    )


@contextmanager

def pause(deopt_all: bool = False) -> Generator[None, None, None]:
    """
    Context manager for temporarily pausing the JIT.

    This will disable the JIT from running on new functions, and if you set
    `deopt_all`, will also de-optimize all currently compiled functions to the
    interpreter.  When the JIT is unpaused, the compiled functions will be put
    back.
    """

    prev_enabled = is_enabled()
    if prev_enabled:
        disable(deopt_all=deopt_all)

    try:
        yield
    finally:
        if prev_enabled:
            # Disable the warning from enable() when the JIT is not
            # installed/initialized.
            with catch_warnings():
                simplefilter("ignore")
                enable()
