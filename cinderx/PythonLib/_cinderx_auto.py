# Attribute-mode defaults stay process-local; explicit overrides are inherited.
import os
import sys
_attr_mode = os.environ.get('ATTR_PATH_MODE')
_attr_expected = None
_torch_registry_state = 'disabled'

def _attr_flag(name, default):
    raw = os.environ.get(name)
    if raw is None:
        return int(default)
    if raw not in ('0', '1'):
        raise ValueError(name + ' must be 0 or 1')
    return int(raw)

def _verify():
    if _attr_mode == 'off':
        if '_cinderx' in sys.modules:
            raise RuntimeError('Native CinderX unexpectedly loaded in off mode')
        return dict(evaluator=False, attribute=None, counters=None, jit=None)
    import _cinderx
    result = dict(evaluator=_cinderx.is_frame_evaluator_installed(),
                  attribute=_cinderx._attr_path_status(),
                  counters=_cinderx._attr_path_counters(),
                  jit=_cinderx._get_trigger_stats(), native_path=_cinderx.__file__)
    if not result['evaluator'] or any(result['jit'].values()):
        raise RuntimeError('Attribute optimization requires active evaluator and zero JIT counters')
    if _attr_expected is not None and result['attribute'] != _attr_expected:
        raise RuntimeError('Attribute configuration mismatch: expected %r, got %r' %
                           (_attr_expected, result['attribute']))
    return result

def register_torch_registry():
    """Register a PyTorch attribute handler that matches the reference implementation."""
    global _torch_registry_state
    from types import FunctionType, GetSetDescriptorType

    torch = sys.modules.get('torch')
    module = sys.modules.get('torch.nn.modules.module')
    if torch is None or module is None:
        return False
    cls = vars(module).get('Module')
    if type(cls) is not type or cls.__base__ is not object or cls is not torch.nn.Module:
        return False
    handler = vars(cls).get('__getattr__')
    descriptor = vars(cls).get('__dict__')
    if (type(handler) is not FunctionType or handler.__globals__ is not vars(module) or
            type(descriptor) is not GetSetDescriptorType or descriptor.__objclass__ is not cls):
        return False

    reference = """
def __getattr__(self, name):
    if "_parameters" in self.__dict__:
        _parameters = self.__dict__["_parameters"]
        if name in _parameters:
            return _parameters[name]
    if "_buffers" in self.__dict__:
        _buffers = self.__dict__["_buffers"]
        if name in _buffers:
            return _buffers[name]
    if "_modules" in self.__dict__:
        modules = self.__dict__["_modules"]
        if name in modules:
            return modules[name]
    raise AttributeError(
        f"'{type(self).__name__}' object has no attribute '{name}'"
    )
"""
    namespace = {}
    exec(compile(reference, '<torch registry reference>', 'exec', dont_inherit=True), namespace)
    expected = namespace['__getattr__'].__code__
    code = handler.__code__
    fields = ('co_code', 'co_names', 'co_varnames', 'co_freevars', 'co_cellvars',
              'co_flags', 'co_argcount', 'co_posonlyargcount', 'co_kwonlyargcount',
              'co_exceptiontable', 'co_stacksize', 'co_nlocals')
    if any(getattr(code, field) != getattr(expected, field) for field in fields):
        return False
    if (len(code.co_consts) != len(expected.co_consts) or
            any(type(a) is not type(b) or a != b for a, b in zip(code.co_consts, expected.co_consts))):
        return False
    import _cinderx
    _cinderx._register_torch_registry_311(handler, code, descriptor)
    _torch_registry_state = 'registered'
    return True


def _register_loaded_torch():
    global _torch_registry_state
    try:
        if not register_torch_registry():
            _torch_registry_state = 'unsupported'
    except Exception:
        # A package without a matching handler still imports and uses Python.
        _torch_registry_state = 'registration_error'


def _install_torch_registry_hook():
    from importlib.machinery import PathFinder, SourceFileLoader

    global _torch_registry_state
    if 'torch' in sys.modules:
        _register_loaded_torch()
        return
    if PathFinder not in sys.meta_path:
        _torch_registry_state = 'unsupported_loader'
        return

    class TorchRegistryFinder:
        def find_spec(self, fullname, path=None, target=None):
            global _torch_registry_state
            if fullname != 'torch' or target is not None:
                return None
            if self in sys.meta_path:
                sys.meta_path.remove(self)
            spec = PathFinder.find_spec(fullname, path, target)
            if spec is None or type(spec.loader) is not SourceFileLoader:
                _torch_registry_state = 'unsupported_loader'
                return spec
            loader = spec.loader
            original = loader.exec_module
            if 'exec_module' in vars(loader):
                _torch_registry_state = 'unsupported_loader'
                return spec

            def exec_module(module):
                # Restore the normal loader before running package code.
                del loader.exec_module
                original(module)
                _register_loaded_torch()

            loader.exec_module = exec_module
            return spec

    sys.meta_path.insert(sys.meta_path.index(PathFinder), TorchRegistryFinder())
    _torch_registry_state = 'pending'


def attr_status():
    """Report live state in the acceptance system's V24 schema."""
    native = sys.modules.get('_cinderx')
    result = dict(
        mode=_attr_mode if _attr_mode is not None else 'off',
        pid=os.getpid(),
        python=sys.version,
        executable=sys.executable,
        native_loaded=native is not None,
        evaluator=False,
        torch_registry_state=_torch_registry_state,
    )
    if native is not None:
        flags = native._attr_path_status()
        result.update(
            native_path=native.__file__,
            evaluator=native.is_frame_evaluator_installed(),
            attribute=dict(
                flags,
                enabled=flags['no_exception'],
                counting=flags['diagnostics'],
                specialize_enabled=flags['specialization'],
                split_hint_enabled=flags['split_dict'],
                global_cache_enabled=flags['global_cache'],
                inline_entry_ready=flags['inline_call'],
            ),
            jit=native._get_trigger_stats(),
        )
    return result


def _enable_acceptance_reports(folder):
    import atexit
    import json
    from pathlib import Path

    destination = Path(folder).resolve()
    destination.mkdir(parents=True, exist_ok=True)

    def write_report(phase):
        _verify()
        result = attr_status()
        report = destination / ('cinderx-%s-%s.json' % (result['pid'], phase))
        temporary = report.with_suffix('.json.tmp')
        temporary.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        temporary.replace(report)

    def finish():
        try:
            write_report('end')
        except BaseException as error:
            print('ATTR_PATH_INVALID_RESULT: %s' % error, file=sys.stderr, flush=True)
            os._exit(78)

    write_report('start')
    atexit.register(finish)


if _attr_mode is not None:
    try:
        if _attr_mode not in ('off', 'cinderx', 'cinderx_control'):
            raise ValueError('Invalid ATTR_PATH_MODE')
        if sys.version_info[:3] != (3, 11, 6):
            raise RuntimeError('Attribute optimization requires CPython 3.11.6')
        if _attr_mode != 'off':
            if os.environ.get('CINDERX_DISABLE', '0').lower() in ('1','true','yes','on'):
                raise RuntimeError('CINDERX_DISABLE conflicts with explicit attribute mode')
            if os.environ.get('TORCH_MODULE_ATTR_FASTPATH','off') not in ('off','0',''):
                raise RuntimeError('slot_native cannot be combined with attribute mode')
            full = _attr_mode == 'cinderx'
            no_exception = _attr_flag('ATTR_PATH_NOEXC', full)
            _attr_expected = dict(
                no_exception=no_exception,
                specialization=_attr_flag('ATTR_PATH_SPECIALIZE', full),
                split_dict=_attr_flag('ATTR_PATH_SPLIT_HINT', full),
                global_cache=_attr_flag('ATTR_PATH_GLOBAL_CACHE', True),
                inline_call=_attr_flag('ATTR_PATH_INLINE_CALL', True),
                torch_registry_requested=no_exception and _attr_flag('ATTR_PATH_TORCH_REGISTRY', True),
                diagnostics=_attr_flag('ATTR_PATH_DIAGNOSTICS', False))
            import _cinderx
            _cinderx.install_frame_evaluator()
        _verify()
        if _attr_expected is not None and _attr_expected['torch_registry_requested']:
            _install_torch_registry_hook()
        report_folder = os.environ.get('ATTR_PATH_REPORT_DIR')
        if _attr_mode != 'off' and report_folder:
            _enable_acceptance_reports(report_folder)
    except Exception as error:
        # site.addpackage catches Exception and continues; SystemExit must
        # escape the .pth boundary so the application cannot run unoptimized.
        raise SystemExit('ATTR_PATH_STARTUP_FAILED: ' + str(error)) from error

import os
import sys


def _env_flag_enabled(name):
    return os.environ.get(name, "0").lower() in ("1", "true", "yes", "on")


def _cinderx_plugin_enabled():
    return os.environ.get("CINDERX_PLUGIN_ENABLE", "0") == "1"


def _cinderx_force_disabled():
    return _env_flag_enabled("CINDERX_DISABLE")


def _cinderx_jit_disabled():
    return _env_flag_enabled("CINDERX_JIT_DISABLE") or _env_flag_enabled(
        "PYTHONJITDISABLE"
    )


def _cinderx_311_execute_mode():
    # On CPython 3.11 the JIT runs only in the explicit execute mode
    # (CINDERX_JIT_MODE=execute, or its test-only spelling "canary"); off,
    # observe and shadow publish no cinderjit module.  The native side
    # applies the same rule (observe.c), disable switches included.
    return os.environ.get("CINDERX_JIT_MODE", "off") in ("execute", "canary")


if _attr_mode is None and _cinderx_plugin_enabled() and not _cinderx_force_disabled():
    import _cinderx  # noqa: F401

    if _cinderx_jit_disabled():
        cinderjit = None
    elif sys.version_info[:2] < (3, 12):
        if _cinderx_311_execute_mode():
            import cinderjit  # noqa: F401
        else:
            cinderjit = None
    else:
        import cinderjit  # noqa: F401

    if sys.version_info[:2] == (3, 11):
        # Startup control: the takeover happens only on explicit request --
        # plugin enabled plus CINDERX_EVAL_MODE=cinder.  Scoped to 3.11 so
        # other versions keep their existing startup behavior.
        _eval_mode = os.environ.get("CINDERX_EVAL_MODE", "stock")
        if _eval_mode not in ("stock", "cinder"):
            raise RuntimeError(
                "CINDERX_EVAL_MODE=%r is not accepted; this build takes "
                "'stock' or 'cinder'" % (_eval_mode,)
            )
        if _eval_mode == "cinder":
            import cinderx

            cinderx.init()
            _cinderx.install_frame_evaluator()

    _AUTOJIT_IMPORT_PROVIDER_MARKER = "_cinderx_autojit_import_provider"
    _AUTOJIT_SETUP_PROVIDER_MARKER = "_cinderx_autojit_setup_provider"
    _AUTOJIT_GATE_STATS_PREFIX = "CINDERX_AUTOJIT_GATE_STATS: "

    def _is_autojit_classification_value(value):
        return isinstance(value, str) and (
            value == "auto" or value.startswith("auto:")
        )

    def _autojit_import_provider():
        provider = os.environ.get("CINDERX_AUTOJIT_IMPORT_PROVIDER")
        if provider is not None:
            return provider
        if _is_autojit_classification_value(
            os.environ.get("PYTHONJITAUTO")
        ) or _is_autojit_classification_value(sys._xoptions.get("jit-auto")):
            return "find_and_load"
        return "off"

    def _autojit_setup_provider():
        provider = os.environ.get("CINDERX_AUTOJIT_SETUP_PROVIDER")
        if provider is not None:
            return provider
        if _is_autojit_classification_value(
            os.environ.get("PYTHONJITAUTO")
        ) or _is_autojit_classification_value(sys._xoptions.get("jit-auto")):
            return "lib2to3_main,multiprocessing_pool"
        return "off"

    def _autojit_setup_provider_tokens(provider=None):
        if provider is None:
            provider = _autojit_setup_provider()
        if provider in ("", "0", "off"):
            return ()
        return tuple(
            token.strip()
            for token in provider.replace("+", ",").split(",")
            if token.strip() and token.strip() not in ("0", "off")
        )

    def _maybe_enable_autojit_gate_stats():
        if cinderjit is None:
            return
        if not _env_flag_enabled("CINDERX_AUTOJIT_GATE_STATS"):
            return
        clear_stats = getattr(cinderjit, "_clear_autojit_gate_stats", None)
        read_stats = getattr(cinderjit, "_autojit_gate_stats", None)
        if clear_stats is None or read_stats is None:
            return

        clear_stats()

        def dump_autojit_gate_stats():
            import json

            payload = {
                "argv": sys.argv,
                "executable": sys.executable,
                "pid": os.getpid(),
                "stats": read_stats(),
            }
            line = json.dumps(payload, sort_keys=True)
            stats_file = os.environ.get("CINDERX_AUTOJIT_GATE_STATS_FILE")
            if stats_file:
                with open(stats_file, "a", encoding="utf-8") as out:
                    out.write(line)
                    out.write("\n")
            else:
                print(_AUTOJIT_GATE_STATS_PREFIX + line, file=sys.stderr)

        import atexit

        atexit.register(dump_autojit_gate_stats)

    def _autojit_setup_predicate_matches(predicate, args):
        if predicate is None:
            return True
        if not args:
            return False
        return predicate(args[0])

    def _make_autojit_setup_wrapper(original, provider, predicate=None):
        def wrapper(*args, **kwargs):
            if not _autojit_setup_predicate_matches(predicate, args):
                return original(*args, **kwargs)
            _cinderx._autojit_setup_enter()
            try:
                return original(*args, **kwargs)
            finally:
                _cinderx._autojit_setup_leave()

        setattr(wrapper, _AUTOJIT_SETUP_PROVIDER_MARKER, provider)
        setattr(wrapper, "__wrapped__", original)
        return wrapper

    def _make_autojit_setup_enter_wrapper(original, provider, predicate=None):
        def wrapper(*args, **kwargs):
            if not _autojit_setup_predicate_matches(predicate, args):
                return original(*args, **kwargs)
            _cinderx._autojit_setup_enter()
            try:
                return original(*args, **kwargs)
            except BaseException:
                _cinderx._autojit_setup_leave()
                raise

        setattr(wrapper, _AUTOJIT_SETUP_PROVIDER_MARKER, provider)
        setattr(wrapper, "__wrapped__", original)
        return wrapper

    def _make_autojit_setup_leave_wrapper(original, provider, predicate=None):
        def wrapper(*args, **kwargs):
            if not _autojit_setup_predicate_matches(predicate, args):
                return original(*args, **kwargs)
            try:
                return original(*args, **kwargs)
            finally:
                _cinderx._autojit_setup_leave()

        setattr(wrapper, _AUTOJIT_SETUP_PROVIDER_MARKER, provider)
        setattr(wrapper, "__wrapped__", original)
        return wrapper

    def _is_process_pool_instance(obj):
        cls = type(obj)
        return (
            getattr(cls, "__module__", None) == "multiprocessing.pool"
            and getattr(cls, "__name__", None) == "Pool"
        )

    def _wrap_autojit_setup_attr(target, attr, provider, make_wrapper, predicate=None):
        current = getattr(target, attr, None)
        if current is None:
            return
        if getattr(current, _AUTOJIT_SETUP_PROVIDER_MARKER, None) == provider:
            return
        setattr(target, attr, make_wrapper(current, provider, predicate))

    def _install_autojit_multiprocessing_pool_provider(module):
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

    def _install_autojit_subprocess_provider(module):
        if sys.version_info[:2] != (3, 11):
            return
        process = getattr(module, "Popen", None)
        if not isinstance(process, type):
            return
        # Process creation and blocking wait are control-plane work, like pool
        # setup. Do not hold this scope across __enter__: application code inside
        # a Popen context must remain eligible for automatic compilation.
        for attr in ("__init__", "wait"):
            current = getattr(process, attr, None)
            if isinstance(current, type(_install_autojit_subprocess_provider)) and getattr(
                current, _AUTOJIT_SETUP_PROVIDER_MARKER, None
            ) != "subprocess_popen":
                setattr(
                    process, attr, _cinderx._autojit_setup_wrap(current, "subprocess_popen")
                )

    def _maybe_install_autojit_setup_provider_for_module(fullname, provider=None):
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

        if "subprocess_popen" in providers and fullname == "subprocess":
            module = sys.modules.get("subprocess")
            if module is not None:
                _install_autojit_subprocess_provider(module)

    def _make_autojit_import_wrapper(original, provider):
        setup_provider = _autojit_setup_provider()

        def wrapper(*args, **kwargs):
            _cinderx._autojit_import_enter()
            try:
                module = original(*args, **kwargs)
            finally:
                _cinderx._autojit_import_leave()
            if setup_provider and args and isinstance(args[0], str):
                _maybe_install_autojit_setup_provider_for_module(
                    args[0], setup_provider
                )
            return module

        setattr(wrapper, _AUTOJIT_IMPORT_PROVIDER_MARKER, provider)
        return wrapper

    def _install_autojit_import_provider():
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

    def _maybe_enable_parallel_gc():
        if os.environ.get("PARALLEL_GC_ENABLED", "0") != "1":
            return
        if not _cinderx.has_parallel_gc():
            return

        import gc

        thresholds = gc.get_threshold()
        parallel_gc_threshold_gen0 = int(
            os.environ.get("PARALLEL_GC_THRESHOLD_GEN0", thresholds[0])
        )
        parallel_gc_threshold_gen1 = int(
            os.environ.get("PARALLEL_GC_THRESHOLD_GEN1", thresholds[1])
        )
        parallel_gc_threshold_gen2 = int(
            os.environ.get("PARALLEL_GC_THRESHOLD_GEN2", thresholds[2])
        )
        gc.set_threshold(
            parallel_gc_threshold_gen0,
            parallel_gc_threshold_gen1,
            parallel_gc_threshold_gen2,
        )

        parallel_gc_num_threads = int(os.environ.get("PARALLEL_GC_NUM_THREADS", "0"))
        parallel_gc_min_generation = int(
            os.environ.get("PARALLEL_GC_MIN_GENERATION", "2")
        )

        _cinderx.enable_parallel_gc(
            min_generation=parallel_gc_min_generation,
            num_threads=parallel_gc_num_threads,
        )

    _maybe_enable_parallel_gc()
    if cinderjit is not None:
        _maybe_enable_autojit_gate_stats()
        _install_autojit_import_provider()
        _maybe_install_autojit_setup_provider_for_module("lib2to3.main")
        _maybe_install_autojit_setup_provider_for_module("multiprocessing.pool")
        _maybe_install_autojit_setup_provider_for_module("subprocess")
