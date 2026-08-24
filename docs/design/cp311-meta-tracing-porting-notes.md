# CPython 3.11 Meta-style tracing/profile fallback

## Contract

The 3.11 execute mode follows the upstream CinderX integration policy:

1. patched `sys.settrace()` and `sys.setprofile()` call the original API;
2. an active callback pauses the JIT and parks published function entries;
3. new explicit and automatic compilation is refused while paused;
4. calls that begin under instrumentation run in the stock interpreter;
5. a transition inside an existing AArch64 JIT frame is observed at the next
   bytecode boundary and deoptimized into the vendored 3.11 interpreter;
6. suspended JIT generators deopt on their next send while instrumentation is
   active;
7. removing the final callback across all interpreter threads re-enables the
   JIT and reattaches parked artifacts.

The JIT does not implement the Python tracing event protocol in machine code.
The interpreter owns event and error delivery after fallback.

## 3.11-specific adaptation

Upstream `deoptAllJitFramesOnStack()` patches native return addresses only for
x86-64 lightweight frames.  The supported 3.11 product is AArch64 and uses
normal `_PyInterpreterFrame` ownership, so that mechanism is unavailable.
Execute-mode HIR therefore carries `CheckInstrumentation` after every
non-terminator bytecode snapshot.  The check reads `c_tracefunc` and
`c_profilefunc`; when either becomes active, normal deopt metadata reconstructs
the frame at a stable boundary and resumes it in `Ci_EvalFrameDefault_311`.

The all-boundary invariant is required because arbitrary Python can activate
instrumentation from operator methods, iterator methods, pending calls, and
destructors inserted by the refcount pass.  The native RuntimeTest
`JITContextTest.DecrefsPrecedeTheNextBoundaryPoll` pins the last case.

## Acceptance mapping

`a1_tracing_probe.py` owns T1-T8: entry-time trace fallback, compile stop,
mid-flight return, mid-flight raise, C trace-error delivery, profile fallback,
recovery after removal, and interpreter-wide final-callback ownership. T8
covers trace+trace and trace+profile on two threads: clearing the first thread
must leave the global JIT paused, and clearing the second must restore it. The
mid-flight cases execute the exact CPython `test_sys_settrace` cases in all
three tracing variants. Missing return events, extra call events, and delayed
C-trace exceptions are never baseline entries.
