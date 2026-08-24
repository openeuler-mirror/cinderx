"""sitecustomize for the verification run: record block/quickened
counts at each regrtest cleanup (installed by quickened_artifact.py)."""

import atexit, gc, os, sys
from array import array
_OUT = os.environ["QA_OUT"]
_SKIP = int(os.environ["QA_SKIP"])
try:
    from test.libregrtest import refleak
except Exception as exc:
    open(_OUT, "w").write("hook-failed %r\n" % (exc,))
else:
    _orig = refleak.dash_R_cleanup
    blocks = array("q")
    quick = array("q")
    call = [0]

    def _patched(*a, **k):
        r = _orig(*a, **k)
        gc.collect()
        call[0] += 1
        if call[0] >= _SKIP:
            blocks.append(sys.getallocatedblocks())
            quick.append(sys._getquickenedcount())
        return r

    refleak.dash_R_cleanup = _patched

    @atexit.register
    def _dump():
        if len(blocks) >= 2:
            with open(_OUT, "w") as fh:
                fh.write("blocks %s\n" % " ".join(str(v) for v in blocks))
                fh.write("quick %s\n" % " ".join(str(v) for v in quick))
