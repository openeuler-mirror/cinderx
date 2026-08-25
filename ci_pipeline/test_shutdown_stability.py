import pytest

from ci_pipeline.jit311.shutdown_stability import (
    DEFAULT_MALLOC_PERTURB,
    ENV_ENTROPY_SPAN,
    STATES,
    entropy_pad,
    parse_quota,
)


def test_quota_parses_the_final_acceptance_shape():
    quota = parse_quota(
        "installed=200,parked=200,function-death=200,code-death=200,"
        "failure-unwind=200,multithread-completed=2000"
    )
    assert quota["multithread-completed"] == 2000
    assert sum(quota.values()) == 3000
    assert list(quota) == list(STATES)


@pytest.mark.parametrize(
    "text",
    [
        "",
        "installed=0",
        "installed=-5",
        "installed=abc",
        "unknown-state=5",
        "installed=1,installed=2",
    ],
)
def test_quota_rejects_silent_misconfiguration(text):
    with pytest.raises(ValueError):
        parse_quota(text)


def test_layout_entropy_is_deterministic_and_actually_varies():
    pads = [entropy_pad(index, ENV_ENTROPY_SPAN) for index in range(1, 200)]
    assert pads == [entropy_pad(index, ENV_ENTROPY_SPAN) for index in range(1, 200)]
    # One fixed environment block samples exactly one layout; the pad must
    # keep consecutive children from sharing theirs.
    assert len(set(pads)) > 100
    assert all(0 <= pad < ENV_ENTROPY_SPAN for pad in pads)
    assert entropy_pad(7, 0) == 0


def test_freed_memory_poisoning_defaults_on():
    # The B9 teardown use-after-free exits 0 in the gate's fixed layout;
    # MALLOC_PERTURB_ is what makes it fault in every layout.  The default
    # must stay armed so a future edit cannot quietly restore the blind gate.
    assert DEFAULT_MALLOC_PERTURB > 0
