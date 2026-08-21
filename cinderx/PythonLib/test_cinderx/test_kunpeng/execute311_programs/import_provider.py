"""A real import must raise the depth the scheduler reads; the product
configuration installs both providers."""
import sys

import _cinderx
import cinderx

from _harness import emit

depths = []


class Probe:
    # Imported for real, from inside the import machinery, so the depth
    # it reads is the one a real import produced.
    def find_module(self, name, path=None):
        depths.append(_cinderx._autojit_import_depth())
        return None


sys.meta_path.insert(0, Probe())
import shlex          # noqa: F401,E402  (any unimported module)
sys.meta_path.pop(0)

emit(
    import_provider=cinderx._autojit_import_provider(),
    setup_provider=cinderx._autojit_setup_provider(),
    depth_during_import=max(depths) if depths else 0,
    depth_after=_cinderx._autojit_import_depth(),
)
