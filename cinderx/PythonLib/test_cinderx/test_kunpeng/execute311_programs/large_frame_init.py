# Copyright (c) Meta Platforms, Inc. and affiliates.

"""A function whose frame holds more than 4095 localsplus slots: the inline
frame initialisation zeroes every slot with immediate-offset stores, and the
slots past 32760 bytes need the address resolved through a scratch register
rather than an unencodable immediate."""

from _harness import T, emit
import cinderjit

N = 4200
params = ", ".join(f"a{i}" for i in range(N))
ns = {}
exec(f"def wide({params}):\n    return ({params},)\n", ns)
wide = ns["wide"]

args = tuple(range(N))
result = None
for _ in range(T + 3):
    result = wide(*args)
emit(
    compiled=cinderjit.is_jit_compiled(wide),
    length=len(result),
    first=result[0],
    last=result[-1],
    mid=result[4096],
)
