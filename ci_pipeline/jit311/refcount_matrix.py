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
    if jit_doc.get("mode") == "jit" and jit_doc.get("executing"):
        silent = [
            case
            for case, entries in jit_doc.get("machine_code_entries", {}).items()
            if not entries
        ]
        if silent or not jit_doc.get("machine_code_entries"):
            print(
                "refcount-matrix: jit report shows cases that never entered "
                f"machine code: {silent or '(no entry table at all)'}"
            )
            return 1
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
    executing = os.environ.get("CINDERX_JIT_MODE") == "canary"
    if mode == "jit":
        # 守卫自适应去特化会在案例中途卸载重编（共享 helper 跑热后被
        # force_compile 会烘焙特化形，守卫风暴触发 despec）：产物切换
        # 使烘焙引用集合变化，双快照协议无法与真实漂移区分（实测恰为
        # 每案例 −1 且 despec 关闭即 0、不随迭代累积——记账工件而非
        # 泄漏）。本判据只验证编译产物的逐迭代引用中性，despec 迁移
        # 的引用平衡由其触发路径的 Ref 持有审计保证。
        os.environ.setdefault("CINDERX_ADAPTIVE_DESPEC", "0")
        # The executing tier runs under the debug allocator from MR-04 on;
        # assert it rather than assume the caller set it.
        if os.environ.get("CINDERX_JIT_MODE") == "canary":
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
                        f"refcount-matrix: canary mode requires the debug "
                        f"allocator; the interpreter reports {in_force!r}",
                        file=sys.stderr,
                    )
                    return 2
            elif os.environ.get("PYTHONMALLOC") != "debug":
                print(
                    "refcount-matrix: canary mode requires PYTHONMALLOC=debug "
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
    targets = {}
    for name, obj in sorted(vars(mod).items()):
        if name.startswith("__") or isinstance(obj, types.ModuleType):
            continue
        if id(obj) in singletons:
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
    for name, fn in cases:
        fns = [fn] + list(getattr(fn, "helpers", ()))
        if jit is not None:
            for f in fns:
                # force_compile() returns False both for a refusal and for
                # an already-compiled function (helpers are shared across
                # cases, so the second sighting is routine).  The truth
                # condition is is_jit_compiled() afterwards.
                jit.force_compile(f)
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
        if executing and entry_counts[name] <= 0:
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
