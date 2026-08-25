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
    "ci_pipeline",
    "docs/design",
    "cinderx/Jit",
    "cinderx/RuntimeTests",
    "cinderx/Common",
    "cinderx/Interpreter",
    "cinderx/PythonLib/test_cinderx/test_kunpeng",
)

EXTENSIONS = {".py", ".sh", ".toml", ".json", ".txt", ".cfg", ".cpp", ".h", ".c", ".md"}

# Third-party sources and the syntax corpus are not acceptance vocabulary;
# `frozen` directories hold byte-locked historical evidence whose digests
# are pinned, so their campaign-era content is permanent by design.
EXCLUDED_PARTS = {"ThirdParty", "corpus", "frozen", "hir_tests"}

# The transition prerequisite pins frozen HISTORY: its frozen_source_files
# keys are the paths as they existed at the frozen commit (validated with
# `git show <frozen>:<path>`), so campaign-era names are the correct and
# permanent content there.  The lint excludes itself: it must be able to
# name the tokens it forbids.
EXCLUDED_FILES = {
    "ci_pipeline/jit311/data/lifecycle_transition_prerequisite.json",
    "ci_pipeline/test_semantic_naming.py",
}

# A campaign token is a stage number fused to a word: `a1_report`,
# `A2_COVERAGE_GAP`, `cp311-a1-deviations`, `A1ReportTest`, or the bare
# word `A1`.  A stage-free identifier that merely ends in a digit (`a1`
# as an argument name in vendored fixtures) is legitimate code and does
# not match.
FORBIDDEN = re.compile(
    r"\b[aA][1-4]_[A-Za-z]|\b[aA][1-4]-[A-Za-z]|\bA[1-4][A-Z][a-z]|\bA[1-4]\b"
)

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
            relative = path.relative_to(REPO)
            if str(relative) in EXCLUDED_FILES:
                continue
            # Only the cp311 campaign documents carry the naming contract;
            # unrelated design docs may use A1/A2 as diagram vertex ids.
            if relative.parts[:2] == ("docs", "design") and not path.name.startswith(
                "cp311-"
            ):
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
