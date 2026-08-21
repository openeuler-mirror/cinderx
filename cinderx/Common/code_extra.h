// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <stdint.h>

// skey_word bit layout for cached AutoJIT structure keys:
//   bit 31: classification payload is valid.
//   bit 30: steady-state policy decided the code object is cold enough to stop
//           per-frame call counting.
//   bits 0-24: packed StructureKey payload (bit 24: self-contained EAFP
//              idiom flag). The gap between bit 30 and bit 24 is reserved
//              for future flags so payload width can stay stable.
#define CI_CODE_EXTRA_SKEY_VALID_BIT 0x80000000u
#define CI_CODE_EXTRA_SKEY_DECIDED_COLD_BIT 0x40000000u
#define CI_CODE_EXTRA_SKEY_PAYLOAD_MASK 0x01FFFFFFu

// roi_ctl bit layout for dynamic negative-ROI backoff:
//   bit 31: code object is frozen for this process.
//   bit 30: a deopt thread is already performing uncompile/backoff work.
//   bits 24-27: completed backoff round count.
#define CI_CODE_EXTRA_ROI_FROZEN_BIT 0x80000000u
#define CI_CODE_EXTRA_ROI_PENDING_BIT 0x40000000u
#define CI_CODE_EXTRA_ROI_ROUND_SHIFT 24
#define CI_CODE_EXTRA_ROI_ROUND_MASK 0x0F000000u

// jit311_ctl bit layout for the CPython 3.11 auto-JIT (execute mode):
//   bit 31: automatic compilation is disabled for this code object -- its
//           one scheduling attempt failed or was refused, and the scheduler
//           must never ask again (explicit force_compile is unaffected).
//   bits 0-15: fresh function objects that were attached to the code's
//              published artifact, counted against the per-code budget.
// Unused on 3.12+, where function watchers deliver the same information.
#define CI_CODE_EXTRA_JIT311_AUTO_DISABLED_BIT 0x80000000u
#define CI_CODE_EXTRA_JIT311_ATTACH_MASK 0x0000FFFFu

#ifdef __cplusplus
extern "C" {
#endif

// Extra data attached to a code object.
typedef struct CodeExtra {
  union {
    // Number of times the code object has been called.
    uint64_t calls;
    // Used for unallocated free list code extras
    struct CodeExtra* next;
  };
  // Cached JIT-compiled entry for this code object. When jit_compiled is
  // non-NULL the code has been JIT-compiled with (jit_globals, jit_builtins),
  // letting a newly created PyFunctionObject with the same globals/builtins
  // skip the compiled_codes_ hashmap lookup in jit::Context. These are borrowed
  // pointers (jit::CompiledFunction* and PyObject*) owned by the JIT; the JIT
  // clears them before the CompiledFunction is freed (see context.cpp). Stored
  // as void* so the C interpreter can include this header. Accessed only by the
  // JIT under the free-threaded entrypoint guard; published/read with
  // release/acquire ordering (see context.cpp / pyjit.cpp).
  void* jit_compiled;
  void* jit_globals;
  void* jit_builtins;
  // Cached AutoJIT behavior classification. bit31 is the valid bit; the low
  // 25 bits (0-24) are a StructureKey payload. Zero-initialized means
  // unclassified.
  uint32_t skey_word;
  // Per-code dynamic negative-ROI backoff state. These fields are runtime
  // feedback and intentionally do not participate in StructureKey identity.
  uint32_t roi_deopt_count;
  uint32_t roi_ctl;
  // CPython 3.11 auto-JIT bookkeeping (CI_CODE_EXTRA_JIT311_*).  Sits in
  // the padding before roi_recompile_floor, so the block keeps its size on
  // every version; only the 3.11 scheduler reads or writes it.
  uint32_t jit311_ctl;
  uint64_t roi_recompile_floor;
} CodeExtra;

// Thread-safe accessors for CodeExtra::calls.
// Under FT-Python, these use atomics to avoid data races.
#ifdef Py_GIL_DISABLED

// Note: _Py_atomic_add_uint64 uses seq_cst ordering, which might be stronger
// than needed for the calls counter. On x86-64, this is the same cost as
// relaxed (both emit lock xaddq). On ARM, a relaxed variant would be cheaper
// but there is no _Py_atomic_add_uint64_relaxed.
static inline void Ci_code_extra_incr_calls(CodeExtra* extra) {
  _Py_atomic_add_uint64(&extra->calls, 1);
}

static inline uint64_t Ci_code_extra_get_calls(const CodeExtra* extra) {
  return _Py_atomic_load_uint64_relaxed(&extra->calls);
}

static inline uint32_t Ci_code_extra_load_skey_acquire(const CodeExtra* extra) {
  return __atomic_load_n(&extra->skey_word, __ATOMIC_ACQUIRE);
}

static inline void Ci_code_extra_store_skey_release(
    CodeExtra* extra,
    uint32_t word) {
  __atomic_store_n(&extra->skey_word, word, __ATOMIC_RELEASE);
}

static inline void Ci_code_extra_or_skey_release(
    CodeExtra* extra,
    uint32_t word) {
  __atomic_fetch_or(&extra->skey_word, word, __ATOMIC_RELEASE);
}

static inline uint32_t Ci_code_extra_load_roi_ctl_relaxed(
    const CodeExtra* extra) {
  return __atomic_load_n(&extra->roi_ctl, __ATOMIC_RELAXED);
}

static inline void Ci_code_extra_store_roi_ctl_release(
    CodeExtra* extra,
    uint32_t word) {
  __atomic_store_n(&extra->roi_ctl, word, __ATOMIC_RELEASE);
}

static inline int Ci_code_extra_cas_roi_ctl_release(
    CodeExtra* extra,
    uint32_t* expected,
    uint32_t desired) {
  return __atomic_compare_exchange_n(
      &extra->roi_ctl,
      expected,
      desired,
      0,
      __ATOMIC_RELEASE,
      __ATOMIC_RELAXED);
}

static inline uint32_t Ci_code_extra_incr_roi_deopt_count(CodeExtra* extra) {
  uint32_t old =
      __atomic_fetch_add(&extra->roi_deopt_count, 1, __ATOMIC_RELAXED);
  return old == UINT32_MAX ? UINT32_MAX : old + 1;
}

static inline void Ci_code_extra_store_roi_deopt_count_relaxed(
    CodeExtra* extra,
    uint32_t value) {
  __atomic_store_n(&extra->roi_deopt_count, value, __ATOMIC_RELAXED);
}

static inline uint64_t Ci_code_extra_load_roi_recompile_floor_relaxed(
    const CodeExtra* extra) {
  return __atomic_load_n(&extra->roi_recompile_floor, __ATOMIC_RELAXED);
}

static inline void Ci_code_extra_store_roi_recompile_floor_release(
    CodeExtra* extra,
    uint64_t floor) {
  __atomic_store_n(&extra->roi_recompile_floor, floor, __ATOMIC_RELEASE);
}

#else

static inline void Ci_code_extra_incr_calls(CodeExtra* extra) {
  extra->calls += 1;
}

static inline uint64_t Ci_code_extra_get_calls(const CodeExtra* extra) {
  return extra->calls;
}

static inline uint32_t Ci_code_extra_load_skey_acquire(const CodeExtra* extra) {
  return extra->skey_word;
}

static inline void Ci_code_extra_store_skey_release(
    CodeExtra* extra,
    uint32_t word) {
  extra->skey_word = word;
}

static inline void Ci_code_extra_or_skey_release(
    CodeExtra* extra,
    uint32_t word) {
  extra->skey_word |= word;
}

static inline uint32_t Ci_code_extra_load_roi_ctl_relaxed(
    const CodeExtra* extra) {
  return extra->roi_ctl;
}

static inline void Ci_code_extra_store_roi_ctl_release(
    CodeExtra* extra,
    uint32_t word) {
  extra->roi_ctl = word;
}

static inline int Ci_code_extra_cas_roi_ctl_release(
    CodeExtra* extra,
    uint32_t* expected,
    uint32_t desired) {
  if (extra->roi_ctl != *expected) {
    *expected = extra->roi_ctl;
    return 0;
  }
  extra->roi_ctl = desired;
  return 1;
}

static inline uint32_t Ci_code_extra_incr_roi_deopt_count(CodeExtra* extra) {
  if (extra->roi_deopt_count != UINT32_MAX) {
    extra->roi_deopt_count += 1;
  }
  return extra->roi_deopt_count;
}

static inline void Ci_code_extra_store_roi_deopt_count_relaxed(
    CodeExtra* extra,
    uint32_t value) {
  extra->roi_deopt_count = value;
}

static inline uint64_t Ci_code_extra_load_roi_recompile_floor_relaxed(
    const CodeExtra* extra) {
  return extra->roi_recompile_floor;
}

static inline void Ci_code_extra_store_roi_recompile_floor_release(
    CodeExtra* extra,
    uint64_t floor) {
  extra->roi_recompile_floor = floor;
}

#endif

// The 3.11 scheduler runs under the GIL and never on a free-threaded
// build, so plain accesses are the whole story here.
static inline int Ci_code_extra_jit311_auto_disabled(const CodeExtra* extra) {
  return (extra->jit311_ctl & CI_CODE_EXTRA_JIT311_AUTO_DISABLED_BIT) != 0;
}

static inline void Ci_code_extra_jit311_disable_auto(CodeExtra* extra) {
  extra->jit311_ctl |= CI_CODE_EXTRA_JIT311_AUTO_DISABLED_BIT;
}

static inline uint32_t Ci_code_extra_jit311_attach_count(
    const CodeExtra* extra) {
  return extra->jit311_ctl & CI_CODE_EXTRA_JIT311_ATTACH_MASK;
}

// Saturating: the budget is far below the field's range, and a saturated
// count still reads as "exhausted".
static inline void Ci_code_extra_jit311_note_attach(CodeExtra* extra) {
  uint32_t count = extra->jit311_ctl & CI_CODE_EXTRA_JIT311_ATTACH_MASK;
  if (count < CI_CODE_EXTRA_JIT311_ATTACH_MASK) {
    extra->jit311_ctl =
        (extra->jit311_ctl & ~CI_CODE_EXTRA_JIT311_ATTACH_MASK) | (count + 1);
  }
}

#ifdef __cplusplus
}
#endif
