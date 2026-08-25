# Copyright (c) Meta Platforms, Inc. and affiliates.
"""CPython 3.11 JIT test corpora, migrated from the reference campaign.

Provenance: the corpus_* modules are verbatim copies from the dryrun
repository (branch dryrun/m10-compat, commit 178ffcbc8) --
ci_pipeline/diffgate/corpus/ for the call/operator/control-flow/hot-loop/
unbound/frame/deopt-resume/IC-mutation corpora and docs/dryrun/ for the
generator corpus.  Keeping them verbatim keeps future diffing against the
campaign trivial; adaptation happens here in the package, not in the files.

Convention: each module defines case_* callables; a case may carry a
.helpers list of functions that a compiling harness should compile alongside
it.  diffgate_rt.checkpoint() is the campaign's mode hook -- a no-op unless
a harness installs one (the shadow/execute harnesses of later MRs uncompile
or checkpoint at these points).
"""

from __future__ import annotations

import importlib
import sys
import types
from collections.abc import Iterator

CORPUS_MODULES = (
    "corpus_calls",
    "corpus_controlflow",
    "corpus_deopt_resume",
    "corpus_frames",
    "corpus_generators",
    "corpus_hotloops",
    "corpus_ic_mutation",
    "corpus_operators",
    "corpus_unbound",
    "corpus_execute_min",
)


def _install_diffgate_rt_shim() -> None:
    """The corpus files import the campaign's `diffgate_rt` hook module by
    its historical name; provide it (same trick the campaign's refcount
    matrix uses) so the files can stay verbatim."""
    if "diffgate_rt" in sys.modules:
        return
    shim = types.ModuleType("diffgate_rt")
    shim._hook = None

    def checkpoint() -> None:
        hook = shim._hook
        if hook is not None:
            hook()

    def set_checkpoint_hook(hook) -> None:
        shim._hook = hook

    shim.checkpoint = checkpoint
    shim.set_checkpoint_hook = set_checkpoint_hook
    sys.modules["diffgate_rt"] = shim


def load_module(name: str):
    _install_diffgate_rt_shim()
    return importlib.import_module(f"{__name__}.{name}")


def iter_cases(modules: tuple[str, ...] = CORPUS_MODULES) -> Iterator[tuple]:
    """Yield (module_name, case_name, case_callable) over the corpora."""
    for module_name in modules:
        module = load_module(module_name)
        for attr in sorted(vars(module)):
            if attr.startswith("case_"):
                yield module_name, attr, getattr(module, attr)
