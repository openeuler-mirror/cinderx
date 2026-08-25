#!/usr/bin/env python3
"""Call-matrix refcount assertion (M5 exit condition).

For every corpus case, measure the refcount drift of module-level objects
(callables, operand instances, containers) across N repeated invocations,
in interp mode and jit mode. The assertion is drift equality between the
two modes: cases that legitimately mutate state drift identically in both,
while a call-path refcount defect shows as a jit-only drift.

Usage:
    refcount_matrix.py <corpus_dir> <module> <mode:interp|jit> <out.json>
    refcount_matrix.py diff <interp.json> <jit.json>

Run once per mode, then compare with the built-in diff mode (exit 1 on any
inequality).  Three things are compared, and all three must hold:

  * refcount drift, per case, between the two modes;
  * the semantic outcome of each case -- the returned value, or the raised
    exception's type and message -- so a lowering that is refcount-neutral
    but returns the wrong answer still fails;
  * in jit mode, that each case actually entered machine code.

In jit mode every case (and its helpers) must actually compile AND run
compiled: an unavailable cinderjit, a refused force_compile(), a missing
frame evaluator or a zero machine-code entry delta is a loud error, never
a silent fall back to interpreted execution -- interpreted runs may not
impersonate JIT evidence.
Pseudo-immortal singletons (True/False/None/...) are excluded: their
refcounts drift by design on 3.11 (see M4-log).
"""

import gc
import importlib
import json
import os
import sys
import types

N = 200

_rt = types.ModuleType("diffgate_rt")
_rt.checkpoint = lambda: None
sys.modules["diffgate_rt"] = _rt


def diff_main() -> int:
    interp_path, jit_path = sys.argv[2:4]
    # The scheduled arm's floor: how many cases the JIT must still be
    # choosing on its own.  Pinned by the caller, because "the scheduler
    # took nothing" and "the scheduler took everything it should" produce
    # the same drift table.
    min_compiled = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    interp = json.load(open(interp_path))
    jit_doc = json.load(open(jit_path))
    mismatched = {}
    cases = set(interp["drift"]) | set(jit_doc["drift"])
    for case in sorted(cases):
        a = interp["drift"].get(case)
        b = jit_doc["drift"].get(case)
        if a != b:
            mismatched[case] = {"drift": {"interp": a, "jit": b}}
        # Refcount neutrality says nothing about answers: compare the
        # outcome each mode produced for the same case.
        oa = interp.get("outcome", {}).get(case)
        ob = jit_doc.get("outcome", {}).get(case)
        if oa != ob:
            mismatched.setdefault(case, {})["outcome"] = {
                "interp": oa,
                "jit": ob,
            }
    if mismatched:
        print(json.dumps(mismatched, indent=1, sort_keys=True))
        print(f"refcount-matrix: {len(mismatched)} case(s) differ")
        return 1
    # An outcome table that never made it into a report would silently
    # reduce this to the drift-only check it used to be.
    for doc, label in ((interp, "interp"), (jit_doc, "jit")):
        if set(doc.get("outcome", {})) != set(doc["drift"]):
            print(
                f"refcount-matrix: {label} report has no outcome for every "
                f"case; the semantic comparison would be vacuous"
            )
            return 1
    # Entry evidence is only meaningful for a run that was allowed to
    # execute; a shadow-mode jit arm records no entries by design.
    if jit_doc.get("mode") in ("jit", "auto") and jit_doc.get("executing"):
        chosen = jit_doc.get("scheduler_compiled")
        silent = [
            case
            for case, entries in jit_doc.get("machine_code_entries", {}).items()
            # In the scheduled arm the JIT decides what it takes; a case it
            # declined is interpreted on purpose and owes no entries.
            if not entries and (chosen is None or chosen.get(case))
        ]
        if silent or not jit_doc.get("machine_code_entries"):
            print(
                "refcount-matrix: jit report shows cases that never entered "
                f"machine code: {silent or '(no entry table at all)'}"
            )
            return 1
        if chosen is not None:
            # The scheduled arm proves nothing if the scheduler stopped
            # taking anything.  The floor is pinned by the caller so an
            # eroding surface is a red leg rather than a quiet one.
            taken = sorted(case for case, fns in chosen.items() if fns)
            if len(taken) < min_compiled:
                print(
                    f"refcount-matrix: the scheduler compiled {len(taken)} "
                    f"case(s), below the pinned floor of {min_compiled}: "
                    f"{taken}"
                )
                return 1
            print(
                f"refcount-matrix: scheduler took {len(taken)}/"
                f"{len(chosen)} case(s)"
            )
    print(
        f"refcount-matrix: {len(cases)} case(s), drift and outcome equal "
        f"across modes"
    )
    return 0


def stable_repr(value):
    # Deterministic across processes: primitives and sequences thereof.
    # Anything that can embed an address or a hash-randomized order pins as
    # its type name only.
    if isinstance(value, (str, bytes, int, float, bool, type(None))):
        return repr(value)
    if isinstance(value, (tuple, list)):
        inner = ",".join(stable_repr(item) for item in value)
        return f"{type(value).__name__}[{inner}]"
    return f"opaque:{type(value).__qualname__}"


def stable_digest(text: str) -> str:
    import hashlib
    import re

    normalized = re.sub(r"0x[0-9a-fA-F]+", "0xADDR", text)
    normalized = re.sub(
        r"(?:/[^/\s\"\']+)+/([^/\s\"\']+)", r"<path>/\1", normalized
    )
    return hashlib.sha256(normalized.encode()).hexdigest()[:8]


def outcome_of(fn) -> str:
    try:
        return f"ok:{stable_digest(stable_repr(fn()))}"
    except BaseException as exc:
        return f"raise:{type(exc).__qualname__}:{stable_digest(str(exc))}"


# Calls made before the scheduled arm expects machine code.  The threshold
# the leg runs at is well below this; the margin covers a case whose
# helpers are only reached on some of its paths.
SCHEDULER_WARMUP = 200


def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == "diff":
        return diff_main()
    corpus_dir, modname, mode, out_path = sys.argv[1:5]
    sys.path.insert(0, corpus_dir)
    sys.path.insert(0, ".")

    jit = None
    # Only the executing mode can move the machine-code entry counter;
    # under shadow the artifact is compiled and deliberately discarded, so
    # requiring entries there would demand something the mode forbids.
    # `canary` is execute's test-only alias, so both spellings name the
    # same tier; reading only one of them silently downgraded the execute
    # arm to "entries are optional".
    executing = os.environ.get("CINDERX_JIT_MODE") in ("execute", "canary")
    # Two ways to reach machine code.  `jit` asks for it directly, which
    # keeps the arm deterministic; `auto` lets the Auto-JIT scheduler
    # decide, which is the path production takes and the only one that
    # exercises dispatch, outer anchoring and fresh attachment under the
    # reference-count contract.
    scheduled = mode == "auto"
    if mode in ("jit", "auto"):
        # 守卫自适应去特化会在案例中途卸载重编（共享 helper 跑热后被
        # force_compile 会烘焙特化形，守卫风暴触发 despec）：产物切换
        # 使烘焙引用集合变化，双快照协议无法与真实漂移区分（实测恰为
        # 每案例 −1 且 despec 关闭即 0、不随迭代累积——记账工件而非
        # 泄漏）。本判据只验证编译产物的逐迭代引用中性，despec 迁移
        # 的引用平衡由其触发路径的 Ref 持有审计保证。
        os.environ.setdefault("CINDERX_ADAPTIVE_DESPEC", "0")
        # The executing tier runs under the debug allocator from MR-04 on;
        # assert it rather than assume the caller set it.
        if executing:
            # Ask the runtime which allocator is in force rather than which
            # one the environment asked for: the variable says what was
            # requested, the interpreter says what happened.
            try:
                import _testcapi

                in_force = _testcapi.pymem_getallocatorsname()
            except (ImportError, AttributeError):
                in_force = None
            if in_force is not None:
                if "debug" not in in_force:
                    print(
                        f"refcount-matrix: the executing mode requires the debug "
                        f"allocator; the interpreter reports {in_force!r}",
                        file=sys.stderr,
                    )
                    return 2
            elif os.environ.get("PYTHONMALLOC") != "debug":
                print(
                    "refcount-matrix: the executing mode requires PYTHONMALLOC=debug "
                    "(this build exposes no way to confirm it physically)",
                    file=sys.stderr,
                )
                return 2
        import _cinderx
        import cinderx

        cinderx.init()
        # Without the frame evaluator the interpreter's specialized CALL
        # builds and runs the Python frame inline, bypassing the compiled
        # entry point that force_compile() installed.  The arm would then
        # measure interpreted execution while reporting is_jit_compiled().
        _cinderx.install_frame_evaluator()
        if not _cinderx.is_frame_evaluator_installed():
            print(
                "refcount-matrix: frame evaluator did not install; jit-mode "
                "evidence requires it, interpreted runs may not impersonate "
                "compiled execution",
                file=sys.stderr,
            )
            return 2
        try:
            import cinderjit as jit
        except ImportError:
            print(
                "refcount-matrix: jit mode requested but cinderjit is not "
                "available (capability-gated build?); refusing to fall back "
                "to interpreted execution",
                file=sys.stderr,
            )
            return 2

    mod = importlib.import_module(f"corpus.{modname}")

    singletons = {id(True), id(False), id(None), id(NotImplemented), id(...)}
    func_globals = set()
    for obj in vars(mod).values():
        if isinstance(obj, types.FunctionType):
            func_globals.add(id(obj.__globals__))
        elif isinstance(obj, dict):
            for inner in obj.values():
                if isinstance(inner, types.FunctionType):
                    func_globals.add(id(inner.__globals__))
    targets = {}
    for name, obj in sorted(vars(mod).items()):
        if name.startswith("__") or isinstance(obj, types.ModuleType):
            continue
        if id(obj) in singletons:
            continue
        # Compiling a helper holds an extra ref to its func_globals via the
        # artifact; ROI backoff then drops that ref when enough exception-
        # path calls withdraw the artifact.  That is not a call-path leak.
        if id(obj) in func_globals:
            continue
        targets[name] = obj

    cases = [
        (n, f)
        for n, f in sorted(vars(mod).items())
        if callable(f) and n.startswith("case_")
    ]

    def snapshot():
        # 类型方法缓存（MCACHE）在 3.11 对缓存名字持强引用，条目被
        # 碰撞驱逐时合法释放——任何曾作为查找名的字符串目标都会因此
        # 产生与被测代码无关的 ±1 漂移（闪烁案 case_sub_index_protocol/
        # _pname：函数内建类使 tp_version 流水与名字哈希在窗口内随机
        # 撞槽驱逐旧名；jit 模式因拉式验证/IC 填充的额外类型查找改变
        # 缓存流量而更易触发）。快照前清空缓存，两侧均无 MCACHE 持
        # 引用，判据确定化。
        sys._clear_type_cache()
        return {n: sys.getrefcount(o) for n, o in targets.items()}

    def entries() -> int:
        if jit is None:
            return 0
        import _cinderx

        return _cinderx._get_trigger_stats()["machine_code_entries"]

    results = {}
    outcomes = {}
    entry_counts = {}
    chosen = {}
    for name, fn in cases:
        print(f"refcount-matrix: running {name}", flush=True)
        fns = [fn] + list(getattr(fn, "helpers", ()))
        if jit is not None:
            to_compile = [
                f for f in fns if isinstance(f, types.FunctionType)
            ]
            # A case that names helpers is a driver for them: the helpers
            # are the per-shape code objects under test, and the driver's
            # own body may legitimately sit outside the execute surface
            # (corpus_ic_mutation's cases build classes and delete
            # attributes; corpus_calls builds 285 nested dispatchers from
            # one shared `def case` code object).  Compile the helpers and
            # leave the driver interpreted.
            #
            # Until MR-11 this was also the only way the corpus_calls
            # dispatchers could be compiled at all, because one artifact
            # admitted one owning function.  That restriction is gone --
            # same-namespace functions now share a code object's artifact
            # -- but the driver-vs-helper split stands on its own.
            if fn in to_compile and any(f is not fn for f in to_compile):
                to_compile = [f for f in to_compile if f is not fn]
            if scheduled:
                # No force_compile(): the scheduler has to pick these up on
                # its own, which is what puts dispatch, outer anchoring and
                # fresh attachment inside the reference-count contract.
                # The warm-up calls are the same ones the case makes; only
                # their number is chosen here.
                for _ in range(SCHEDULER_WARMUP):
                    try:
                        fn()
                    except BaseException:
                        pass
                # Which functions the scheduler took is its decision, not
                # this leg's: an automatic surface narrower than
                # force_compile()'s, and the exception-rate freeze that
                # returns a function whose every call raises to the
                # interpreter, are both designed behaviour.  Record the
                # partition instead of demanding all of it -- what must
                # not happen is the set quietly emptying, which the pinned
                # floor in the diff catches.
                chosen[name] = sorted(
                    getattr(f, "__name__", repr(f))
                    for f in to_compile
                    if jit.is_jit_compiled(f)
                )
                to_compile = []
            for f in to_compile:
                try:
                    jit.force_compile(f)
                except RuntimeError as exc:
                    ops = [
                        instr.opname
                        for instr in __import__("dis").get_instructions(f)
                    ]
                    print(
                        f"refcount-matrix: force_compile refused for "
                        f"{name} ({getattr(f, '__name__', f)}): {exc}; "
                        f"opcodes={ops}",
                        file=sys.stderr,
                    )
                    return 2
                if not jit.is_jit_compiled(f):
                    print(
                        f"refcount-matrix: force_compile refused for "
                        f"{name}; jit-mode evidence requires compiled "
                        f"execution",
                        file=sys.stderr,
                    )
                    return 2
        # Warm up once: first-call effects (caches, quickening) are not part
        # of the steady-state drift contract.  The warm-up call is also the
        # one whose result is recorded, so the two arms can be compared on
        # what the case actually computed and not only on its bookkeeping.
        outcomes[name] = outcome_of(fn)
        gc.collect()
        before = snapshot()
        entries_before = entries()
        for _ in range(N):
            try:
                fn()
            except BaseException:
                pass
        gc.collect()
        after = snapshot()
        entry_counts[name] = entries() - entries_before
        # A case the scheduler declined runs interpreted on purpose, so
        # only the ones it took owe machine-code entries.
        owes_entries = executing and (not scheduled or chosen.get(name))
        if owes_entries and entry_counts[name] <= 0:
            print(
                f"refcount-matrix: {name} was compiled but never entered "
                f"machine code across {N} iterations",
                file=sys.stderr,
            )
            return 2
        drift = {
            n: after[n] - before[n] for n in before if after[n] != before[n]
        }
        results[name] = drift

    json.dump(
        {
            "module": modname,
            "mode": mode,
            "iterations": N,
            "drift": results,
            "outcome": outcomes,
            "executing": executing,
            "machine_code_entries": entry_counts,
            # Only the scheduled arm has a partition; recording an empty
            # one for the force_compile arm would report "the scheduler
            # took 0 of 0 cases" about a leg that never asked it to.
            "scheduler_compiled": chosen if scheduled else None,
        },
        open(out_path, "w"),
        indent=1,
        sort_keys=True,
    )
    drifting = {k: v for k, v in results.items() if v}
    print(f"{modname} [{mode}]: {len(cases)} cases, {len(drifting)} with drift")
    return 0


if __name__ == "__main__":
    sys.exit(main())
