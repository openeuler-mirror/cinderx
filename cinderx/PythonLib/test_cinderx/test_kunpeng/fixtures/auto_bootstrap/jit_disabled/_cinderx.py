# Simulates the minimal _cinderx module used when JIT startup is disabled.
_setup_depth = 0


def has_parallel_gc():
    return False


def _autojit_setup_enter():
    global _setup_depth
    _setup_depth += 1


def _autojit_setup_leave():
    global _setup_depth
    assert _setup_depth > 0
    _setup_depth -= 1


def _autojit_setup_depth():
    return _setup_depth
