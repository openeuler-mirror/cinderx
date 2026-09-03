import importlib.util
from pathlib import Path

import pytest


def _load_module():
    path = Path(__file__).resolve().parent / "scripts" / "check_markdown_tables.py"
    spec = importlib.util.spec_from_file_location("check_markdown_tables", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


mdt = _load_module()


def errors_for(text: str) -> list[str]:
    return [line.split(": ", 1)[1] for line in mdt.check_text("doc.md", text)]


def test_split_cells_follows_gfm_rules():
    assert mdt.split_cells("| a | b |") == ["a", "b"]
    assert mdt.split_cells("a | b") == ["a", "b"]
    assert mdt.split_cells("| a \\| b | c |") == ["a \\| b", "c"]
    assert mdt.split_cells("| `x||y` |") == ["`x", "", "y`"]
    assert mdt.split_cells("|---|---|---|") == ["---", "---", "---"]


def test_is_delimiter_row():
    assert mdt.is_delimiter_row("|---|:--:|--:|")
    assert mdt.is_delimiter_row("--|--")
    assert not mdt.is_delimiter_row("---")
    assert not mdt.is_delimiter_row("| a | b |")


def test_well_formed_tables_pass():
    text = """# Title

| a | b |
|---|---|
| 1 | 2 |
| `x\\|y` | ok |

prose with a | pipe and `x||y` in it

a | b
--|--
1 | 2

Setext heading
---

| a | b |
|---|---|
| 1 | 2 |
- list item right after a table is a new block
"""
    assert errors_for(text) == []


def test_header_delimiter_mismatch_is_reported():
    text = """| file | note |
|---|---|---|
| `x/` | desc |
"""
    errors = errors_for(text)
    assert len(errors) == 1
    assert "delimiter row has 3 column(s) but header has 2" in errors[0]


def test_row_column_count_mismatch_is_reported():
    text = """| a | b |
|---|---|
| 1 | 2 | 3 |
| 1 |
"""
    errors = errors_for(text)
    assert errors == [
        "table row has 3 column(s) but header has 2",
        "table row has 1 column(s) but header has 2",
    ]


def test_pipe_inside_code_span_is_reported_with_hint():
    text = """| a | b |
|---|---|
| `x||y` | 2 |
"""
    errors = errors_for(text)
    assert len(errors) == 1
    assert "unescaped '|' inside a code span" in errors[0]


def test_text_directly_after_table_is_reported():
    text = """| a | b |
|---|---|
| 1 | 2 |
this line becomes a row
"""
    errors = errors_for(text)
    assert len(errors) == 1
    assert "without a blank line" in errors[0]


def test_delimiter_row_without_header_is_reported():
    text = """intro

|---|---|
| 1 | 2 |
"""
    errors = errors_for(text)
    assert errors == ["table delimiter row has no header row directly above it"]


def test_fullwidth_pipe_is_reported():
    text = """｜a｜b |
|---|---|
"""
    errors = errors_for(text)
    assert any("full-width pipe" in error for error in errors)


def test_fenced_code_blocks_are_skipped():
    text = """```
| a | b |
|---|
```

~~~md
|---|---|
~~~
"""
    assert errors_for(text) == []


def test_is_candidate_excludes_vendored_trees():
    assert mdt.is_candidate("README.md")
    assert mdt.is_candidate("docs/notes.markdown")
    assert not mdt.is_candidate("cinderx/ThirdParty/x/README.md")
    assert not mdt.is_candidate("cinderx/UpstreamBorrow/README.md")
    assert not mdt.is_candidate("ci_pipeline/run_gate.py")


def test_check_file_reports_invalid_utf8(tmp_path):
    bad = tmp_path / "bad.md"
    bad.write_bytes(b"| a |\n|---|\n\xff\n")
    errors = mdt.check_file(tmp_path, "bad.md")
    assert len(errors) == 1
    assert "not valid UTF-8" in errors[0]


@pytest.mark.parametrize(
    "line",
    ["|---|---|", "| :---: | ---: |", "-|-"],
)
def test_delimiter_variants(line):
    assert mdt.is_delimiter_row(line)
