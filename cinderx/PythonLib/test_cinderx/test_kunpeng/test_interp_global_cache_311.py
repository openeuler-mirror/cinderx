# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Global-name cache hits and invalidation with the ordinary evaluator."""

from ._interp311_test_support import Interpreter311Case


class GlobalNameCache311Tests(Interpreter311Case):
    probes = ("key",)

    def test_global_cache(self):
        self.run_case("case_cache.py", mode=None)
