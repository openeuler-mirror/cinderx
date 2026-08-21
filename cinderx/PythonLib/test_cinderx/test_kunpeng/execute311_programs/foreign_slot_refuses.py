"""Register a foreign code-extra slot BEFORE CinderX loads: every watching
mode must refuse the takeover rather than risk the dealloc walk."""
import ctypes
import json
import os

CALLS = []
FREEFUNC = ctypes.CFUNCTYPE(None, ctypes.c_voidp)


def _free(ptr):
    CALLS.append(ptr)


_callback = FREEFUNC(_free)
capi = ctypes.pythonapi
capi._PyEval_RequestCodeExtraIndex.restype = ctypes.c_ssize_t
foreign = capi._PyEval_RequestCodeExtraIndex(_callback)

refused = None
try:
    import _cinderx
    import cinderx
    cinderx.init()
    _cinderx.install_frame_evaluator()
except RuntimeError as exc:
    refused = str(exc)


def target(a, b):
    return a + b


T = int(os.environ.get("PYTHONJITAUTO", "50"))
for i in range(T * 2):
    assert target(i, 2) == i + 2

print("JOURNAL " + json.dumps({
    "foreign": foreign,
    "refused": refused,
    "foreign_calls": len(CALLS),
}))
