"""Attestation for the -R run (installed as sitecustomize by
run_refleak_311.sh).  It only records; the evaluator is installed by the
product startup path, so what is attested is that path working."""
import atexit
import os


def _attest():
    ledger = os.environ.get("CINDERX_REFLEAK_LEDGER")
    if not ledger:
        return
    try:
        import _cinderx

        stats = _cinderx._get_trigger_stats()
        line = "%d %s %d %d\n" % (
            os.getpid(),
            bool(_cinderx.is_frame_evaluator_installed()),
            stats["machine_code_entries"],
            stats["compiled_function_creations"],
        )
    except Exception:
        line = "%d unavailable 0 0\n" % os.getpid()
    try:
        with open(ledger, "a", encoding="utf-8") as fh:
            fh.write(line)
    except OSError:
        pass


atexit.register(_attest)
