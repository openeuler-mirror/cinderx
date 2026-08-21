"""Unit tests for the lifecycle census criterion.

The census itself needs an interpreter with the frame evaluator installed;
what is tested here is the judge it uses, because a criterion that never
reports anything and a workload that never leaks look identical from the
outside.
"""

import importlib.util
import pathlib

import pytest

_PATH = pathlib.Path(__file__).parent / "jit311" / "lifecycle_census.py"
_SPEC = importlib.util.spec_from_file_location("jit311_lifecycle_census", _PATH)
census = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(census)

growing = census.growing_every_round
residency = census.native_residency_drift


def test_a_key_that_grows_every_round_is_reported():
    samples = [{"Widget": 1}, {"Widget": 2}, {"Widget": 3}, {"Widget": 4}]
    assert growing(samples) == {"Widget": [1, 1, 1]}


def test_a_cache_that_fills_once_is_not_a_leak():
    # The shape of a lazily built table: one jump, then flat.  Reporting
    # this is what makes a leak hunt useless.
    samples = [{"Widget": 1}, {"Widget": 9}, {"Widget": 9}, {"Widget": 9}]
    assert growing(samples) == {}


def test_a_key_that_pauses_for_one_round_is_not_a_leak():
    samples = [{"Widget": 1}, {"Widget": 2}, {"Widget": 2}, {"Widget": 3}]
    assert growing(samples) == {}


def test_a_key_absent_from_early_samples_still_counts():
    # An object type that only appears once the workload has run has to be
    # comparable against the rounds before it existed.
    samples = [{}, {"Widget": 1}, {"Widget": 2}, {"Widget": 3}]
    assert growing(samples) == {"Widget": [1, 1, 1]}


def test_a_shrinking_key_is_not_a_leak():
    samples = [{"Widget": 9}, {"Widget": 6}, {"Widget": 3}, {"Widget": 1}]
    assert growing(samples) == {}


@pytest.mark.parametrize("samples", [[], [{"Widget": 1}]])
def test_too_few_samples_report_nothing(samples):
    # One census has no deltas at all; reporting a leak from it would be
    # inventing evidence.
    assert growing(samples) == {}

def test_a_native_gauge_that_climbs_every_round_is_reported():
    # The census the object walk cannot do: a code buffer or a code-extra
    # block leaked once per round is invisible to gc.get_objects(), so the
    # runtime gauges are censused under their own key space -- and judged
    # by their own rule, since they are required back at baseline rather
    # than merely required not to grow.
    samples = [
        {"<all gc objects>": 100, "<native> resident_code_buffers": n}
        for n in (1, 2, 3, 4)
    ]
    assert residency(samples) == {
        "<native> resident_code_buffers": {"baseline": 1, "rounds": [2, 3, 4]}
    }


def test_a_flat_native_gauge_is_not_a_leak():
    samples = [
        {"<native> resident_code_buffers": 3, "<all gc objects>": 100 + n}
        for n in (0, 1, 0, 1)
    ]
    assert residency(samples) == {}
    assert growing(samples) == {}

# -- the residency judge -------------------------------------------------


def test_a_gauge_stranded_once_and_never_again_is_a_leak():
    # The commonest shape a real leak has, and the one "grew in every
    # round" waves through: one object stranded on the first measured
    # round, permanent thereafter.
    samples = [{"<native> resident_code_buffers": n} for n in (0, 1, 1, 1)]
    assert growing(samples) == {}
    assert residency(samples) == {
        "<native> resident_code_buffers": {"baseline": 0, "rounds": [1, 1, 1]}
    }


def test_a_gauge_stranded_every_other_round_is_a_leak():
    samples = [{"<native> resident_code_buffers": n} for n in (0, 1, 1, 2, 2)]
    assert growing(samples) == {}
    assert residency(samples)


def test_a_gauge_back_at_baseline_every_round_is_clean():
    samples = [{"<native> resident_code_buffers": 3} for _ in range(5)]
    assert residency(samples) == {}


def test_a_gauge_that_dips_below_baseline_is_also_drift():
    # Under-counting is a broken ledger, not a clean run: the rehash and
    # the census both read these as truth.
    samples = [{"<native> watched_codes": n} for n in (10, 10, 6, 10)]
    assert residency(samples) == {
        "<native> watched_codes": {"baseline": 10, "rounds": [10, 6, 10]}
    }


def test_the_object_census_judge_ignores_native_keys():
    # Otherwise a gauge would be judged twice, by two different rules.
    samples = [
        {"<native> resident_code_buffers": n, "Widget": n} for n in (1, 2, 3)
    ]
    assert growing(samples) == {"Widget": [1, 1]}


def test_the_residency_judge_ignores_object_census_keys():
    samples = [{"Widget": n} for n in (1, 9, 9)]
    assert residency(samples) == {}


def test_too_few_samples_report_no_drift():
    assert residency([]) == {}
    assert residency([{"<native> resident_code_buffers": 1}]) == {}
