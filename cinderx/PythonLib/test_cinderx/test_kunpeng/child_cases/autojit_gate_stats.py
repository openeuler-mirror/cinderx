import argparse


def install_fake_lib2to3_importer() -> None:
    import importlib.abc
    import importlib.machinery
    import sys

    class FakeLib2to3Loader(importlib.abc.Loader):
        def create_module(self, spec):
            return None

        def exec_module(self, module):
            if module.__name__ == "lib2to3":
                module.__path__ = []
                return
            if module.__name__ == "lib2to3.main":

                def main():
                    return 42

                module.main = main

    class FakeLib2to3Finder(importlib.abc.MetaPathFinder):
        def find_spec(self, fullname, path=None, target=None):
            if fullname == "lib2to3":
                return importlib.machinery.ModuleSpec(
                    fullname,
                    FakeLib2to3Loader(),
                    is_package=True,
                )
            if fullname == "lib2to3.main":
                return importlib.machinery.ModuleSpec(
                    fullname,
                    FakeLib2to3Loader(),
                )
            return None

    sys.meta_path.insert(0, FakeLib2to3Finder())


def import_provider_default() -> None:
    import sys

    bootstrap = sys.modules["importlib._bootstrap"]
    assert (
        getattr(
            bootstrap._find_and_load,
            "_cinderx_autojit_import_provider",
            None,
        )
        == "find_and_load"
    )


def import_provider_off() -> None:
    import sys

    bootstrap = sys.modules["importlib._bootstrap"]
    assert (
        getattr(
            bootstrap._find_and_load,
            "_cinderx_autojit_import_provider",
            None,
        )
        is None
    )


def setup_provider_default() -> None:
    install_fake_lib2to3_importer()
    import lib2to3.main

    assert (
        getattr(
            lib2to3.main.main,
            "_cinderx_autojit_setup_provider",
            None,
        )
        == "lib2to3_main"
    )


def setup_provider_off() -> None:
    install_fake_lib2to3_importer()
    import lib2to3.main

    assert (
        getattr(
            lib2to3.main.main,
            "_cinderx_autojit_setup_provider",
            None,
        )
        is None
    )


def setup_provider_for_cinderx_init() -> None:
    import sys
    import types

    import cinderx

    module = types.ModuleType("lib2to3.main")

    def main():
        return 42

    module.main = main
    sys.modules["lib2to3.main"] = module
    cinderx._maybe_install_autojit_setup_provider_for_module("lib2to3.main")
    assert (
        getattr(
            module.main,
            "_cinderx_autojit_setup_provider",
            None,
        )
        == "lib2to3_main"
    )


def multiprocessing_pool_setup_provider_default() -> None:
    import sys
    import types

    import _cinderx
    import cinderx

    observed_depths = []
    module = types.ModuleType("multiprocessing.pool")

    class Pool:
        __module__ = "multiprocessing.pool"

        def __enter__(self):
            observed_depths.append(_cinderx._autojit_setup_depth())
            return self

        def __exit__(self, *exc):
            observed_depths.append(_cinderx._autojit_setup_depth())

        def imap(self, *args):
            observed_depths.append(_cinderx._autojit_setup_depth())
            return iter(())

    class ThreadPool(Pool):
        __module__ = "multiprocessing.pool"

    class IMapIterator:
        def next(self):
            return None

        __next__ = next

    module.Pool = Pool
    module.IMapIterator = IMapIterator
    sys.modules["multiprocessing.pool"] = module

    cinderx._maybe_install_autojit_setup_provider_for_module(
        "multiprocessing.pool"
    )

    assert (
        getattr(
            module.Pool.__enter__,
            "_cinderx_autojit_setup_provider",
            None,
        )
        == "multiprocessing_pool"
    )
    assert (
        getattr(
            module.Pool.imap,
            "_cinderx_autojit_setup_provider",
            None,
        )
        == "multiprocessing_pool"
    )
    assert (
        getattr(
            module.IMapIterator.next,
            "_cinderx_autojit_setup_provider",
            None,
        )
        is None
    )
    with module.Pool():
        observed_depths.append(_cinderx._autojit_setup_depth())

    assert observed_depths == [1, 1, 1], observed_depths
    assert _cinderx._autojit_setup_depth() == 0

    observed_depths.clear()
    list(module.Pool().imap(None, ()))
    assert observed_depths == [1], observed_depths
    assert _cinderx._autojit_setup_depth() == 0

    observed_depths.clear()
    with ThreadPool():
        observed_depths.append(_cinderx._autojit_setup_depth())

    assert observed_depths == [0, 0, 0], observed_depths
    assert _cinderx._autojit_setup_depth() == 0

    observed_depths.clear()
    list(ThreadPool().imap(None, ()))
    assert observed_depths == [0], observed_depths
    assert _cinderx._autojit_setup_depth() == 0


def multiprocessing_pool_setup_provider_off() -> None:
    import sys
    import types

    import cinderx

    module = types.ModuleType("multiprocessing.pool")

    class Pool:
        __module__ = "multiprocessing.pool"

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            pass

        def imap(self, *args):
            return iter(())

    module.Pool = Pool
    sys.modules["multiprocessing.pool"] = module

    cinderx._maybe_install_autojit_setup_provider_for_module(
        "multiprocessing.pool"
    )

    assert (
        getattr(
            module.Pool.__enter__,
            "_cinderx_autojit_setup_provider",
            None,
        )
        is None
    )
    assert (
        getattr(
            module.Pool.imap,
            "_cinderx_autojit_setup_provider",
            None,
        )
        is None
    )


def gate_stats_smoke() -> None:
    def target(x):
        return x + 1

    for i in range(5):
        target(i)


def numeric_loop(value):
    total = 0
    for _ in range(8):
        total += value
    return total


def warm_numeric_loop_until_compiled():
    import cinderx.jit as jit

    for _ in range(120):
        numeric_loop(1)

    assert jit.is_jit_compiled(numeric_loop), (
        jit.count_interpreted_calls(numeric_loop)
    )
    return jit


def roi_backoff_uncompile() -> None:
    import cinderjit

    cinderjit._clear_autojit_gate_stats()
    jit = warm_numeric_loop_until_compiled()

    for _ in range(32):
        numeric_loop(1.5)

    stats = cinderjit._autojit_gate_stats()
    assert stats["roi_uncompile"] >= 1, stats
    assert stats["roi_recompile"] == 0, stats
    assert stats["roi_frozen"] == 0, stats
    assert not jit.is_jit_compiled(numeric_loop), stats


def roi_backoff_default_freeze() -> None:
    import cinderjit

    cinderjit._clear_autojit_gate_stats()
    jit = warm_numeric_loop_until_compiled()

    for _ in range(64):
        numeric_loop(1.5)

    stats = cinderjit._autojit_gate_stats()
    assert stats["roi_frozen"] >= 1, stats
    assert stats["roi_uncompile"] == 0, stats
    assert stats["roi_recompile"] == 0, stats
    assert not jit.is_jit_compiled(numeric_loop), stats


def roi_backoff_disabled() -> None:
    import cinderjit

    cinderjit._clear_autojit_gate_stats()
    jit = warm_numeric_loop_until_compiled()

    for _ in range(64):
        numeric_loop(1.5)

    stats = cinderjit._autojit_gate_stats()
    assert stats["roi_frozen"] == 0, stats
    assert stats["roi_uncompile"] == 0, stats
    assert stats["roi_recompile"] == 0, stats
    assert jit.is_jit_compiled(numeric_loop), stats


def compiles_low_roi_functions_at_base() -> None:
    import cinderjit
    import cinderx.jit as jit

    cinderjit._clear_autojit_gate_stats()

    def trivial(value):
        return value

    for value in range(20):
        trivial(value)

    trivial_stats = cinderjit._autojit_gate_stats()
    assert trivial_stats["classified_defer_freeze"] == 0, trivial_stats
    assert jit.is_jit_compiled(trivial), trivial_stats

    cinderjit._clear_autojit_gate_stats()

    def identity(value):
        return value

    def dispatch(func, value):
        return func(value)

    for value in range(2000):
        dispatch(identity, value)

    dispatch_stats = cinderjit._autojit_gate_stats()
    assert dispatch_stats["classified_defer_freeze"] == 0, dispatch_stats
    assert jit.is_jit_compiled(dispatch), dispatch_stats


def compiles_low_loop_object_manipulators_at_base() -> None:
    import cinderjit
    import cinderx.jit as jit

    class Payload:
        def __init__(self):
            self.value = 42

    def write_value(obj, value):
        obj.value = value
        return obj.value

    payload = Payload()
    cinderjit._clear_autojit_gate_stats()

    for value in range(20):
        write_value(payload, value)

    stats = cinderjit._autojit_gate_stats()
    assert stats["classified_defer_freeze"] == 0, stats
    assert jit.is_jit_compiled(write_value), stats


def compiles_self_contained_eafp_predicates() -> None:
    import logging

    import cinderjit
    import cinderx.jit as jit

    logger = logging.getLogger("cinderx.autojit.logging")
    logger.handlers[:] = []
    logger.setLevel(logging.WARNING)
    logger.propagate = False

    cinderjit._clear_autojit_gate_stats()
    # The default IC gate may defer compilation for 256 calls after auto:2.
    for _ in range(512):
        assert not logger.isEnabledFor(logging.DEBUG)
        if jit.is_jit_compiled(logging.Logger.isEnabledFor):
            break

    stats = cinderjit._autojit_gate_stats()
    assert jit.is_jit_compiled(logging.Logger.isEnabledFor), (
        stats,
        jit.count_interpreted_calls(logging.Logger.isEnabledFor),
    )


def defers_call_only_dispatch_loop() -> None:
    import cinderjit
    import cinderx.jit as jit

    class Target:
        def debug(self, msg):
            return None

    def call_only_loop(target, msg, loops):
        for _ in range(loops):
            target.debug(msg)
            target.debug(msg)
            target.debug(msg)
            target.debug(msg)
            target.debug(msg)
            target.debug(msg)
            target.debug(msg)
            target.debug(msg)
            target.debug(msg)
            target.debug(msg)

    target = Target()
    for _ in range(8):
        target.debug("x")

    cinderjit._clear_autojit_gate_stats()
    for _ in range(8):
        call_only_loop(target, "x", 4)

    stats = cinderjit._autojit_gate_stats()
    assert stats["classified_defer_freeze"] >= 1, stats
    assert not jit.is_jit_compiled(call_only_loop), (
        stats,
        jit.count_interpreted_calls(call_only_loop),
    )


CASES = {
    "compiles_low_loop_object_manipulators_at_base": (
        compiles_low_loop_object_manipulators_at_base
    ),
    "compiles_low_roi_functions_at_base": compiles_low_roi_functions_at_base,
    "compiles_self_contained_eafp_predicates": (
        compiles_self_contained_eafp_predicates
    ),
    "defers_call_only_dispatch_loop": defers_call_only_dispatch_loop,
    "gate_stats_smoke": gate_stats_smoke,
    "import_provider_default": import_provider_default,
    "import_provider_off": import_provider_off,
    "multiprocessing_pool_setup_provider_default": (
        multiprocessing_pool_setup_provider_default
    ),
    "multiprocessing_pool_setup_provider_off": (
        multiprocessing_pool_setup_provider_off
    ),
    "roi_backoff_default_freeze": roi_backoff_default_freeze,
    "roi_backoff_disabled": roi_backoff_disabled,
    "roi_backoff_uncompile": roi_backoff_uncompile,
    "setup_provider_default": setup_provider_default,
    "setup_provider_for_cinderx_init": setup_provider_for_cinderx_init,
    "setup_provider_off": setup_provider_off,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("case", choices=sorted(CASES))
    args = parser.parse_args()
    CASES[args.case]()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
