# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict
"""High-performance Python runtime extensions."""

from __future__ import annotations

import sys
from os import environ

# ============================================================================
# Note!
#
# At Meta, this module is currently loaded as part of Lib/site.py in an attempt
# to get benefits from using it as soon as possible.  However
# Lib/test/test_site.py will assert that site.py does not import too many
# modules.  Be careful with adding import statements here.
#
# The plan is to move applications over to using an explicit initialization
# step rather than Lib/site.py.  Once that is done we can add all the imports
# we want here.
# ============================================================================


_import_error: ImportError | None = None


def is_force_disabled() -> bool:
    """
    Check that the _cinderx native extension hasn't been forcefully disabled
    via an explicit environment variable.
    """

    cinderx_disable = environ.get("CINDERX_DISABLE", "")
    return cinderx_disable != "" and cinderx_disable != "0"


def is_supported_runtime() -> bool:
    """
    Check that the current Python runtime will be able to load the _cinderx
    native extension.
    """

    if sys.platform not in ("darwin", "linux"):
        return False

    version = (sys.version_info.major, sys.version_info.minor)
    if version == (3, 11):
        return sys.version_info.micro == 6
    if version == (3, 14) or version == (3, 15):
        return True
    if version == (3, 12):
        return "+meta" in sys.version
    return False


try:
    if is_force_disabled():
        raise ImportError(
            "The _cinderx native extension has been forcibly disabled with CINDERX_DISABLE in the environment"
        )

    # Currently if we try to import _cinderx on runtimes without our internal patches
    # the import will crash.  This is meant to go away in the future.
    if not is_supported_runtime():
        raise ImportError(
            f"The _cinderx native extension is not supported for Python version '{sys.version}' on platform '{sys.platform}'"
        )

    from _cinderx import (
        _autojit_import_depth,
        _autojit_import_enter,
        _autojit_import_leave,
        _autojit_import_scope_depth,
        _autojit_setup_depth,
        _autojit_setup_enter,
        _autojit_setup_leave,
        _compile_perf_trampoline_pre_fork,
        _is_compile_perf_trampoline_pre_fork_enabled,
        async_cached_classproperty,
        async_cached_property,
        cached_classproperty,
        cached_property,
        cached_property_with_descr,
        clear_caches,
        clear_classloader_caches,
        disable_parallel_gc,
        enable_parallel_gc,
        freeze_type,
        get_parallel_gc_settings,
        has_parallel_gc,
        immortalize_heap,
        install_frame_evaluator,
        is_frame_evaluator_installed,
        is_lightweight_frames_enabled,
        is_immortal,
        remove_frame_evaluator,
        strict_module_patch,
        strict_module_patch_delete,
        strict_module_patch_enabled,
        StrictModule,
        watch_sys_modules,
    )

    if sys.version_info < (3, 11):
        # In 3.12+ use the versions in the polyfill cinder library instead.
        from _cinderx import (
            _get_entire_call_stack_as_qualnames_with_lineno,
            _get_entire_call_stack_as_qualnames_with_lineno_and_frame,
            clear_all_shadow_caches,
        )

    if sys.version_info >= (3, 12):
        from _cinderx import delay_adaptive, get_adaptive_delay, set_adaptive_delay

except ImportError as e:
    if "undefined symbol:" in str(e):
        # If we're on a dev build report this as an error, otherwise muddle along with alternative definitions
        # on unsupported Python's.
        from os.path import dirname, exists, join

        if exists(join(dirname(__file__), ".dev_build")):
            raise ImportError(
                "The _cinderx native extension is not available due to a missing symbol. This is likely a bug you introduced.  "
                "Please ensure that the cinderx kernel is being used."
            ) from e
    _import_error = e

    def _compile_perf_trampoline_pre_fork() -> None:
        pass

    def _get_entire_call_stack_as_qualnames_with_lineno() -> list[tuple[str, int]]:
        return []

    def _get_entire_call_stack_as_qualnames_with_lineno_and_frame() -> list[
        tuple[str, int, object]
    ]:
        return []

    def _is_compile_perf_trampoline_pre_fork_enabled() -> bool:
        return False

    def _autojit_import_enter() -> None:
        pass

    def _autojit_import_leave() -> None:
        pass

    def _autojit_import_depth() -> int:
        return 0

    def _autojit_import_scope_depth() -> int:
        return 0

    def _autojit_setup_enter() -> None:
        pass

    def _autojit_setup_leave() -> None:
        pass

    def _autojit_setup_depth() -> int:
        return 0

    def is_lightweight_frames_enabled() -> bool:
        return False

    from asyncio import AbstractEventLoop, Future
    from typing import (
        Awaitable,
        Callable,
        Dict,
        final,
        Generator,
        Generic,
        List,
        NoReturn,
        Optional,
        overload,
        Tuple,
        Type,
        TYPE_CHECKING,
        TypeVar,
    )

    _TClass = TypeVar("_TClass")
    _TReturnType = TypeVar("_TReturnType")

    @final
    class NoValueSet:
        pass

    NO_VALUE_SET = NoValueSet()

    class _BaseCachedProperty(Generic[_TClass, _TReturnType]):
        fget: Callable[[_TClass], _TReturnType]
        __name__: str

        def __init__(
            self,
            f: Callable[[_TClass], _TReturnType],
            slot: Optional[Descriptor[_TReturnType]] = None,
        ) -> None:
            self.fget: Callable[[_TClass], _TReturnType] = f
            self.__name__ = f.__name__
            self.__doc__: str | None = f.__doc__
            self.slot = slot

        @overload
        def __get__(
            self, obj: None, cls: Type[_TClass]
        ) -> _BaseCachedProperty[_TClass, _TReturnType]: ...

        @overload
        def __get__(self, obj: _TClass, cls: Type[_TClass]) -> _TReturnType: ...

        def __get__(
            self, obj: Optional[_TClass], cls: Type[_TClass]
        ) -> _BaseCachedProperty[_TClass, _TReturnType] | _TReturnType:
            if obj is None:
                return self
            slot = self.slot
            if slot is not None:
                try:
                    res = slot.__get__(obj, cls)
                except AttributeError:
                    res = self.fget(obj)
                    slot.__set__(obj, res)
                return res

            result = self.fget(obj)
            obj.__dict__[self.__name__] = result
            return result

    class _AsyncLazyValueState:
        NotStarted = 0
        Running = 1
        Done = 2

    _T = TypeVar("_T", covariant=True)
    _TParams = TypeVar("_TParams")

    # noqa: F401
    import asyncio

    class _AsyncLazyValue(Awaitable[_T]):
        """
        This is a low-level class used mainly for two things:
        * It helps to avoid calling a coroutine multiple times, by caching the
        result of a previous call
        * It ensures that the coroutine is called only once

        _AsyncLazyValue has well defined cancellation behavior in these cases:

        1. When we have a single task stack (call stack for you JS folks), which is
        awaiting on the AsyncLazyValue
        -> In this case, we mimic the behavior of a normal await. i.e: If the
            task stack gets cancelled, we cancel the coroutine (by raising a
            CancelledError in the underlying future)

        2. When we have multiple task stacks awaiting on the future.
        We have two sub cases here.

        2.1. The initial task stack (which resulted in an await of the coroutine)
                gets cancelled.
                -> In this case, we cancel the coroutine, and all the tasks depending
                on it. If we don't do that, we'd have to implement retry logic,
                which is a bad idea in such low level code. Even if we do implement
                retries, there's no guarantee that they would succeed, so it's better
                to just fail here.

                Also, the number of times this happens is very small (I don't have
                data to prove it, but qualitative arguments suggest this is the
                case).

        2.2. One of the many task stacks gets cancelled (but not the one which ended
            up awaiting the coroutine)
            -> In this case, we just allow the task stack to be cancelled, but
                the rest of them are processed without being affected.
        """

        def __init__(
            self,
            # pyre-fixme[31]: Expression `typing.Callable[(_TParams,
            #  typing.Awaitable[_T])]` is not a valid type.
            coro_func: Callable[_TParams, Awaitable[_T]],
            # pyre-fixme[11]: Annotation `args` is not defined as a type.
            *args: _TParams.args,
            # pyre-fixme[11]: Annotation `kwargs` is not defined as a type.
            **kwargs: _TParams.kwargs,
        ) -> None:
            global asyncio
            # pyre-fixme[31]: Expression `typing.Optional[typing.Callable[(_TParams,
            #  typing.Awaitable[_T])]]` is not a valid type.
            self.coro_func: Optional[Callable[_TParams, Awaitable[_T]]] = coro_func
            self.args: Tuple[object, ...] = args
            self.kwargs: Dict[str, object] = kwargs
            self.state: int = _AsyncLazyValueState.NotStarted
            self.res: Optional[_T] = None
            self._futures: List[Future] = []
            self._awaiting_tasks = 0

        async def _async_compute(self) -> _T:
            futures = self._futures
            try:
                coro_func = self.coro_func
                # lint-fixme: NoAssertsRule
                assert coro_func is not None
                self.res = res = await coro_func(*self.args, **self.kwargs)

                self.state = _AsyncLazyValueState.Done

                # pyre-fixme[1001]: Awaitable assigned to `value` is never awaited.
                for value in futures:
                    if not value.done():
                        value.set_result(self.res)

                self.args = ()
                self.kwargs.clear()
                del self._futures[:]
                self.coro_func = None

                return res

            except (Exception, asyncio.CancelledError) as e:
                # pyre-fixme[1001]: Awaitable assigned to `value` is never awaited.
                for value in futures:
                    if not value.done():
                        value.set_exception(e)
                self._futures = []
                self.state = _AsyncLazyValueState.NotStarted
                raise

        def _get_future(self, loop: Optional[AbstractEventLoop]) -> Future:
            if loop is None:
                loop = asyncio.get_event_loop()
            f = asyncio.Future(loop=loop)
            self._futures.append(f)
            self._awaiting_tasks += 1
            return f

        def __iter__(self) -> _AsyncLazyValue[_T]:
            return self

        def __next__(self) -> NoReturn:
            raise StopIteration(self.res)

        def __await__(self) -> Generator[None, None, _T]:
            if self.state == _AsyncLazyValueState.Done:
                # pyre-ignore[7]: Expected `Generator[None, None, Variable[_T](covariant)]`
                # but got `_AsyncLazyValue[Variable[_T](covariant)]`.
                return self
            elif self.state == _AsyncLazyValueState.Running:
                c = self._get_future(None)
                return c.__await__()
            else:
                self.state = _AsyncLazyValueState.Running
                c = self._async_compute()
                return c.__await__()

        def as_future(self, loop: AbstractEventLoop) -> Future:
            if self.state == _AsyncLazyValueState.Done:
                f = asyncio.Future(loop=loop)
                f.set_result(self.res)
                return f
            elif self.state == _AsyncLazyValueState.Running:
                return self._get_future(loop)
            else:
                if loop is None:
                    loop = asyncio.get_event_loop()
                t = loop.create_task(self._async_compute())
                self.state = _AsyncLazyValueState.Running
                # pyre-ignore[16]: Undefined attribute `asyncio.tasks.Task`
                # has no attribute `_source_traceback`.
                if t._source_traceback:
                    # pyrefly: ignore [unsupported-operation]
                    del t._source_traceback[-1]
                return t

    _TAwaitableReturnType = TypeVar("_TAwaitableReturnType")

    class async_cached_property(
        Generic[_TAwaitableReturnType, _TClass],
        _BaseCachedProperty[_TClass, Awaitable[_TAwaitableReturnType]],
    ):
        def __init__(
            self,
            f: Callable[[_TClass], _TReturnType],
            slot: Optional[Descriptor[_TReturnType]] = None,
        ) -> None:
            # pyrefly: ignore [bad-argument-type]
            super().__init__(f, slot)

        # pyrefly: ignore [bad-override]
        def __get__(
            self, obj: Optional[_TClass], cls: Type[_TClass]
        ) -> (
            _BaseCachedProperty[_TClass, Awaitable[_TAwaitableReturnType]]
            | Awaitable[_TAwaitableReturnType]
        ):
            if obj is None:
                return self

            slot = self.slot
            if slot is not None:
                try:
                    res = slot.__get__(obj, cls)
                except AttributeError:
                    res = _AsyncLazyValue(self.fget, obj)
                    slot.__set__(obj, res)
                return res

            lazy_value = _AsyncLazyValue(self.fget, obj)
            setattr(obj, self.__name__, lazy_value)
            return lazy_value

    class async_cached_classproperty(
        Generic[_TAwaitableReturnType, _TClass],
        _BaseCachedProperty[Type[_TClass], Awaitable[_TAwaitableReturnType]],
    ):
        def __init__(
            self,
            f: Callable[[_TClass], Awaitable[_TAwaitableReturnType]],
            slot: Optional[Descriptor[Awaitable[_TAwaitableReturnType]]] = None,
        ) -> None:
            # pyrefly: ignore [bad-argument-type]
            super().__init__(f, slot)
            self._value: NoValueSet | Awaitable[_TAwaitableReturnType] = NO_VALUE_SET

        # pyrefly: ignore [bad-override]
        def __get__(
            self, obj: Optional[_TClass], cls: Type[_TClass]
        ) -> Awaitable[_TAwaitableReturnType]:
            lazy_value = self._value
            if not isinstance(lazy_value, NoValueSet):
                return lazy_value
            self._value = lazy_value = _AsyncLazyValue(self.fget, cls)
            return lazy_value

    class cached_classproperty(_BaseCachedProperty[Type[_TClass], _TReturnType]):
        def __init__(
            self,
            f: Callable[[_TClass], _TReturnType],
            slot: Optional[Descriptor[_TReturnType]] = None,
        ) -> None:
            # pyrefly: ignore [bad-argument-type]
            super().__init__(f, slot)
            self._value: NoValueSet | _TReturnType = NO_VALUE_SET

        # pyrefly: ignore [bad-override]
        def __get__(self, obj: Optional[_TClass], cls: Type[_TClass]) -> _TReturnType:
            result = self._value
            if not isinstance(result, NoValueSet):
                return result
            self._value = result = self.fget(cls)
            return result

    _TClass = TypeVar("_TClass")
    _TReturnType = TypeVar("_TReturnType")

    if TYPE_CHECKING:
        from abc import ABC

        @final
        class Descriptor(ABC, Generic[_TReturnType]):
            __name__: str
            __objclass__: Type[object]

            def __get__(
                self, inst: object, ctx: Optional[Type[object]] = None
            ) -> _TReturnType: ...

            def __set__(self, inst: object, value: _TReturnType) -> None:
                pass

            def __delete__(self, inst: object) -> None:
                pass

    class cached_property(_BaseCachedProperty[_TClass, _TReturnType]):
        def __init__(
            self,
            f: Callable[[_TClass], _TReturnType],
            slot: Optional[Descriptor[_TReturnType]] = None,
        ) -> None:
            super().__init__(f, slot)
            if slot is not None:
                if (
                    type(self) is not cached_property
                    and type(self) is not cached_property_with_descr
                ):
                    raise TypeError(
                        "slot can't be used with subtypes of cached_property"
                    )
                # pyre-ignore[4]: Missing attribute annotation for __class__
                self.__class__ = cached_property_with_descr

    class cached_property_with_descr(cached_property[_TClass, _TReturnType]):
        def __set__(self, inst: object, value: _TReturnType) -> None:
            slot = self.slot
            if slot is not None:
                slot.__set__(inst, value)
            else:
                setattr(inst, self.__name__, value)

        def __delete__(self, inst: object) -> None:
            slot = self.slot
            if slot is not None:
                slot.__delete__(inst)
            else:
                delattr(inst, self.__name__)

    if sys.version_info < (3, 11):

        def clear_all_shadow_caches() -> None:
            pass

    def clear_caches() -> None:
        pass

    def clear_classloader_caches() -> None:
        pass

    def disable_parallel_gc() -> None:
        pass

    def enable_parallel_gc(min_generation: int = 2, num_threads: int = 0) -> None:
        raise RuntimeError(
            "No Parallel GC support because _cinderx did not load correctly"
        )

    def freeze_type(ty: object) -> object:
        return ty

    def get_parallel_gc_settings() -> dict[str, int] | None:
        return None

    def has_parallel_gc() -> bool:
        return False

    def immortalize_heap() -> None:
        pass

    def install_frame_evaluator() -> None:
        pass

    def is_frame_evaluator_installed() -> bool:
        return False

    def is_lightweight_frames_enabled() -> bool:
        return False

    def is_immortal(obj: object) -> bool:
        raise RuntimeError(
            "Can't answer whether an object is mortal or immortal from Python code"
        )

    def remove_frame_evaluator() -> None:
        pass

    def strict_module_patch(mod: object, name: str, value: object) -> None:
        pass

    def strict_module_patch_delete(mod: object, name: str) -> None:
        pass

    def strict_module_patch_enabled(mod: object) -> bool:
        return False

    class StrictModule:
        def __init__(self, d: dict[str, object], b: bool) -> None:
            pass

    def watch_sys_modules() -> None:
        pass


def maybe_enable_parallel_gc() -> None:
    """Conditionally enable parallel GC based on environment variables."""
    is_parallel_gc_enabled = environ.get("PARALLEL_GC_ENABLED", "0") == "1"
    if not has_parallel_gc() or not is_parallel_gc_enabled:
        return
    import gc

    thresholds = gc.get_threshold()
    parallel_gc_threshold_gen0 = int(
        environ.get("PARALLEL_GC_THRESHOLD_GEN0", thresholds[0])
    )
    parallel_gc_threshold_gen1 = int(
        environ.get("PARALLEL_GC_THRESHOLD_GEN1", thresholds[1])
    )
    parallel_gc_threshold_gen2 = int(
        environ.get("PARALLEL_GC_THRESHOLD_GEN2", thresholds[2])
    )
    gc.set_threshold(
        parallel_gc_threshold_gen0,
        parallel_gc_threshold_gen1,
        parallel_gc_threshold_gen2,
    )

    parallel_gc_num_threads = int(environ.get("PARALLEL_GC_NUM_THREADS", "0"))
    parallel_gc_min_generation = int(environ.get("PARALLEL_GC_MIN_GENERATION", "2"))

    enable_parallel_gc(
        min_generation=parallel_gc_min_generation,
        num_threads=parallel_gc_num_threads,
    )


_is_init: bool = False


_AUTOJIT_IMPORT_PROVIDER_MARKER = "_cinderx_autojit_import_provider"
_AUTOJIT_SETUP_PROVIDER_MARKER = "_cinderx_autojit_setup_provider"


def _is_autojit_classification_value(value: object) -> bool:
    return isinstance(value, str) and (value == "auto" or value.startswith("auto:"))


def _autojit_scheduling_is_configured() -> bool:
    """Whether a mode that schedules automatically is configured.

    On 3.12+ that is the auto[:N] classifier, which is also what asks for
    the import and setup providers.  CPython 3.11 refuses that spelling --
    its threshold is a plain count -- and names the mode separately, so
    the providers have to key off the mode there.  Keying off the
    classifier alone left them off in exactly the configuration that
    schedules: CINDERX_JIT_MODE=execute with a numeric PYTHONJITAUTO,
    which is the product configuration and the one the matrix tests.
    """
    if _is_autojit_classification_value(
        environ.get("PYTHONJITAUTO")
    ) or _is_autojit_classification_value(sys._xoptions.get("jit-auto")):
        return True
    if sys.version_info[:2] == (3, 11):
        return environ.get("CINDERX_JIT_MODE") in ("execute", "canary")
    return False


def _autojit_import_provider() -> str:
    provider = environ.get("CINDERX_AUTOJIT_IMPORT_PROVIDER")
    if provider is not None:
        return provider
    if _autojit_scheduling_is_configured():
        return "find_and_load"
    return "off"


def _autojit_setup_provider() -> str:
    provider = environ.get("CINDERX_AUTOJIT_SETUP_PROVIDER")
    if provider is not None:
        return provider
    if _autojit_scheduling_is_configured():
        return "lib2to3_main,multiprocessing_pool"
    return "off"


def _autojit_setup_provider_tokens(provider: str | None = None) -> tuple[str, ...]:
    if provider is None:
        provider = _autojit_setup_provider()
    if provider in ("", "0", "off"):
        return ()
    return tuple(
        token.strip()
        for token in provider.replace("+", ",").split(",")
        if token.strip() and token.strip() not in ("0", "off")
    )


def _autojit_setup_predicate_matches(
    predicate: object | None, args: tuple[object, ...]
) -> bool:
    if predicate is None:
        return True
    if not args:
        return False
    # pyre-ignore[29]: Provider predicates are dynamically selected callables.
    return predicate(args[0])


def _make_autojit_setup_wrapper(
    original: object, provider: str, predicate: object | None = None
) -> object:
    def wrapper(*args: object, **kwargs: object) -> object:
        if not _autojit_setup_predicate_matches(predicate, args):
            # pyre-ignore[29]: The wrapped setup callable is dynamically chosen.
            return original(*args, **kwargs)
        _autojit_setup_enter()
        try:
            # pyre-ignore[29]: The wrapped setup callable is dynamically chosen.
            return original(*args, **kwargs)
        finally:
            _autojit_setup_leave()

    setattr(wrapper, _AUTOJIT_SETUP_PROVIDER_MARKER, provider)
    setattr(wrapper, "__wrapped__", original)
    return wrapper


def _make_autojit_setup_enter_wrapper(
    original: object, provider: str, predicate: object | None = None
) -> object:
    def wrapper(*args: object, **kwargs: object) -> object:
        if not _autojit_setup_predicate_matches(predicate, args):
            # pyre-ignore[29]: The wrapped setup callable is dynamically chosen.
            return original(*args, **kwargs)
        _autojit_setup_enter()
        try:
            # pyre-ignore[29]: The wrapped setup callable is dynamically chosen.
            return original(*args, **kwargs)
        except BaseException:
            _autojit_setup_leave()
            raise

    setattr(wrapper, _AUTOJIT_SETUP_PROVIDER_MARKER, provider)
    setattr(wrapper, "__wrapped__", original)
    return wrapper


def _make_autojit_setup_leave_wrapper(
    original: object, provider: str, predicate: object | None = None
) -> object:
    def wrapper(*args: object, **kwargs: object) -> object:
        if not _autojit_setup_predicate_matches(predicate, args):
            # pyre-ignore[29]: The wrapped setup callable is dynamically chosen.
            return original(*args, **kwargs)
        try:
            # pyre-ignore[29]: The wrapped setup callable is dynamically chosen.
            return original(*args, **kwargs)
        finally:
            _autojit_setup_leave()

    setattr(wrapper, _AUTOJIT_SETUP_PROVIDER_MARKER, provider)
    setattr(wrapper, "__wrapped__", original)
    return wrapper


def _is_process_pool_instance(obj: object) -> bool:
    cls = type(obj)
    return (
        getattr(cls, "__module__", None) == "multiprocessing.pool"
        and getattr(cls, "__name__", None) == "Pool"
    )


def _wrap_autojit_setup_attr(
    target: object,
    attr: str,
    provider: str,
    make_wrapper: object,
    predicate: object | None = None,
) -> None:
    current = getattr(target, attr, None)
    if current is None:
        return
    if getattr(current, _AUTOJIT_SETUP_PROVIDER_MARKER, None) == provider:
        return
    # pyre-ignore[29]: The wrapper factory is selected by provider type.
    setattr(target, attr, make_wrapper(current, provider, predicate))


def _install_autojit_multiprocessing_pool_provider(module: object) -> None:
    pool = getattr(module, "Pool", None)
    if pool is not None:
        _wrap_autojit_setup_attr(
            pool,
            "__init__",
            "multiprocessing_pool",
            _make_autojit_setup_wrapper,
            _is_process_pool_instance,
        )
        _wrap_autojit_setup_attr(
            pool,
            "__enter__",
            "multiprocessing_pool",
            _make_autojit_setup_enter_wrapper,
            _is_process_pool_instance,
        )
        _wrap_autojit_setup_attr(
            pool,
            "__exit__",
            "multiprocessing_pool",
            _make_autojit_setup_leave_wrapper,
            _is_process_pool_instance,
        )
        for attr in (
            "map",
            "imap",
            "imap_unordered",
            "starmap",
            "map_async",
            "starmap_async",
        ):
            _wrap_autojit_setup_attr(
                pool,
                attr,
                "multiprocessing_pool",
                _make_autojit_setup_wrapper,
                _is_process_pool_instance,
            )


def _maybe_install_autojit_setup_provider_for_module(
    fullname: str, provider: str | None = None
) -> None:
    providers = _autojit_setup_provider_tokens(provider)
    if not providers:
        return

    if "lib2to3_main" in providers and fullname == "lib2to3.main":
        module = sys.modules.get("lib2to3.main")
        if module is None:
            return

        current = getattr(module, "main", None)
        if current is None:
            return
        if (
            getattr(current, _AUTOJIT_SETUP_PROVIDER_MARKER, None)
            != "lib2to3_main"
        ):
            setattr(
                module,
                "main",
                _make_autojit_setup_wrapper(current, "lib2to3_main"),
            )

    if "multiprocessing_pool" in providers and fullname == "multiprocessing.pool":
        module = sys.modules.get("multiprocessing.pool")
        if module is not None:
            _install_autojit_multiprocessing_pool_provider(module)


# The wrapper below replaces importlib._bootstrap._find_and_load, so it
# becomes a frame in every import.  `warnings` decides which frame to
# blame with stacklevel, and it skips the import machinery by filename:
#
#     def _is_internal_frame(frame):
#         filename = frame.f_code.co_filename
#         return 'importlib' in filename and '_bootstrap' in filename
#
# A wrapper defined here carries THIS file's name, so it is not skipped
# and every import-time warning gets blamed on cinderx instead of on the
# code that imported the module.  Compiling the wrapper under the name of
# the machinery it stands in for keeps that attribution where it was.
_AUTOJIT_IMPORT_WRAPPER_FILENAME = "<frozen importlib._bootstrap>"

_AUTOJIT_IMPORT_WRAPPER_SOURCE = """
def _make(original, provider, setup_provider, enter, leave, install, marker):
    def wrapper(*args, **kwargs):
        enter()
        try:
            module = original(*args, **kwargs)
        finally:
            leave()
        if setup_provider and args and isinstance(args[0], str):
            install(args[0], setup_provider)
        return module

    setattr(wrapper, marker, provider)
    return wrapper
"""


def _make_autojit_import_wrapper(original: object, provider: str) -> object:
    setup_provider = _autojit_setup_provider()
    namespace: dict[str, object] = {}
    exec(
        compile(
            _AUTOJIT_IMPORT_WRAPPER_SOURCE,
            _AUTOJIT_IMPORT_WRAPPER_FILENAME,
            "exec",
        ),
        namespace,
    )
    # pyre-ignore[29]: Built by the exec above.
    wrapper = namespace["_make"](
        original,
        provider,
        setup_provider,
        _autojit_import_enter,
        _autojit_import_leave,
        _maybe_install_autojit_setup_provider_for_module,
        _AUTOJIT_IMPORT_PROVIDER_MARKER,
    )
    return wrapper


def _install_autojit_import_provider() -> None:
    provider = _autojit_import_provider()
    if provider in ("", "0", "off"):
        return

    if provider == "builtins":
        target = sys.modules.get("builtins")
        attr = "__import__"
    elif provider == "find_and_load":
        target = sys.modules.get("importlib._bootstrap")
        attr = "_find_and_load"
    else:
        return

    if target is None:
        return

    current = getattr(target, attr)
    if getattr(current, _AUTOJIT_IMPORT_PROVIDER_MARKER, None) == provider:
        return

    setattr(target, attr, _make_autojit_import_wrapper(current, provider))


def init() -> None:
    """Initialize CinderX."""
    global _is_init

    # Failed to import _cinderx, nothing to initialize.
    if _import_error is not None:
        return

    # Already initialized.
    if _is_init:
        return

    maybe_enable_parallel_gc()
    _install_autojit_import_provider()
    _maybe_install_autojit_setup_provider_for_module("lib2to3.main")
    _maybe_install_autojit_setup_provider_for_module("multiprocessing.pool")

    _is_init = True


def is_initialized() -> bool:
    """
    Check if the cinderx extension has been properly initialized.
    """
    return _is_init


def get_import_error() -> ImportError | None:
    """
    Get the ImportError that occurred when _cinderx was imported, if there was
    an error.
    """
    return _import_error


init()
