# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Subprocess setup scopes: binding, warnings, cleanup and JIT eligibility."""

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import textwrap
import unittest


@unittest.skipUnless(sys.version_info[:3] == (3, 11, 6), "requires CPython 3.11.6")
class SubprocessSetup311Tests(unittest.TestCase):

    def run_child(self, source, *, extra_env=None, expected=0, before=""):
        spec = importlib.util.find_spec("_cinderx")
        if spec is None or spec.origin is None:
            self.skipTest("requires an installed CinderX native extension")
        package_dir = str(Path(spec.origin).parent)
        env = {
            name: value
            for name, value in os.environ.items()
            if not name.startswith(("CINDERX_", "PYTHONJIT", "SPIKE_"))
            and name != "PYTHONPATH"
        }
        env.update(
            CINDERX_PLUGIN_ENABLE="1",
            CINDERX_EVAL_MODE="cinder",
            CINDERX_JIT_MODE="execute",
            PYTHONJITAUTO="4",
            PYTHONJITLIGHTWEIGHTFRAME="0",
        )
        if extra_env:
            env.update(extra_env)
        code = (
            "import sys\nsys.path.insert(0, " + repr(package_dir) + ")\n"
            + textwrap.dedent(before)
            + "\nimport _cinderx_auto\n"
            + textwrap.dedent(source)
        )
        result = subprocess.run(
            [sys.executable, "-S", "-c", code],
            env=env,
            capture_output=True,
            text=True,
            timeout=45,
        )
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def test_process_control_provider_is_idempotent_and_preserves_signature(self):
        self.run_child("""
            import inspect, subprocess, _cinderx
            init = subprocess.Popen.__init__
            wait = subprocess.Popen.wait
            assert init._cinderx_autojit_setup_provider == 'subprocess_popen'
            assert wait._cinderx_autojit_setup_provider == 'subprocess_popen'
            assert inspect.signature(init) == inspect.signature(init.__wrapped__)
            _cinderx_auto._maybe_install_autojit_setup_provider_for_module('subprocess')
            import cinderx
            cinderx._maybe_install_autojit_setup_provider_for_module('subprocess')
            assert subprocess.Popen.__init__ is init
            assert subprocess.Popen.wait is wait
            assert _cinderx._autojit_setup_depth() == 0
        """)

    def test_process_control_provider_covers_preloaded_and_public_api_paths(self):
        source = """
            import subprocess, _cinderx
            assert subprocess.Popen.__init__._cinderx_autojit_setup_provider == 'subprocess_popen'
            assert subprocess.Popen.wait._cinderx_autojit_setup_provider == 'subprocess_popen'
            assert _cinderx._autojit_setup_depth() == 0
        """
        self.run_child(source, before="import subprocess")
        self.run_child(textwrap.dedent("""
            import cinderx, subprocess
            assert not hasattr(subprocess.Popen.__init__, '_cinderx_autojit_setup_provider')
            cinderx._maybe_install_autojit_setup_provider_for_module('subprocess', 'subprocess_popen')
        """) + textwrap.dedent(source), extra_env={
            'CINDERX_AUTOJIT_SETUP_PROVIDER': 'off',
        })

    def test_process_control_provider_preserves_errors_timeout_and_io(self):
        self.run_child("""
            import subprocess, _cinderx
            depths = []
            class Process(subprocess.Popen):
                def _execute_child(self, *args, **kwargs):
                    depths.append(_cinderx._autojit_setup_depth())
                    return super()._execute_child(*args, **kwargs)
                def _wait(self, *args, **kwargs):
                    depths.append(_cinderx._autojit_setup_depth())
                    return super()._wait(*args, **kwargs)
            with Process([sys.executable, '-S', '-c', 'print(42)'],
                         stdout=subprocess.PIPE, text=True) as process:
                assert _cinderx._autojit_setup_depth() == 0
                assert process.communicate(timeout=5)[0] == '42\\n'
            assert depths and all(depth > 0 for depth in depths), depths
            try:
                Process(['/nonexistent-cinderx-process-control-test'])
            except FileNotFoundError:
                pass
            else:
                raise AssertionError('missing executable did not fail')
            assert _cinderx._autojit_setup_depth() == 0
            with Process([sys.executable, '-S', '-c', 'import time;time.sleep(10)']) as process:
                try:
                    process.wait(timeout=0.01)
                except subprocess.TimeoutExpired:
                    pass
                else:
                    raise AssertionError('wait did not time out')
                finally:
                    process.kill()
                    process.wait()
                assert _cinderx._autojit_setup_depth() == 0
            assert subprocess.check_output([sys.executable, '-S', '-c', 'print(7)']) == b'7\\n'
        """)

    def test_process_context_body_remains_eligible_for_jit(self):
        self.run_child("""
            import subprocess, time, cinderjit, _cinderx
            def business(value):
                total = 0
                for i in range(100):
                    total += value * i
                return total
            time.sleep(0.12)
            with subprocess.Popen([sys.executable, '-S', '-c', 'pass']) as process:
                assert _cinderx._autojit_setup_depth() == 0
                for _ in range(80):
                    assert business(3) == 14850
                assert cinderjit.is_jit_compiled(business)
                assert cinderjit.is_enabled()
            assert _cinderx._autojit_setup_depth() == 0
        """)

    def test_process_wait_scope_does_not_block_other_threads(self):
        self.run_child("""
            import subprocess, threading, time, cinderjit, _cinderx
            entered = threading.Event()
            release = threading.Event()
            errors = []
            class Process(subprocess.Popen):
                def _wait(self, *args, **kwargs):
                    assert _cinderx._autojit_setup_depth() > 0
                    entered.set()
                    assert release.wait(5)
                    return super()._wait(*args, **kwargs)
            def waiter(process):
                try:
                    process.wait()
                except BaseException as exc:
                    errors.append(exc)
            def business(value):
                return value + 1
            process = Process([sys.executable, '-S', '-c', 'pass'])
            thread = threading.Thread(target=waiter, args=(process,))
            thread.start()
            try:
                assert entered.wait(5)
                assert _cinderx._autojit_setup_depth() == 0
                time.sleep(0.12)
                for _ in range(80):
                    assert business(4) == 5
                assert cinderjit.is_jit_compiled(business)
            finally:
                release.set()
                thread.join(5)
            assert not thread.is_alive() and not errors, errors
        """)

    def test_process_control_provider_respects_explicit_off(self):
        self.run_child("""
            import subprocess
            assert not hasattr(subprocess.Popen.__init__, '_cinderx_autojit_setup_provider')
            assert not hasattr(subprocess.Popen.wait, '_cinderx_autojit_setup_provider')
        """, extra_env={'CINDERX_AUTOJIT_SETUP_PROVIDER': 'off'})

    def test_native_setup_descriptor_binding_keywords_and_metadata(self):
        self.run_child("""
            import _cinderx, inspect
            calls = []
            def method(self, value, *, increment=1):
                assert _cinderx._autojit_setup_depth() > 0
                calls.append((self, value))
                return value + increment
            method.__doc__ = 'original documentation'
            class Example:
                wrapped = _cinderx._autojit_setup_wrap(method, 'test_setup')
            instance = Example()
            assert instance.wrapped(3, increment=4) == 7
            assert Example.wrapped(instance, 5) == 6
            assert calls == [(instance, 3), (instance, 5)]
            assert Example.wrapped.__wrapped__ is method
            assert Example.wrapped.__name__ == method.__name__
            assert Example.wrapped.__qualname__ == method.__qualname__
            assert Example.wrapped.__doc__ == method.__doc__
            assert Example.wrapped.__code__ is method.__code__
            assert Example.wrapped.__globals__ is method.__globals__
            Example.wrapped.__kwdefaults__ = {'increment': 2}
            assert instance.wrapped(5) == 7
            assert method.__kwdefaults__ == {'increment': 2}
            Example.wrapped.custom_metadata = 'preserved'
            assert method.custom_metadata == 'preserved'
            assert inspect.signature(Example.wrapped) == inspect.signature(method)
            assert _cinderx._autojit_setup_depth() == 0
            try:
                instance.wrapped()
            except TypeError:
                pass
            else:
                raise AssertionError('missing argument was accepted')
            assert _cinderx._autojit_setup_depth() == 0
            try:
                _cinderx._autojit_setup_wrap(42, 'test_setup')
            except TypeError:
                pass
            else:
                raise AssertionError('non-callable was accepted')
        """)

    def test_process_control_provider_preserves_non_descriptor_replacements(self):
        self.run_child("""
            assert subprocess.Popen.wait is replacement
        """, before="""
            import subprocess
            class Wait:
                def __call__(self, process, timeout=None):
                    return 7
            replacement = Wait()
            subprocess.Popen.wait = replacement
        """)
        self.run_child("""
            assert subprocess.Popen is replacement
            assert not replacement.__dict__
        """, before="""
            import subprocess
            def replacement(*args, **kwargs):
                return None
            subprocess.Popen = replacement
        """)

    def test_native_setup_descriptor_does_not_add_warning_or_traceback_frames(self):
        self.run_child("""
            import _cinderx, warnings, traceback
            def target():
                warnings.warn('caller warning', stacklevel=2)
                raise ValueError('target failure')
            wrapped = _cinderx._autojit_setup_wrap(target, 'test_setup')
            def invoke(call):
                with warnings.catch_warnings(record=True) as recorded:
                    warnings.simplefilter('always')
                    try:
                        call()
                    except ValueError as exc:
                        frames = [(x.name, x.lineno) for x in traceback.extract_tb(exc.__traceback__)]
                    else:
                        raise AssertionError('target did not raise')
                warning = recorded[0]
                return warning.filename, warning.lineno, frames
            assert invoke(wrapped) == invoke(target)
            assert _cinderx._autojit_setup_depth() == 0
        """)

    def test_native_setup_descriptor_gc_cycle_and_recursion_unwind(self):
        self.run_child("""
            import _cinderx, gc, weakref
            def make_cycle():
                def target():
                    return 1
                wrapped = _cinderx._autojit_setup_wrap(target, 'test_setup')
                target.link = wrapped
                return weakref.ref(wrapped)
            reference = make_cycle()
            gc.collect()
            assert reference() is None
            factory = _cinderx._autojit_setup_wrap
            target = lambda: 1
            owner_refs = sys.getrefcount(_cinderx)
            for _ in range(1000):
                temporary = factory(target, 'test_setup')
                assert sys.getrefcount(_cinderx) == owner_refs + 1
                del temporary
            assert sys.getrefcount(_cinderx) == owner_refs
            wrapped = lambda: 1
            for _ in range(sys.getrecursionlimit() + 100):
                wrapped = _cinderx._autojit_setup_wrap(wrapped, 'test_setup')
            try:
                wrapped()
            except RecursionError:
                pass
            else:
                raise AssertionError('native wrapper chain bypassed recursion accounting')
            assert _cinderx._autojit_setup_depth() == 0
        """)


if __name__ == "__main__":
    unittest.main()
