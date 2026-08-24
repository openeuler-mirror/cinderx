"""Naming lint: stage numbers stay out of active acceptance code.

The acceptance vocabulary is semantic (execution, runtime_transition,
lifecycle, ...); campaign stage numbers belong to RFC history, git
history and frozen evidence only.  This lint keeps `a1_`/`A1`-style
tokens from re-entering the active surfaces, so the next campaign adds
`workload_stability`, not `a4_runner.py`.

A line that must mention a historical token (for example a validator of
frozen campaign-era evidence) carries a `naming-lint: allow` marker.
"""

import pathlib
import re

REPO = pathlib.Path(__file__).resolve().parent.parent

SCAN_ROOTS = (
    "ci_pipeline/jit311",
    "ci_pipeline/scripts",
    "cinderx/Jit",
    "cinderx/RuntimeTests",
    "cinderx/Common",
    "cinderx/Interpreter",
    "cinderx/PythonLib/test_cinderx/test_kunpeng",
)

EXTENSIONS = {".py", ".sh", ".toml", ".json", ".txt", ".cfg", ".cpp", ".h", ".c"}

# Third-party sources and the syntax corpus are not acceptance vocabulary.
EXCLUDED_PARTS = {"ThirdParty", "corpus"}

# The transition prerequisite pins frozen HISTORY: its frozen_source_files
# keys are the paths as they existed at the frozen commit (validated with
# `git show <frozen>:<path>`), so campaign-era names are the correct and
# permanent content there.
EXCLUDED_FILES = {"ci_pipeline/jit311/data/lifecycle_transition_prerequisite.json"}

FORBIDDEN = re.compile(r"\ba[1-4]_|\bA[1-4]\b")

ALLOW_MARKER = "naming-lint: allow"


def test_stage_numbers_stay_out_of_active_code():
    offenders = []
    for root in SCAN_ROOTS:
        base = REPO / root
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or path.suffix not in EXTENSIONS:
                continue
            if EXCLUDED_PARTS.intersection(path.parts):
                continue
            if str(path.relative_to(REPO)) in EXCLUDED_FILES:
                continue
            try:
                text = path.read_text()
            except UnicodeDecodeError:
                continue
            for number, line in enumerate(text.splitlines(), start=1):
                if ALLOW_MARKER in line:
                    continue
                if FORBIDDEN.search(line):
                    offenders.append(f"{path.relative_to(REPO)}:{number}: {line.strip()[:100]}")
    assert not offenders, "stage-numbered names re-entered active code:\n" + "\n".join(
        offenders[:40]
    )
