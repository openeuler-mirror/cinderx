"""A2 exact CPython test_slots-shaped JIT publication footprint probe."""

from __future__ import annotations

import argparse
from collections import Counter
import gc
import json
from pathlib import Path


class G:
    def __eq__(self, other):
        return False


def _raw_census(_cinderx) -> str:
    objects = gc.get_objects()
    histogram = Counter(
        f"{type(obj).__module__}.{type(obj).__qualname__}" for obj in objects
    )
    count = len(objects)
    del objects
    stats = _cinderx._get_trigger_stats()
    return json.dumps(
        {
            "gc_objects": count,
            "object_types": dict(sorted(histogram.items())),
            "resident_code_buffers": int(stats["resident_code_buffers"]),
            "compiled_function_creations": int(stats["compiled_function_creations"]),
            "machine_code_entries": int(stats["machine_code_entries"]),
        },
        sort_keys=True,
    )


def _type_delta(left: dict, right: dict) -> dict[str, int]:
    keys = set(left) | set(right)
    return {
        key: int(right.get(key, 0)) - int(left.get(key, 0))
        for key in sorted(keys)
        if int(right.get(key, 0)) != int(left.get(key, 0))
    }


def run() -> dict:
    import _cinderx
    import cinderjit
    import cinderx
    from cinderx.jit import jit_suppress

    cinderx.init()
    _cinderx.install_frame_evaluator()
    jit_suppress(_raw_census)
    jit_suppress(_type_delta)
    for _ in range(5):
        _raw_census(_cinderx)
    raw_samples: dict[str, str] = {}
    g = G()

    gc.collect()
    raw_samples["before_first_call"] = _raw_census(_cinderx)

    assert (g == g) is False
    raw_samples["after_first_publication"] = _raw_census(_cinderx)
    if not cinderjit.is_jit_compiled(G.__eq__):
        raise AssertionError("first G.__eq__ call did not publish at threshold=1")

    for call in range(2, 1001):
        assert (g == g) is False
        if call in (10, 100, 1000):
            raw_samples[f"after_{call}"] = _raw_census(_cinderx)

    gc.collect()
    raw_samples["after_gc_1"] = _raw_census(_cinderx)
    gc.collect()
    raw_samples["after_gc_2"] = _raw_census(_cinderx)

    samples = {name: json.loads(raw) for name, raw in raw_samples.items()}
    baseline = samples["before_first_call"]
    first = samples["after_first_publication"]
    after_10 = samples["after_10"]
    after_100 = samples["after_100"]
    after_1000 = samples["after_1000"]
    after_gc_1 = samples["after_gc_1"]
    after_gc_2 = samples["after_gc_2"]

    steady_counts_equal = (
        after_10["gc_objects"] == after_100["gc_objects"] == after_1000["gc_objects"]
    )
    steady_types_equal = (
        after_10["object_types"]
        == after_100["object_types"]
        == after_1000["object_types"]
    )
    steady_buffers_equal = (
        after_10["resident_code_buffers"]
        == after_100["resident_code_buffers"]
        == after_1000["resident_code_buffers"]
    )
    steady_compiles_equal = (
        after_10["compiled_function_creations"]
        == after_100["compiled_function_creations"]
        == after_1000["compiled_function_creations"]
    )
    gc_stable = (
        after_gc_1["gc_objects"] == after_gc_2["gc_objects"]
        and after_gc_1["object_types"] == after_gc_2["object_types"]
        and after_gc_1["resident_code_buffers"] == after_gc_2["resident_code_buffers"]
    )
    strict_plateau = (
        steady_counts_equal
        and steady_types_equal
        and steady_buffers_equal
        and steady_compiles_equal
        and gc_stable
    )
    first_type_delta = _type_delta(baseline["object_types"], first["object_types"])
    steady_type_delta = _type_delta(
        after_10["object_types"], after_1000["object_types"]
    )
    return {
        "result": "PASS" if strict_plateau else "FAIL",
        "classification": (
            "APPROVED_STRESS_MODE_DEVIATION" if strict_plateau else "PRODUCT_BUG"
        ),
        "shape": "test.test_descr.ClassPropertiesAndMethods.test_slots:G.__eq__",
        "samples": samples,
        "delta": {
            "first_publication_gc_objects": first["gc_objects"]
            - baseline["gc_objects"],
            "first_publication_object_types": first_type_delta,
            "steady_10_to_1000_gc_objects": after_1000["gc_objects"]
            - after_10["gc_objects"],
            "steady_10_to_1000_object_types": steady_type_delta,
            "steady_resident_code_buffers": after_1000["resident_code_buffers"]
            - after_10["resident_code_buffers"],
            "steady_compiled_function_creations": after_1000[
                "compiled_function_creations"
            ]
            - after_10["compiled_function_creations"],
        },
        "strict_checks": {
            "10_100_1000_gc_counts_equal": steady_counts_equal,
            "10_100_1000_type_histograms_equal": steady_types_equal,
            "10_100_1000_resident_buffers_equal": steady_buffers_equal,
            "10_100_1000_compile_counts_equal": steady_compiles_equal,
            "two_post_gc_samples_equal": gc_stable,
        },
        "strict_plateau": strict_plateau,
        "compatibility_baseline_changed": False,
        "explanation": (
            "one-time JIT publication footprint; not repeated lookup leak"
            if strict_plateau
            else "steady-state footprint continued to change"
        ),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    report = run()
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(
        json.dumps(
            {
                "result": report["result"],
                "classification": report["classification"],
                "strict_plateau": report["strict_plateau"],
            },
            sort_keys=True,
        )
    )
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
