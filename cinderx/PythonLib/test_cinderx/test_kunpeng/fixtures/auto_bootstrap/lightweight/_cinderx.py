# Simulates the native _cinderx APIs required by lightweight bootstrap.
_setup_depth = 0


def install_frame_evaluator():
    pass


def _autojit_import_enter():
    pass


def _autojit_import_leave():
    pass


def _autojit_import_depth():
    return 0


def _autojit_import_scope_depth():
    return 0


def _autojit_setup_enter():
    global _setup_depth
    _setup_depth += 1


def _autojit_setup_leave():
    global _setup_depth
    assert _setup_depth > 0
    _setup_depth -= 1


def _autojit_setup_depth():
    return _setup_depth
