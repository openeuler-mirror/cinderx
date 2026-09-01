# Simulates discovery scheduling and requires it to precede JIT initialization.
import sys


assert "_cinderx" not in sys.modules
assert "cinderjit" not in sys.modules

bootstrapped = False


def bootstrap():
    global bootstrapped
    bootstrapped = True
    return ()
