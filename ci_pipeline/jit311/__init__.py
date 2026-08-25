# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Problem-front-loading test infrastructure for the CPython 3.11 JIT.

This package carries the trigger-proof report (report.py) and the test
drivers defined by the development plan (docs/cp311-jit-dev-plan.md, MR-01):
tests must be able to prove that the paths they claim to cover actually
fired, and must turn red when an expected trigger did not happen.
"""
