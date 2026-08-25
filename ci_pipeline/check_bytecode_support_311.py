#!/usr/bin/env python3.11
"""Consistency gate for the CPython 3.11 bytecode support list.

The support list (cinderx/Interpreter/3.11/bytecode_support.toml) is the
single fact source for how the 3.11 JIT front end treats every opcode.  This
checker keeps it honest against the interpreter that actually runs the
product:

  * completeness -- every opcode the anchored CPython 3.11 defines has
    exactly one row, and no row names an opcode that does not exist;
  * cache widths -- each row's inline-cache entry count matches the
    interpreter's own table, because one wrong width desynchronizes every
    later instruction in the stream;
  * normalization -- rows marked "normalize" map to the base opcode the
    interpreter's specialization families define, and only to a base that is
    itself translated;
  * state discipline -- states come from the closed four-value set, refusal
    rows carry a registered opcode reason, translated rows carry none;
  * refusal registry -- opcode and whole-code-object shape reasons live in
    separate, closed namespaces in the support list;
  * front-end reachability -- translated rows (and normalization targets) are
    admitted by isSupportedOpcode() and have a buildHIRImpl translation case,
    while refusal rows are admitted by neither.

Ground truth is the running interpreter's opcode module plus the vendored
pristine opcode_targets.h; the gate runs this under the anchored 3.11.6.
The C++ comparison evaluates the small set of preprocessor conditions in the
two audited functions as a CPython 3.11 build with Lazy Imports disabled.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

STATES = ("translate", "normalize", "refuse", "interpreter-only")
REASON_CATEGORIES = ("opcode", "shape")

REPO_ROOT = Path(__file__).resolve().parent.parent
SUPPORT_LIST = "cinderx/Interpreter/3.11/bytecode_support.toml"
OPCODE_TARGETS = "cinderx/Interpreter/3.11/upstream/opcode_targets.h"
HIR_BUILDER = "cinderx/Jit/hir/builder.cpp"
BYTECODE_DECODER = "cinderx/Jit/bytecode.cpp"

# BytecodeInstructionBlock folds EXTENDED_ARG into the following instruction,
# so it is intentionally accepted by the support predicate but can never have
# a case in HIRBuilder::translate().  This is the sole dispatch exception.
DECODER_CONSUMED = frozenset({"EXTENDED_ARG"})


def _mask_cpp_comments_and_literals(source: str) -> str:
    """Replace comments and quoted literals while preserving source offsets."""
    token = re.compile(
        r"//[^\r\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
        re.DOTALL,
    )

    def mask(match: re.Match[str]) -> str:
        return "".join("\n" if char == "\n" else " " for char in match.group())

    return token.sub(mask, source)


def _find_braced_body(source: str, start: int) -> str:
    masked = _mask_cpp_comments_and_literals(source)
    open_brace = masked.find("{", start)
    if open_brace < 0:
        raise ValueError("opening brace not found")
    depth = 1
    for pos in range(open_brace + 1, len(masked)):
        if masked[pos] == "{":
            depth += 1
        elif masked[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[open_brace + 1 : pos]
    raise ValueError("closing brace not found")


def _function_body(source: str, signature: str) -> str:
    masked = _mask_cpp_comments_and_literals(source)
    start = masked.find(signature)
    if start < 0:
        raise ValueError(f"function {signature!r} not found")
    return _find_braced_body(source, start + len(signature))


def _eval_cpp_condition(expression: str, macros: dict[str, int]) -> bool:
    expression = re.sub(
        r"\bdefined\s*(?:\(\s*(\w+)\s*\)|(\w+))",
        lambda match: "1"
        if (match.group(1) or match.group(2)) in macros
        else "0",
        expression,
    )
    expression = re.sub(
        r"\b[A-Za-z_]\w*\b",
        lambda match: str(macros.get(match.group(), 0)),
        expression,
    )
    expression = expression.replace("&&", " and ").replace("||", " or ")
    expression = re.sub(r"!(?!=)", " not ", expression)
    if not re.fullmatch(r"[\s0-9a-fA-FxX()<>!=&|+*/%~.\-andornot]+", expression):
        raise ValueError(f"unsupported preprocessor expression {expression!r}")
    return bool(eval(expression, {"__builtins__": {}}, {}))


def _preprocess_cpp_311(source: str) -> str:
    """Select the CPython 3.11, non-Lazy-Imports arms of a C++ fragment."""
    # ENABLE_LAZY_IMPORTS is deliberately absent: the 3.11 gate build leaves
    # the macro undefined rather than defining it to zero.
    macros = {"PY_VERSION_HEX": 0x030B0000}
    active = True
    # Each entry is [parent_active, an earlier sibling condition matched].
    stack: list[list[bool]] = []
    output: list[str] = []
    for line in source.splitlines(keepends=True):
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if directive is None:
            if active:
                output.append(line)
            continue

        kind, argument = directive.groups()
        argument = argument.strip()
        if kind in ("if", "ifdef", "ifndef"):
            if kind == "if":
                condition = _eval_cpp_condition(argument, macros)
            else:
                condition = argument in macros
                if kind == "ifndef":
                    condition = not condition
            stack.append([active, condition])
            active = active and condition
        elif kind == "elif":
            if not stack:
                raise ValueError("#elif without #if")
            parent_active, branch_taken = stack[-1]
            condition = _eval_cpp_condition(argument, macros)
            active = parent_active and not branch_taken and condition
            stack[-1][1] = branch_taken or condition
        elif kind == "else":
            if not stack:
                raise ValueError("#else without #if")
            parent_active, branch_taken = stack[-1]
            active = parent_active and not branch_taken
            stack[-1][1] = True
        else:
            if not stack:
                raise ValueError("#endif without #if")
            parent_active, _ = stack.pop()
            active = parent_active
    if stack:
        raise ValueError("unterminated preprocessor conditional")
    return "".join(output)


def _opcode_switch_cases(
    function_body: str, switch_re: str = r"\bswitch\s*\(\s*opcode\s*\)\s*\{"
) -> set[str]:
    """Return top-level cases from the unique opcode switch in a function."""
    body = _preprocess_cpp_311(function_body)
    masked = _mask_cpp_comments_and_literals(body)
    switches = list(re.finditer(switch_re, masked))
    if len(switches) != 1:
        raise ValueError(
            f"expected one opcode switch matching {switch_re}, "
            f"found {len(switches)}"
        )
    switch_body = _find_braced_body(body, switches[0].start())
    masked_switch = _mask_cpp_comments_and_literals(switch_body)

    cases: set[str] = set()
    depth = 0
    case_at = re.compile(r"\bcase\s+([A-Z][A-Z0-9_]*)\s*:")
    pos = 0
    for match in case_at.finditer(masked_switch):
        for char in masked_switch[pos : match.start()]:
            if char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
        if depth == 0:
            cases.add(match.group(1))
        pos = match.end()
    return cases


_REASON_SWITCH_RE = (
    r"\bswitch\s*\(\s*bc_it\s*->\s*opcode\s*\(\s*\)\s*\)\s*\{"
)


def _reason_opcode_cases(builder_source: str) -> set[str]:
    """Cases in unsupportedOpcodeReason311's bytecode opcode switch."""
    return _opcode_switch_cases(
        _function_body(
            builder_source,
            "const char* unsupportedOpcodeReason311(BorrowedRef<PyCodeObject> code)",
        ),
        _REASON_SWITCH_RE,
    )


def frontend_opcode_sets(builder_source: str) -> tuple[set[str], set[str]]:
    supported = _opcode_switch_cases(
        _function_body(builder_source, "bool isSupportedOpcode(int opcode)")
    )
    dispatched = _opcode_switch_cases(
        _function_body(builder_source, "void HIRBuilder::translate(")
    )
    return supported, dispatched


def validate_frontend(
    rows: dict[str, dict], builder_source: str, decoder_source: str
) -> list[str]:
    errors: list[str] = []
    try:
        supported, dispatched = frontend_opcode_sets(builder_source)
        reason_cases = _reason_opcode_cases(builder_source)
    except ValueError as exc:
        return [f"HIR builder source: {exc}"]

    try:
        opcode_body = _function_body(
            decoder_source, "int BytecodeInstruction::opcode() const"
        )
        word_body = _function_body(
            decoder_source, "_Py_CODEUNIT BytecodeInstruction::word() const"
        )
        calc_body = _function_body(
            decoder_source,
            "void BytecodeInstruction::calcOpcodeOffsetAndOparg() const",
        )
    except ValueError as exc:
        return [f"bytecode decoder source: {exc}"]

    if not re.search(r"_Py_OPCODE\s*\(\s*word\s*\(\s*\)\s*\)", opcode_body):
        errors.append("bytecode decoder: opcode() does not consume word()")
    if not re.search(
        r"unspecialize\s*\(\s*uninstrumentedOpcode\s*\(\s*\)\s*\)",
        word_body,
    ):
        errors.append(
            "bytecode decoder: word() does not normalize specialized opcodes"
        )
    if not re.search(
        r"while\s*\(\s*_Py_OPCODE\s*\(\s*word\s*\(\s*\)\s*\)\s*"
        r"==\s*EXTENDED_ARG\s*\)",
        calc_body,
    ):
        errors.append("bytecode decoder: EXTENDED_ARG is not consumed")

    for name, row in sorted(rows.items()):
        state = row.get("state")
        if state == "translate":
            if name not in supported:
                errors.append(
                    f"opcode {name}: translate row missing from "
                    "isSupportedOpcode"
                )
            if name not in DECODER_CONSUMED and name not in dispatched:
                errors.append(
                    f"opcode {name}: translate row missing from "
                    "buildHIRImpl dispatch"
                )
        elif state == "normalize":
            target = row.get("to")
            if name in supported:
                errors.append(
                    f"opcode {name}: normalize row admitted directly by "
                    "isSupportedOpcode"
                )
            if name in dispatched:
                errors.append(
                    f"opcode {name}: normalize row present directly in "
                    "buildHIRImpl dispatch"
                )
            if target not in supported:
                errors.append(
                    f"opcode {name}: normalize target {target!r} missing from "
                    "isSupportedOpcode"
                )
            if target not in DECODER_CONSUMED and target not in dispatched:
                errors.append(
                    f"opcode {name}: normalize target {target!r} missing from "
                    "buildHIRImpl dispatch"
                )
        elif state in ("refuse", "interpreter-only"):
            if name in supported:
                errors.append(
                    f"opcode {name}: {state} row admitted by isSupportedOpcode"
                )
            if name in dispatched:
                errors.append(
                    f"opcode {name}: {state} row present in buildHIRImpl dispatch"
                )
            if name not in reason_cases:
                errors.append(
                    f"opcode {name}: {state} row missing from "
                    "unsupportedOpcodeReason311"
                )
    return errors


def parse_targets(path: Path) -> dict[int, str]:
    """Slot -> name table from the vendored pristine opcode_targets.h."""
    names: dict[int, str] = {}
    idx = 0
    for m in re.finditer(r"&&(\w+)", path.read_text()):
        label = m.group(1)
        if label != "_unknown_opcode":
            if not label.startswith("TARGET_"):
                raise SystemExit(f"unexpected label in opcode_targets.h: {label}")
            names[idx] = label[len("TARGET_") :]
        idx += 1
    if idx != 256:
        raise SystemExit(f"opcode_targets.h has {idx} slots, expected 256")
    return names


def ground_truth(repo_root: Path):
    """Name/number/cache/deopt tables from the running 3.11 interpreter."""
    if sys.version_info[:2] != (3, 11):
        raise SystemExit(f"must run under CPython 3.11, got {sys.version}")
    import dis
    import opcode

    all_opname = getattr(dis, "_all_opname", opcode.opname)
    defined = {
        i: all_opname[i] for i in range(256) if not all_opname[i].startswith("<")
    }

    targets = parse_targets(repo_root / OPCODE_TARGETS)
    # DO_TRACING is the evaluator's tracing dispatch target: a real slot in
    # opcode_targets.h that is never emitted into co_code and is invisible
    # to the opcode module.  The support list carries it as a pseudo-slot.
    if targets.get(255) == "DO_TRACING":
        defined[255] = "DO_TRACING"
    mismatch = {
        i: (targets.get(i), defined.get(i))
        for i in range(256)
        if targets.get(i) != defined.get(i)
    }
    if mismatch:
        raise SystemExit(
            f"interpreter and vendored opcode tables disagree: {mismatch}"
        )

    caches = list(opcode._inline_cache_entries)
    deopt = {}  # variant name -> base name
    for base, variants in opcode._specializations.items():
        for v in variants:
            deopt[v] = base
    return defined, caches, deopt


def load_document(path: Path) -> dict:
    import tomllib

    with path.open("rb") as fp:
        return tomllib.load(fp)


def load_rows(path: Path) -> dict[str, dict]:
    doc = load_document(path)
    if "opcodes" not in doc:
        raise SystemExit(f"{path}: missing [opcodes] table")
    return doc["opcodes"]


def load_reason_registry(path: Path) -> dict[str, dict[str, str]]:
    doc = load_document(path)
    registry = doc.get("refusal_reasons", {})
    if not isinstance(registry, dict):
        return {}
    return {
        category: dict(entries) if isinstance(entries, dict) else entries
        for category, entries in registry.items()
    }


def validate_reason_registry(
    registry: dict[str, dict[str, str]], rows: dict[str, dict]
) -> list[str]:
    errors: list[str] = []
    categories = set(registry)
    for category in sorted(set(REASON_CATEGORIES) - categories):
        errors.append(f"refusal registry: missing category {category}")
    for category in sorted(categories - set(REASON_CATEGORIES)):
        errors.append(f"refusal registry: unknown category {category}")

    for category in REASON_CATEGORIES:
        reasons = registry.get(category)
        if not isinstance(reasons, dict):
            errors.append(f"refusal registry: {category} must be a table")
            continue
        if not reasons:
            errors.append(f"refusal registry: {category} must not be empty")
        for reason, description in sorted(reasons.items()):
            where = f"refusal registry {category}.{reason}"
            if category == "shape":
                if not reason.startswith("REFUSE_SHAPE_"):
                    errors.append(
                        f"{where}: shape reason must start with REFUSE_SHAPE_"
                    )
            elif not reason.startswith(("REFUSE_", "INTERP_ONLY_")) or (
                reason.startswith("REFUSE_SHAPE_")
            ):
                errors.append(
                    f"{where}: opcode reason must start with REFUSE_ or "
                    "INTERP_ONLY_, but not REFUSE_SHAPE_"
                )
            if not isinstance(description, str) or not description.strip():
                errors.append(f"{where}: description must be a non-empty string")

    opcode_reasons = registry.get("opcode", {})
    shape_reasons = registry.get("shape", {})
    if isinstance(opcode_reasons, dict) and isinstance(shape_reasons, dict):
        for reason in sorted(set(opcode_reasons) & set(shape_reasons)):
            errors.append(
                f"refusal registry: reason {reason} appears in opcode and shape"
            )

        used_opcode_reasons = {
            row.get("reason")
            for row in rows.values()
            if row.get("state") in ("refuse", "interpreter-only")
        }
        for reason in sorted(set(opcode_reasons) - used_opcode_reasons):
            errors.append(f"refusal registry: unused opcode reason {reason}")

    return errors


def validate(
    rows: dict[str, dict],
    truth,
    reason_registry: dict[str, dict[str, str]] | None = None,
) -> list[str]:
    defined, caches, deopt = truth
    num_of = {n: i for i, n in defined.items()}
    if reason_registry is None:
        reason_registry = load_reason_registry(REPO_ROOT / SUPPORT_LIST)
    errors = validate_reason_registry(reason_registry, rows)
    opcode_reasons = reason_registry.get("opcode", {})
    if not isinstance(opcode_reasons, dict):
        opcode_reasons = {}

    for name in sorted(set(defined.values()) - set(rows)):
        errors.append(f"missing row for defined opcode {name}")
    for name in sorted(set(rows) - set(defined.values())):
        errors.append(f"row for unknown opcode {name}")

    for name in sorted(set(rows) & set(defined.values())):
        row = rows[name]
        where = f"opcode {name}"
        state = row.get("state")
        if state not in STATES:
            errors.append(f"{where}: state {state!r} not in {STATES}")
            continue
        if row.get("num") != num_of[name]:
            errors.append(
                f"{where}: num {row.get('num')} != interpreter's {num_of[name]}"
            )
        base = deopt.get(name, name)
        expected_cache = caches[num_of.get(base, num_of[name])]
        if row.get("cache") != expected_cache:
            errors.append(
                f"{where}: cache {row.get('cache')} != interpreter's "
                f"{expected_cache}"
            )

        if state == "normalize":
            to = row.get("to")
            if to is None:
                errors.append(f"{where}: normalize row without 'to'")
            elif deopt.get(name) != to:
                errors.append(
                    f"{where}: normalizes to {to} but the interpreter's "
                    f"specialization family says {deopt.get(name)}"
                )
            elif rows.get(to, {}).get("state") != "translate":
                errors.append(
                    f"{where}: normalizes to {to}, whose state is "
                    f"{rows.get(to, {}).get('state')!r} instead of 'translate'"
                )
        elif "to" in row:
            errors.append(f"{where}: 'to' is only valid on normalize rows")

        if state in ("refuse", "interpreter-only"):
            reason = row.get("reason")
            if reason not in opcode_reasons:
                errors.append(
                    f"{where}: {state} row needs a registered reason from the "
                    f"opcode registry, got {reason!r}"
                )
        elif "reason" in row:
            errors.append(f"{where}: reason is only valid on refusal rows")

    return errors


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", type=Path, default=REPO_ROOT)
    args = ap.parse_args(argv)

    support_list = args.repo / SUPPORT_LIST
    rows = load_rows(support_list)
    registry = load_reason_registry(support_list)
    errors = validate(rows, ground_truth(args.repo), registry)
    errors.extend(
        validate_frontend(
            rows,
            (args.repo / HIR_BUILDER).read_text(),
            (args.repo / BYTECODE_DECODER).read_text(),
        )
    )
    if errors:
        for err in errors:
            print(f"bytecode-support: {err}", file=sys.stderr)
        print(f"bytecode-support: {len(errors)} error(s)", file=sys.stderr)
        return 1
    counts: dict[str, int] = {}
    for row in rows.values():
        counts[row["state"]] = counts.get(row["state"], 0) + 1
    summary = ", ".join(f"{k}={v}" for k, v in sorted(counts.items()))
    print(f"bytecode-support: {len(rows)} opcodes consistent ({summary})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
