# Simulates discovery scheduling and requires an AutoJIT hold while enabled.
import sys


bootstrapped = False


def bootstrap():
    global bootstrapped
    assert "_cinderx" in sys.modules
    import _cinderx

    assert _cinderx._autojit_setup_depth() == 1
    bootstrapped = True
    return ()
