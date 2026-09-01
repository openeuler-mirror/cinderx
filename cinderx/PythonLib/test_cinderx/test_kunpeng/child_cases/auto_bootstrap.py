import argparse
import sys


def lightweight_bootstrap() -> None:
    import _cinderx_auto  # noqa: F401
    import _cinderx_plugins_bootstrap

    assert "_cinderx_plugins_bootstrap" in sys.modules
    assert _cinderx_plugins_bootstrap.bootstrapped
    assert "cinderx" not in sys.modules
    assert "_cinderx" in sys.modules
    if sys.version_info[:2] == (3, 11):
        assert "cinderjit" not in sys.modules
    else:
        assert "cinderjit" in sys.modules


def jit_disabled() -> None:
    import _cinderx_auto  # noqa: F401

    assert "_cinderx" in sys.modules
    assert "cinderjit" not in sys.modules


def discovery_only_zero_candidate() -> None:
    import _cinderx_auto  # noqa: F401

    assert "_cinderx_plugins_bootstrap" in sys.modules
    assert "cinderx" not in sys.modules
    assert "_cinderx" not in sys.modules


def plugin_candidate() -> None:
    import _cinderx_auto  # noqa: F401
    import _cinderx_plugins_bootstrap

    results = _cinderx_plugins_bootstrap.bootstrap()
    assert len(results) == 1
    assert results[0].available
    assert results[0].manifest.id == "candidate"
    assert "never_imported" not in sys.modules
    assert "never_imported.adapter" not in sys.modules
    assert "never_imported_target" not in sys.modules
    assert _cinderx_plugins_bootstrap.bootstrap() is results


CASES = {
    "discovery-only-zero-candidate": discovery_only_zero_candidate,
    "jit-disabled": jit_disabled,
    "lightweight-bootstrap": lightweight_bootstrap,
    "plugin-candidate": plugin_candidate,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("case", choices=sorted(CASES))
    args = parser.parse_args()
    CASES[args.case]()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
