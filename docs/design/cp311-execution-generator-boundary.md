# CPython 3.11 JIT synchronous-generator boundary

Synchronous generator functions remain a supported JIT capability.  Their
instances use CinderX's extended `JitGenObject` layout so suspended machine
state can retain a `GenDataFooter`; consequently the object is not a subtype
of CPython's static `PyGen_Type`.

The signal/yield-from failure occurs before signal delivery:
`_testcapi.raise_SIGINT_then_send_None()` parses its argument with
`O! &PyGen_Type` and rejects a suspended JIT generator with `TypeError`.
The supported native-C-API boundary is therefore explicit deoptimization:
call `cinderx.jit._deopt_gen()` on the suspended object before passing it to an
API that requires `PyGen_Type`.  This converts the object and reifies its
interpreter continuation before the C API sends into it.

The standard CinderX CPython lib-test runner already applies this narrow
adapter for `_testcapi.raise_SIGINT_then_send_None`.  The execution acceptance's C lane stages the same
adapter, and `generator_execution_probe.py` separately proves:

- normal yield-from;
- send and throw through yield-from;
- JIT machine entry before the boundary;
- successful conversion to exact `types.GeneratorType`;
- signal delivery ending in `StopIteration("PASSED")`.

This is not a compatibility baseline and it does not disable generator JIT.
An arbitrary third-party extension that accepts only exact `PyGen_Type` must
use the same deopt boundary; transparent binary interception is not possible
with the extended object layout.
