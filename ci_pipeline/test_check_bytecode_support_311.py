"""Self-test for the CPython 3.11 bytecode support list checker.

Runs the checker against the committed list (which must be consistent) and
then against targeted mutations of it, each of which must be rejected.  A
checker that cannot see these faults would wave a stale or corrupted list
through the gate, so this file runs before the checker in CI.
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_bytecode_support_311 as checker


@pytest.fixture(scope="module")
def truth():
    if sys.version_info[:2] != (3, 11):
        pytest.skip("interpreter table validation requires CPython 3.11")
    return checker.ground_truth(checker.REPO_ROOT)


@pytest.fixture(scope="module")
def committed_rows():
    return checker.load_rows(checker.REPO_ROOT / checker.SUPPORT_LIST)


@pytest.fixture(scope="module")
def committed_registry():
    return checker.load_reason_registry(
        checker.REPO_ROOT / checker.SUPPORT_LIST
    )


@pytest.fixture(scope="module")
def builder_source():
    return (checker.REPO_ROOT / checker.HIR_BUILDER).read_text()


@pytest.fixture(scope="module")
def decoder_source():
    return (checker.REPO_ROOT / checker.BYTECODE_DECODER).read_text()


def mutate(rows):
    return {name: dict(row) for name, row in rows.items()}


def mutate_function(source, signature, old, new):
    search_start = source.index(signature)
    body = checker._function_body(source, signature)
    body_start = source.index(body, search_start)
    assert body.count(old) == 1
    mutated_body = body.replace(old, new, 1)
    return source[:body_start] + mutated_body + source[body_start + len(body) :]


def test_committed_list_is_consistent(committed_rows, committed_registry, truth):
    assert checker.validate(committed_rows, truth, committed_registry) == []


def test_committed_frontend_matches_list(
    committed_rows, builder_source, decoder_source
):
    assert checker.validate_frontend(
        committed_rows, builder_source, decoder_source
    ) == []


def test_cpp_preprocessor_uses_311_non_lazy_arms():
    source = """\
#if PY_VERSION_HEX < 0x030C0000
case PY311:
#else
case NEWER:
#endif
#if PY_VERSION_HEX >= 0x030E0000 || ENABLE_LAZY_IMPORTS
case IMPORT_FROM:
#endif
"""
    selected = checker._preprocess_cpp_311(source)
    assert "case PY311:" in selected
    assert "case NEWER:" not in selected
    assert "case IMPORT_FROM:" not in selected


def test_missing_supported_case_is_detected(
    committed_rows, builder_source, decoder_source
):
    source = mutate_function(
        builder_source,
        "bool isSupportedOpcode(int opcode)",
        "    case LOAD_FAST:\n",
        "",
    )
    errors = checker.validate_frontend(committed_rows, source, decoder_source)
    assert any(
        "LOAD_FAST" in error and "isSupportedOpcode" in error
        for error in errors
    )


def test_missing_translation_case_is_detected(
    committed_rows, builder_source, decoder_source
):
    source = mutate_function(
        builder_source,
        "void HIRBuilder::translate(",
        "        case LOAD_FAST:\n",
        "",
    )
    errors = checker.validate_frontend(committed_rows, source, decoder_source)
    assert any(
        "LOAD_FAST" in error and "buildHIRImpl dispatch" in error
        for error in errors
    )


def test_refusal_cannot_be_admitted_by_supported_predicate(
    committed_rows, builder_source, decoder_source
):
    source = mutate_function(
        builder_source,
        "bool isSupportedOpcode(int opcode)",
        "    case LOAD_FAST:\n",
        "    case MATCH_CLASS:\n    case LOAD_FAST:\n",
    )
    errors = checker.validate_frontend(committed_rows, source, decoder_source)
    assert any(
        "MATCH_CLASS" in error and "admitted by isSupportedOpcode" in error
        for error in errors
    )


def test_refusal_cannot_enter_translation_dispatch(
    committed_rows, builder_source, decoder_source
):
    source = mutate_function(
        builder_source,
        "void HIRBuilder::translate(",
        "        case LOAD_FAST:\n",
        "        case MATCH_CLASS:\n        case LOAD_FAST:\n",
    )
    errors = checker.validate_frontend(committed_rows, source, decoder_source)
    assert any(
        "MATCH_CLASS" in error and "present in buildHIRImpl dispatch" in error
        for error in errors
    )


def test_missing_reason_case_is_detected(
    committed_rows, builder_source, decoder_source
):
    source = mutate_function(
        builder_source,
        "const char* unsupportedOpcodeReason311(BorrowedRef<PyCodeObject> code)",
        "      case GET_AITER:\n",
        "",
    )
    errors = checker.validate_frontend(committed_rows, source, decoder_source)
    assert any(
        "GET_AITER" in error and "unsupportedOpcodeReason311" in error
        for error in errors
    )


def test_normalize_row_cannot_bypass_decoder(
    committed_rows, builder_source, decoder_source
):
    source = mutate_function(
        builder_source,
        "bool isSupportedOpcode(int opcode)",
        "    case LOAD_FAST:\n",
        "    case BINARY_OP_ADD_INT:\n    case LOAD_FAST:\n",
    )
    errors = checker.validate_frontend(committed_rows, source, decoder_source)
    assert any(
        "BINARY_OP_ADD_INT" in error
        and "admitted directly by isSupportedOpcode" in error
        for error in errors
    )


def test_normalization_must_be_consumed_by_decoder(
    committed_rows, builder_source, decoder_source
):
    source = mutate_function(
        decoder_source,
        "_Py_CODEUNIT BytecodeInstruction::word() const",
        "unspecialize(uninstrumentedOpcode())",
        "uninstrumentedOpcode()",
    )
    errors = checker.validate_frontend(committed_rows, builder_source, source)
    assert "bytecode decoder: word() does not normalize specialized opcodes" in errors


def test_extended_arg_dispatch_exception_is_verified(
    committed_rows, builder_source, decoder_source
):
    source = mutate_function(
        decoder_source,
        "void BytecodeInstruction::calcOpcodeOffsetAndOparg() const",
        "== EXTENDED_ARG",
        "== EXTENDED_ARG_REMOVED",
    )
    errors = checker.validate_frontend(committed_rows, builder_source, source)
    assert "bytecode decoder: EXTENDED_ARG is not consumed" in errors


def test_missing_row_is_detected(committed_rows, truth):
    rows = mutate(committed_rows)
    del rows["LOAD_FAST"]
    errors = checker.validate(rows, truth)
    assert any("missing row" in e and "LOAD_FAST" in e for e in errors)


def test_unknown_opcode_row_is_detected(committed_rows, truth):
    rows = mutate(committed_rows)
    rows["NOT_A_REAL_OPCODE"] = {"num": 1, "state": "translate", "cache": 0}
    errors = checker.validate(rows, truth)
    assert any("unknown opcode NOT_A_REAL_OPCODE" in e for e in errors)


def test_wrong_cache_width_is_detected(committed_rows, truth):
    rows = mutate(committed_rows)
    rows["LOAD_ATTR"]["cache"] += 1
    errors = checker.validate(rows, truth)
    assert any("LOAD_ATTR" in e and "cache" in e for e in errors)


def test_wrong_number_is_detected(committed_rows, truth):
    rows = mutate(committed_rows)
    rows["LOAD_FAST"]["num"] += 1
    errors = checker.validate(rows, truth)
    assert any("LOAD_FAST" in e and "num" in e for e in errors)


def test_unregistered_state_is_detected(committed_rows, truth):
    rows = mutate(committed_rows)
    rows["LOAD_FAST"]["state"] = "maybe"
    errors = checker.validate(rows, truth)
    assert any("LOAD_FAST" in e and "'maybe'" in e for e in errors)


def test_normalization_must_follow_the_interpreter(committed_rows, truth):
    rows = mutate(committed_rows)
    rows["LOAD_ATTR_INSTANCE_VALUE"]["to"] = "LOAD_GLOBAL"
    errors = checker.validate(rows, truth)
    assert any(
        "LOAD_ATTR_INSTANCE_VALUE" in e and "specialization family" in e
        for e in errors
    )


def test_normalization_target_must_be_translated(committed_rows, truth):
    rows = mutate(committed_rows)
    rows["LOAD_ATTR"]["state"] = "refuse"
    rows["LOAD_ATTR"]["reason"] = "REFUSE_UNPORTED"
    errors = checker.validate(rows, truth)
    assert any("instead of 'translate'" in e for e in errors)


def test_refusal_requires_registered_reason(committed_rows, truth):
    rows = mutate(committed_rows)
    del rows["MATCH_CLASS"]["reason"]
    errors = checker.validate(rows, truth)
    assert any("MATCH_CLASS" in e and "registered reason" in e for e in errors)


def test_shape_reason_is_not_valid_on_an_opcode_row(
    committed_rows, committed_registry, truth
):
    rows = mutate(committed_rows)
    rows["MATCH_CLASS"]["reason"] = "REFUSE_SHAPE_ASYNC_CODE"
    errors = checker.validate(rows, truth, committed_registry)
    assert any(
        "MATCH_CLASS" in e and "opcode registry" in e
        for e in errors
    )


def test_reason_is_refusal_only(committed_rows, truth):
    rows = mutate(committed_rows)
    rows["LOAD_FAST"]["reason"] = "REFUSE_UNPORTED"
    errors = checker.validate(rows, truth)
    assert any("LOAD_FAST" in e and "refusal rows" in e for e in errors)
