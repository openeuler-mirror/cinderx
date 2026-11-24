# CPython 3.11.6 interpreter patches

These patches adapt the vendored CPython 3.11.6 interpreter. The tracked
`upstream/ceval.c` and `upstream/specialize.c` remain unchanged. The top-level
CMake build checks their SHA256 hashes, copies them into `ci311-attr/` in the
build directory, and applies the following patches in order. The two wrapper
translation units include those patched copies.

| Order | Patch | Target | Purpose and constraints |
|---|---|---|---|
| 1 | `0001-specialize-getattr-only-types.patch` | `specialize.c` | Permit recognized getattr-only types to proceed through descriptor analysis. Reject general-key dictionaries conservatively; retain existing descriptor, shadowing and version checks. The shared dictionary helper also affects STORE_ATTR eligibility. |
| 2 | `0002-load-attr-with-hint-split-dict.patch` | `ceval.c` / LOAD_ATTR_WITH_HINT | Read split dictionaries through `ma_values`; keep name/index/empty-value guards and fall back for general-key dictionaries. |
| 3 | `0003-global-name-cache.patch` | `ceval.c` / LOAD_GLOBAL_ADAPTIVE | Try the portable name cache before repeated unsuccessful specialization. Keep adaptive-counter progress and stack/reference ownership. The implementation and full dictionary-version guards live in `global_cache311.h`. |
| 4 | `0004-jit-off-inline-call.patch` | `ceval.c` / CALL, CALL_PY_EXACT_ARGS, CALL_PY_WITH_DEFAULTS | Permit the existing inline frame path only when `Ci_CallInlineOK311()` succeeds. That guard requires JIT-off, evaluator ownership, default Python vectorcall, no trace/profile, no observer or conflicting JIT state/artifact. Parameter binding and frame construction remain upstream logic. |

The fourth patch follows the third; its context is located after the global
cache insertion. The generated wheel evaluator preserves dev's coroutine
construction guard in generic `CALL` and `CALL_PY_EXACT_ARGS`, and additionally
allows the JIT-off inline call path. The standalone interpreter keeps its
coroutine-only generated source.

## Validation

The attribute, cache, call and startup regressions are documented in
[`docs/attribute-path-311.md`](../../../../docs/attribute-path-311.md).
`test_cinderx.test_kunpeng.test_interp_patch_build_311` checks each patch and
compares incremental outputs with a clean configure.

`apply.cmake` registers the pinned source files and all four patches in
`CMAKE_CONFIGURE_DEPENDS`, so edits trigger configuration when invoking
`cmake --build`.

`apply.cmake` rejects input-hash mismatches and failed patch commands. It does
not currently verify a post-patch hash or implement a general patch management
framework. Other Python versions and the separate standalone CMake entry point
are outside this patch application path.
