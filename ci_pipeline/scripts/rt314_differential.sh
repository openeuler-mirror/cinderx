#!/bin/bash
# The 3.14 reference-line neutrality differential (dev plan v2.1 §3.5).
#
# Builds runtime_tests for the merge base and for HEAD with identical flags,
# runs both, and requires the normalized failure sets to be isomorphic.
# import-level smoke is NOT a substitute for this check.
#
# Usage: rt314_differential.sh <base-ref> <work-dir>
# Requires: a CPython 3.14 toolchain on PATH (python3.14, cmake, ninja/make).
set -euo pipefail
# C collation for every sort/comm: committed lists are byte-ordered.
export LC_ALL=C
# Inherited GTEST_*/TESTBRIDGE_* (filter, sharding, repeat, output) can
# silently shrink or reshape what actually executes -- symmetrically, so
# the two sides would still compare equal.  The differential owns its
# execution surface.
while IFS='=' read -r name _; do
  case "$name" in GTEST_*|TESTBRIDGE_*) unset "$name" ;; esac
done < <(env)
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)

ALLOWLIST_BOOTSTRAP_COUNT=4
ALLOWLIST_BOOTSTRAP_SHA256=7dfb47d7b0488f63d87bdcfa30ecd47890cc17b1b2b71f8fef50985d05ad92c2

extract_failed() {  # $1: gtest log -> sorted Suite.Test names on stdout
  { grep -E '^\[  FAILED  \]' "$1" || true; } \
    | sed 's/^\[  FAILED  \] //; s/ ([0-9]* ms)$//' \
    | { grep -E '^[A-Za-z_][A-Za-z0-9_]*\.' || true; } | sort -u
}

extract_skipped() {  # $1: gtest log -> sorted skipped Suite.Test names
  { grep -E '^\[  SKIPPED \]' "$1" || true; } \
    | sed 's/^\[  SKIPPED \] //; s/ ([0-9]* ms)$//' \
    | { grep -E '^[A-Za-z_][A-Za-z0-9_]*\.' || true; } | sort -u
}

extract_diagnostic() {  # $1: log, $2: Suite.Test -> normalized failure section
  awk -v t="$2" '
    $0 == "[ RUN      ] " t { grab = 1 }
    grab { print }
    grab && index($0, "[  FAILED  ] " t) == 1 { exit }
  ' "$1" \
    | sed -E 's/0x[0-9a-fA-F]+/0xADDR/g; s/[0-9]+ ms/N ms/g;
              s#(/[^/ ]+)+/([^/ ]+)#<path>/\2#g;
              s/([A-Za-z0-9_.<>-]+\.(cpp|cc|h|hpp)):[0-9]+/\1:LINE/g;
              s/([A-Za-z0-9_.<>-]+\.(cpp|cc|h|hpp))",[[:space:]]*[0-9]+/\1", LINE/g'
}

compare_run_logs() {  # $1: base log, $2: head log, $3: allowlist file
  extract_failed "$1" > "$WORK/cmp-base-failed.txt"
  extract_failed "$2" > "$WORK/cmp-head-failed.txt"
  if ! diff "$WORK/cmp-base-failed.txt" "$WORK/cmp-head-failed.txt"; then
    echo "rt314-differential: NON-ZERO -- reference line behaviour changed"
    return 1
  fi
  # A skip is a behavior too: base pass -> head skip keeps the failure
  # sets equal while silently shrinking what actually ran.
  extract_skipped "$1" > "$WORK/cmp-base-skipped.txt"
  extract_skipped "$2" > "$WORK/cmp-head-skipped.txt"
  if ! diff "$WORK/cmp-base-skipped.txt" "$WORK/cmp-head-skipped.txt"; then
    echo "rt314-differential: NON-ZERO -- skipped-test sets diverged"
    return 1
  fi
  OFFENDERS=$({ grep -Ev '^[[:space:]]*(#|$)' "$3" || true; } | sort -u \
    | comm -23 "$WORK/cmp-head-failed.txt" -)
  if [ -n "$OFFENDERS" ]; then
    echo "rt314-differential: NON-ZERO -- symmetric failures outside the"
    echo "environment allowlist (broken environment or shared regression):"
    echo "$OFFENDERS"
    return 1
  fi
  # Allowlisted failures are excused only when they fail the SAME way:
  # the normalized diagnostic section must be identical on both sides
  # (dev plan section 3.5).  Source LOCATIONS (file:line) normalize away
  # -- unrelated edits shift line numbers without changing behavior --
  # both `file:line` and gtest's `file", line` AssertionHelper form --
  # while assertion text and semantic markers (the OSR interruption
  # point, expected/actual values) stay and must match.
  while IFS= read -r name; do
    extract_diagnostic "$1" "$name" > "$WORK/cmp-diag-base.txt"
    extract_diagnostic "$2" "$name" > "$WORK/cmp-diag-head.txt"
    if ! diff "$WORK/cmp-diag-base.txt" "$WORK/cmp-diag-head.txt"; then
      echo "rt314-differential: NON-ZERO -- allowlisted failure $name"
      echo "diverged in its diagnostics/interruption point"
      return 1
    fi
  done < "$WORK/cmp-head-failed.txt"
  return 0
}

if [ "${1:-}" = "--verify-logs" ]; then
  # Self-test entry: run the full log-comparison stage on two given gtest
  # logs (no builds), through the same functions the differential uses.
  WORK=$(mktemp -d)
  compare_run_logs "${2:?base log}" "${3:?head log}" \
    "${4:-$REPO_ROOT/ci_pipeline/jit311/data/rt314_env_allowed_failures.txt}"
  exit $?
fi
if [ "${1:-}" = "--verify-allowlist-growth" ]; then
  # $2: grown names; $3: names that failed on both sides.
  UNJUSTIFIED=$(comm -23 <(grep -Ev '^[[:space:]]*(#|$)' "${2:?grown}" | sort -u) \
                         <(grep -Ev '^[[:space:]]*(#|$)' "${3:?symmetric}" | sort -u))
  if [ -n "$UNJUSTIFIED" ]; then
    echo "rt314-differential: NON-ZERO -- environment allowlist grew with"
    echo "names that did not fail identically on both sides:"
    echo "$UNJUSTIFIED"
    exit 1
  fi
  exit 0
fi

BASE_REF=${1:?usage: rt314_differential.sh <base-ref> <work-dir>}
WORK=${2:?usage: rt314_differential.sh <base-ref> <work-dir>}
mkdir -p "$WORK"

# The flag set is computed ONCE, from the HEAD tree, and applied to both
# builds.  Deriving flags per-tree would let an MR that edits cmake_options
# itself build the two sides differently and stop being a differential.
FLAGS=$(cd "$REPO_ROOT" && python3.14 -c '
import sys
sys.path.insert(0, "ci_pipeline")
from cmake_options import cmake_feature_options
opts = cmake_feature_options(py_version="3.14")
print(" ".join(f"-D{k}={v}" for k, v in sorted(opts.items())))
')
if [ -n "${CINDERX_LOCAL_DEPS_DIR:-}${CINDERX_LOCAL_DEPS:-}" ]; then
  FLAGS="$FLAGS -DCINDERX_LOCAL_DEPS_DIR=${CINDERX_LOCAL_DEPS_DIR:-$CINDERX_LOCAL_DEPS}"
fi

build_and_run() {
  local tree=$1 tag=$2
  cmake -S "$tree" -B "$WORK/$tag-build" -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_RUNTIME_TESTS=ON $FLAGS \
    > "$WORK/$tag-configure.log" 2>&1
  cmake --build "$WORK/$tag-build" -j "$(nproc)" --target runtime_tests \
    > "$WORK/$tag-build.log" 2>&1
  local bin
  bin=$(find "$WORK/$tag-build" -name runtime_tests -type f | head -1)
  set +e
  # Bytecode caches must be symmetric: the head side runs from the live
  # checkout (which carries __pycache__ from ordinary development) while
  # the base side runs from a pristine archive.  That asymmetry alone
  # flips StaticSanityTest-class outcomes (measured: 6/6 green without
  # the caches, 3/6 red with them), so both sides read and write their
  # bytecode under the work dir and never touch the tree's caches.
  (cd "$tree/cinderx" && \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONPYCACHEPREFIX="$WORK/$tag-pycache" \
    "$bin") > "$WORK/$tag-tests.log" 2>&1
  echo $? > "$WORK/$tag-exit"
  set -e
  # A run that did not reach the gtest epilogue crashed or was killed;
  # name-set comparison alone must never launder that into a pass.
  if ! grep -qE '^\[==========\] .* ran\.' "$WORK/$tag-tests.log"; then
    echo "rt314-differential: $tag run did not complete (crash/kill)" >&2
    tail -n 20 "$WORK/$tag-tests.log" >&2
    exit 1
  fi
  # Exit codes: 0 (all green) or 1 (test failures, compared below by name)
  # are the only acceptable outcomes; signals and aborts are red outright.
  local code
  code=$(cat "$WORK/$tag-exit")
  if [ "$code" != 0 ] && [ "$code" != 1 ]; then
    echo "rt314-differential: $tag exited $code (abnormal termination)" >&2
    exit 1
  fi
  # Suite.Test entries only; the "N tests, listed below:" epilogue line is
  # not a failure name.  Guarded so a fully green side survives pipefail.
  { grep -E '^\[  FAILED  \]' "$WORK/$tag-tests.log" || true; } \
    | sed 's/^\[  FAILED  \] //; s/ ([0-9]* ms)$//' \
    | { grep -E '^[A-Za-z_][A-Za-z0-9_]*\.' || true; } \
    | sort -u > "$WORK/$tag-failed.txt"
  grep -E '^\[==========\] .* ran\.' "$WORK/$tag-tests.log" \
    | sed 's/^\[==========\] //; s/ (.*$//' > "$WORK/$tag-ran.txt"
  # Full registered-test identity, not just counts: equal totals cannot
  # detect one test silently replaced by another.
  "$bin" --gtest_list_tests 2>/dev/null \
    | awk '/^[A-Za-z_][A-Za-z0-9_]*\./ { suite = $1 }
           /^  [A-Za-z_]/ { print suite $1 }' \
    | sort -u > "$WORK/$tag-registered.txt"
  # The run must cover the full enabled population: base/head equality
  # alone would accept a filter or shard that shrank both sides alike.
  ENABLED=$(grep -cv 'DISABLED_' "$WORK/$tag-registered.txt")
  RAN=$(sed -E 's/^([0-9]+) tests? from.*/\1/' "$WORK/$tag-ran.txt")
  if [ "$RAN" != "$ENABLED" ]; then
    echo "rt314-differential: $tag ran $RAN tests but $ENABLED are enabled"
    echo "(a filter/shard shrank the execution surface)"
    exit 1
  fi
}

mkdir -p "$WORK/base-tree"
git -C "$REPO_ROOT" archive "$BASE_REF" | tar -x -C "$WORK/base-tree"

# openeuler/cinderx#176 (e40be55): googletest is resolved by tag, not by a
# mirror-specific SHA.  The 3.14 reference line may still pin 262727f0
# from !174; that object exists only in curated github.com mirrors and is
# absent from public upstream v1.17.0 (52eb8108...).  Rewrite the archived
# base pin to the tag so both trees configure.  RuntimeTests sources and
# hir_tests goldens are untouched.
_gtest_cmake="$WORK/base-tree/cinderx/RuntimeTests/CMakeLists.txt"
if [ -f "$_gtest_cmake" ]; then
  sed -i -E 's/[0-9a-f]{40}[[:space:]]+# tag (v[0-9.]+)/\1/' "$_gtest_cmake"
fi

# Golden texts first (cheap, fail-fast): hir_test golden output IS observed
# behavior, and a change that also updates the goldens keeps both failure
# sets equal -- the differential is structurally blind to it.  Any golden
# delta therefore refuses mechanical certification and demands explicit
# reconciliation in the MR.
if ! diff -r "$WORK/base-tree/cinderx/RuntimeTests/hir_tests" \
     "$REPO_ROOT/cinderx/RuntimeTests/hir_tests" > "$WORK/golden.diff" 2>&1; then
  echo "rt314-differential: NON-ZERO -- hir_test golden texts changed;"
  echo "golden change is behavior change and must be reconciled explicitly"
  head -n 40 "$WORK/golden.diff"
  exit 1
fi

# The environment allowlist may grow only by registering failures that
# already fail identically on both sides (documenting a reference-line
# environment fact).  Growing it with a name that is not a symmetric
# failure would wash a HEAD-only regression or pad the list.
BASE_ALLOWLIST="$WORK/base-tree/ci_pipeline/jit311/data/rt314_env_allowed_failures.txt"
HEAD_ALLOWLIST="$REPO_ROOT/ci_pipeline/jit311/data/rt314_env_allowed_failures.txt"
: > "$WORK/allowlist-grown.txt"
if [ -f "$BASE_ALLOWLIST" ]; then
  comm -13 <(grep -Ev '^[[:space:]]*(#|$)' "$BASE_ALLOWLIST" | sort -u) \
           <(grep -Ev '^[[:space:]]*(#|$)' "$HEAD_ALLOWLIST" | sort -u) \
    > "$WORK/allowlist-grown.txt" || true
else
  # Bootstrap window: while the allowlist does not exist at the base, the
  # committed list must byte-match the audited pin (the plan's four known
  # environment failures); once the file lands at the base, the tree
  # comparison above takes over and the pin becomes inert.
  NORMALIZED_AL="$WORK/allowlist-normalized.txt"
  grep -Ev '^[[:space:]]*(#|$)' "$HEAD_ALLOWLIST" > "$NORMALIZED_AL"
  AL_COUNT=$(wc -l < "$NORMALIZED_AL" | tr -d ' ')
  AL_SHA=$(sha256sum "$NORMALIZED_AL" | awk '{print $1}')
  if [ "$AL_COUNT" != "$ALLOWLIST_BOOTSTRAP_COUNT" ] \
     || [ "$AL_SHA" != "$ALLOWLIST_BOOTSTRAP_SHA256" ]; then
    echo "rt314-differential: NON-ZERO -- allowlist absent at base and the"
    echo "committed list does not match the audited bootstrap pin"
    echo "($AL_COUNT entries, sha256 $AL_SHA)"
    exit 1
  fi
  echo "rt314-differential: bootstrap allowlist matches the audited pin" \
    "($ALLOWLIST_BOOTSTRAP_COUNT entries)"
fi

build_and_run "$WORK/base-tree" base
build_and_run "$REPO_ROOT" head

# Identical failure sets are not enough on their own: a test that silently
# disappears while green keeps the failure sets equal but shrinks coverage.
if ! diff "$WORK/base-registered.txt" "$WORK/head-registered.txt"; then
  echo "rt314-differential: NON-ZERO -- registered test identity changed"
  exit 1
fi
if ! diff "$WORK/base-ran.txt" "$WORK/head-ran.txt"; then
  echo "rt314-differential: NON-ZERO -- total test count changed"
  exit 1
fi
if [ -s "$WORK/allowlist-grown.txt" ]; then
  comm -12 "$WORK/base-failed.txt" "$WORK/head-failed.txt" \
    > "$WORK/symmetric-failed.txt"
  UNJUSTIFIED=$(comm -23 "$WORK/allowlist-grown.txt" "$WORK/symmetric-failed.txt")
  if [ -n "$UNJUSTIFIED" ]; then
    echo "rt314-differential: NON-ZERO -- environment allowlist grew with"
    echo "names that did not fail identically on both sides:"
    echo "$UNJUSTIFIED"
    exit 1
  fi
fi
compare_run_logs "$WORK/base-tests.log" "$WORK/head-tests.log" \
  "$REPO_ROOT/ci_pipeline/jit311/data/rt314_env_allowed_failures.txt" || exit 1
echo "rt314-differential: ZERO (identity equal, failure/skip sets isomorphic,"
echo "allowlisted, diagnostics aligned)"
