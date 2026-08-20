// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstddef>
#include <cstdint>

namespace jit {

// Monotonic process-wide counters backing the trigger-proof report
// (ci_pipeline/jit311): tests assert not only on observable behaviour but on
// whether the machinery behind it actually fired.  On builds whose capability
// gate keeps the JIT unreachable these prove a negative -- executable memory
// was never allocated and no CompiledFunction ever existed -- and on
// execution-capable builds they prove the positive.
//
// Every increment site is a compilation-side cold path; per-call hot paths
// stay untouched (machine_code_entries is incremented by the CPython 3.11
// entry glue once that ships, and stays zero until then).
struct TriggerStats {
  // Number of executable-memory allocation calls made by the JIT's code
  // allocator, and the bytes they requested.
  uint64_t executable_alloc_calls;
  uint64_t executable_alloc_bytes;
  // Number of CompiledFunction objects ever created.
  uint64_t compiled_function_creations;
  // Number of times control entered JIT-compiled machine code.
  uint64_t machine_code_entries;
  // Number of complete shadow compilations, specialized bytecode operations
  // consumed by them, and generated (but never installed) code bytes.
  uint64_t shadow_compile_success;
  uint64_t shadow_specialized_opcodes_consumed;
  uint64_t shadow_codegen_bytes;
  // Death notifications delivered (code / function).  3.11 has no watchers;
  // these prove the substitutes fire -- a leak shows as counts that stop
  // climbing while objects keep dying.
  uint64_t code_destroyed_notifications;
  uint64_t function_destroyed_notifications;
  // Live code buffers: incremented when a CompiledFunction takes ownership
  // of executable memory, decremented when its destructor gives it up.  A
  // GAUGE, not a counter: this is the physical residency measurement, and
  // it tracks the buffer's real lifetime -- an artifact removed from every
  // registry but kept alive by an external reference still holds its
  // machine code, and must still count.
  uint64_t resident_code_buffers;
};

// Increment sites.  Relaxed atomics: the counters order nothing.
void triggerStatsOnExecutableAlloc(std::size_t bytes);
void triggerStatsOnCompiledFunctionCreate();
void triggerStatsOnMachineCodeEntry();
void triggerStatsOnShadowCompile(
    std::size_t code_bytes,
    uint64_t specialized_opcodes);
void triggerStatsOnCodeDestroyed();
void triggerStatsOnFunctionDestroyed();
void triggerStatsOnCodeBufferAcquired();
void triggerStatsOnCodeBufferReleased();

// Read a consistent-enough snapshot for reporting.
TriggerStats triggerStatsSnapshot();

} // namespace jit
