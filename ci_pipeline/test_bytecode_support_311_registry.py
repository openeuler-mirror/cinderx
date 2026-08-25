"""Mutation tests for the CPython 3.11 refusal-reason registry."""

from ci_pipeline import check_bytecode_support_311 as checker


SUPPORT_LIST = checker.REPO_ROOT / checker.SUPPORT_LIST


def mutate_registry(registry):
    return {
        category: dict(reasons)
        for category, reasons in registry.items()
    }


def test_committed_registry_is_consistent():
    rows = checker.load_rows(SUPPORT_LIST)
    registry = checker.load_reason_registry(SUPPORT_LIST)

    assert checker.validate_reason_registry(registry, rows) == []


def test_shape_registry_covers_mr3_eligibility_reasons():
    registry = checker.load_reason_registry(SUPPORT_LIST)

    assert set(registry["shape"]) >= {
        "REFUSE_SHAPE_GENERATOR_RUNTIME_UNAUDITED",
        "REFUSE_SHAPE_ASYNC_CODE",
        "REFUSE_SHAPE_NON_FUNCTION_SCOPE",
        "REFUSE_SHAPE_NAMESPACE_UNSUPPORTED",
        "REFUSE_SHAPE_INT_ACCUMULATOR_POLICY",
        "REFUSE_SHAPE_CODEGEN_SPAN",
        "REFUSE_SHAPE_STATIC_RUNTIME_CACHE",
        "REFUSE_SHAPE_INVALID_UTF8_NAME",
    }


def test_missing_shape_registry_is_detected():
    rows = checker.load_rows(SUPPORT_LIST)
    registry = mutate_registry(checker.load_reason_registry(SUPPORT_LIST))
    del registry["shape"]

    errors = checker.validate_reason_registry(registry, rows)

    assert any("missing category shape" in error for error in errors)


def test_shape_reason_prefix_is_checked():
    rows = checker.load_rows(SUPPORT_LIST)
    registry = mutate_registry(checker.load_reason_registry(SUPPORT_LIST))
    registry["shape"]["REFUSE_NOT_A_SHAPE_REASON"] = "bad mutation"

    errors = checker.validate_reason_registry(registry, rows)

    assert any(
        "REFUSE_NOT_A_SHAPE_REASON" in error and "REFUSE_SHAPE_" in error
        for error in errors
    )


def test_reason_description_is_required():
    rows = checker.load_rows(SUPPORT_LIST)
    registry = mutate_registry(checker.load_reason_registry(SUPPORT_LIST))
    registry["shape"]["REFUSE_SHAPE_ASYNC_CODE"] = ""

    errors = checker.validate_reason_registry(registry, rows)

    assert any(
        "REFUSE_SHAPE_ASYNC_CODE" in error and "non-empty string" in error
        for error in errors
    )


def test_unused_opcode_reason_is_detected():
    rows = checker.load_rows(SUPPORT_LIST)
    registry = mutate_registry(checker.load_reason_registry(SUPPORT_LIST))
    registry["opcode"]["REFUSE_TEST_MUTATION"] = "unused test reason"

    errors = checker.validate_reason_registry(registry, rows)

    assert any(
        "unused opcode reason REFUSE_TEST_MUTATION" in error
        for error in errors
    )
