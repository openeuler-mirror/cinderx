#!/usr/bin/env python3
"""Check Markdown tables for structural errors that break GFM rendering.

This is a dependency-free (standard library only) checker intended for the
pre-commit gate. It targets the failure modes that make a table render as raw
text on GitHub/GitCode, rather than general style rules:

  * header row and delimiter row cell counts differ (GFM then does not
    recognise the block as a table at all);
  * a body row has more or fewer cells than the header (GFM silently pads or
    truncates, losing content);
  * a delimiter row without a header row directly above it;
  * an unescaped pipe inside a code span (GFM splits cells on it anyway);
  * a full-width pipe (U+FF5C) used in place of ASCII '|';
  * non-blank text immediately after a table without a blank line, which GFM
    swallows into the table as extra rows.

Like check_clean_code_incremental.py, the default mode checks files changed
relative to --base; use --all for the whole tracked tree, or pass explicit
paths.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys
from typing import Iterable

EXCLUDED_PREFIXES = (
    "ThirdParty/",
    "cinderx/ThirdParty/",
    "cinderx/Interpreter/3.11/upstream/",
    "cinderx/UpstreamBorrow/",
)

MARKDOWN_SUFFIXES = {".md", ".markdown"}

FULLWIDTH_PIPE = "｜"

FENCE_RE = re.compile(r"^ {0,3}(`{3,}|~{3,})")
DELIMITER_CELL_RE = re.compile(r"^:?-+:?$")
BLOCK_START_RE = re.compile(r"^ {0,3}(#{1,6}(\s|$)|>|[-*+]\s|\d{1,9}[.)]\s|`{3,}|~{3,}|<)")


def git_lines(args: list[str], *, cwd: Path) -> list[str]:
    result = subprocess.run(
        ["git", *args], cwd=cwd, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def is_candidate(path: str) -> bool:
    normalized = path.replace("\\", "/")
    if normalized.startswith(EXCLUDED_PREFIXES):
        return False
    return Path(normalized).suffix.lower() in MARKDOWN_SUFFIXES


def candidate_files(cwd: Path, files: Iterable[str]) -> list[str]:
    return sorted(path for path in set(files) if is_candidate(path) and (cwd / path).is_file())


def changed_files(cwd: Path, base: str, include_untracked: bool) -> list[str]:
    files: set[str] = set()
    files.update(git_lines(["diff", "--name-only", "--diff-filter=ACMRT", f"{base}...HEAD"], cwd=cwd))
    files.update(git_lines(["diff", "--name-only", "--diff-filter=ACMRT"], cwd=cwd))
    files.update(git_lines(["diff", "--cached", "--name-only", "--diff-filter=ACMRT"], cwd=cwd))
    if include_untracked:
        files.update(git_lines(["ls-files", "--others", "--exclude-standard"], cwd=cwd))
    return candidate_files(cwd, files)


def all_tracked_files(cwd: Path) -> list[str]:
    return candidate_files(cwd, git_lines(["ls-files"], cwd=cwd))


def split_cells(line: str) -> list[str]:
    """Split a table row into cells following GFM rules.

    Leading/trailing whitespace is ignored, one optional leading and trailing
    pipe is dropped, and cells are split on every unescaped '|' -- including
    pipes inside code spans, which is exactly what GFM does.
    """
    text = line.strip()
    cells: list[str] = []
    current: list[str] = []
    escaped = False
    for char in text:
        if escaped:
            current.append(char)
            escaped = False
        elif char == "\\":
            current.append(char)
            escaped = True
        elif char == "|":
            cells.append("".join(current))
            current = []
        else:
            current.append(char)
    cells.append("".join(current))

    if text.startswith("|"):
        cells = cells[1:]
    if text.endswith("|") and not text.endswith("\\|") and len(cells) > 0:
        cells = cells[:-1]
    return [cell.strip() for cell in cells]


def is_delimiter_row(line: str) -> bool:
    text = line.strip()
    if "|" not in text or "-" not in text:
        return False
    cells = split_cells(text)
    if not cells:
        return False
    return all(DELIMITER_CELL_RE.match(cell.replace(" ", "")) for cell in cells)


def has_pipe_inside_code_span(line: str) -> bool:
    in_code = False
    escaped = False
    for char in line:
        if escaped:
            escaped = False
            continue
        if char == "\\":
            escaped = True
            continue
        if char == "`":
            in_code = not in_code
        elif char == "|" and in_code:
            return True
    return False


def check_text(path: str, text: str) -> list[str]:
    errors: list[str] = []
    lines = text.splitlines()
    total = len(lines)

    def report(lineno: int, message: str) -> None:
        errors.append(f"{path}:{lineno}: {message}")

    fence: str | None = None
    i = 0
    while i < total:
        line = lines[i]
        lineno = i + 1

        fence_match = FENCE_RE.match(line)
        if fence is not None:
            if fence_match and fence_match.group(1)[0] == fence[0] and len(fence_match.group(1)) >= len(fence):
                fence = None
            i += 1
            continue
        if fence_match:
            fence = fence_match.group(1)
            i += 1
            continue

        if FULLWIDTH_PIPE in line and "|" in line:
            report(lineno, "full-width pipe (U+FF5C) mixed with ASCII '|'; use ASCII '|' in tables")

        next_line = lines[i + 1] if i + 1 < total else None
        if next_line is None or not line.strip() or not is_delimiter_row(next_line):
            if is_delimiter_row(line) and (i == 0 or not lines[i - 1].strip() or is_delimiter_row(lines[i - 1])):
                report(lineno, "table delimiter row has no header row directly above it")
            i += 1
            continue

        header_cells = split_cells(line)
        delimiter_cells = split_cells(next_line)
        column_count = len(header_cells)

        if column_count != len(delimiter_cells):
            report(
                lineno + 1,
                f"table delimiter row has {len(delimiter_cells)} column(s) but header has {column_count}; "
                "GFM will not render this block as a table",
            )
        if has_pipe_inside_code_span(line):
            report(lineno, "unescaped '|' inside a code span splits the cell; write '\\|'")

        j = i + 2
        while j < total:
            row = lines[j]
            if not row.strip():
                break
            if "|" not in row:
                if BLOCK_START_RE.match(row):
                    break
                report(
                    j + 1,
                    "text directly after a table without a blank line is rendered as a table row; "
                    "insert a blank line",
                )
                break
            if column_count == len(delimiter_cells):
                row_cells = split_cells(row)
                if len(row_cells) != column_count:
                    hint = " (an unescaped '|' inside a code span?)" if has_pipe_inside_code_span(row) else ""
                    report(j + 1, f"table row has {len(row_cells)} column(s) but header has {column_count}{hint}")
                elif has_pipe_inside_code_span(row):
                    report(j + 1, "unescaped '|' inside a code span splits the cell; write '\\|'")
            j += 1

        i = j

    return errors


def check_file(cwd: Path, path: str) -> list[str]:
    try:
        text = (cwd / path).read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        return [f"{path}:0: not valid UTF-8 ({exc})"]
    return check_text(path, text)


def main() -> int:
    parser = argparse.ArgumentParser(description="Check Markdown tables for GFM rendering errors.")
    parser.add_argument("paths", nargs="*", help="explicit Markdown files to check; overrides incremental mode")
    parser.add_argument("--base", default="origin/master", help="base ref for incremental checks, default: origin/master")
    parser.add_argument("--all", action="store_true", help="check all tracked Markdown files instead of only changed ones")
    parser.add_argument("--no-untracked", action="store_true", help="do not include untracked files in the changed set")
    args = parser.parse_args()

    cwd = Path.cwd()
    try:
        repo = Path(git_lines(["rev-parse", "--show-toplevel"], cwd=cwd)[0])
    except subprocess.CalledProcessError as exc:
        print(exc.stderr, file=sys.stderr, end="")
        return exc.returncode

    try:
        if args.paths:
            files = [p for p in args.paths if is_candidate(p) and (repo / p).is_file()]
        elif args.all:
            files = all_tracked_files(repo)
        else:
            files = changed_files(repo, args.base, include_untracked=not args.no_untracked)
    except subprocess.CalledProcessError as exc:
        print(exc.stderr, file=sys.stderr, end="")
        print(
            f"error: cannot compute changed files against {args.base!r}; "
            "pass --base <ref>, explicit paths, or --all",
            file=sys.stderr,
        )
        return 2

    if not files:
        print("markdown-tables: no Markdown files to check")
        return 0

    errors: list[str] = []
    for path in files:
        errors.extend(check_file(repo, path))

    print(f"markdown-tables: checked {len(files)} file(s)")
    if errors:
        for error in errors:
            print(error)
        print(f"markdown-tables: {len(errors)} error(s)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
