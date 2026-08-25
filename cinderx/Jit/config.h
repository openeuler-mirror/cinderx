// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jit {

// Lifetime transitions of the JIT compiler:
//
//   Non-executing: NotInitialized -> Shadow -> Finalizing -> NotInitialized
//   Executing:     NotInitialized -> Running <-> Paused
//                                      |           |
//                                      +-> Finalizing -> NotInitialized
//
// Shadow never transitions to Running or Paused.
enum class State : uint8_t {
  kNotInitialized,
  // The compiler pipeline is available, but generated code is never
  // published or entered.
  kShadow,
  kRunning,
  kPaused,
  kFinalizing,
};

enum class FrameMode : uint8_t {
  kNormal,
  kLightweight,
};

// List of HIR optimization passes to run.
struct HIROptimizations {
  bool begin_inlined_function_elim{true};
  bool builtin_load_method_elim{true};
  bool clean_cfg{true};
  bool dead_code_elim{true};
  bool dynamic_comparison_elim{true};
  bool float_accumulator_promotion{true};
  bool guard_type_removal{true};
  bool inliner{true};
  bool insert_update_prev_instr{true};
  bool list_prefix_reverse_assign{true};
  bool phi_elim{true};
  bool tree_iter_state_machine{true};
  bool primitive_box_remat{true};
  bool primitive_unbox_cse{true};
  bool float_comparison_simplification{true};
  bool simplify{true};
};

// List of LIR optimization passes to run.
struct LIROptimizations {
  bool inliner{true};
};

struct SimplifierConfig {
  // The maximum number of times the simplifier can process a function's CFG.
  size_t iteration_limit{100};
  // The maximum number of new blocks that can be added by the simplifier to a
  // function.
  size_t new_block_limit{1000};
};

struct GdbOptions {
  // Whether GDB support is enabled.
  bool supported{false};
  // Whether to write generated ELF objects to disk.
  bool write_elf_objects{false};
};

struct JitListOptions {
  // Name of the file loaded in as a JIT list.
  std::string filename;
  // Raise a Python error when a line fails to parse.
  bool error_on_parse{false};
  // Use line numbers or not when checking if a function is on a JIT list.
  bool match_line_numbers{false};
};

struct LogOptions {
  // Log general debug messages from the JIT.
  bool debug{false};
  // Log debug messages in the inlining pass.
  bool debug_inliner{false};
  // Log debug messages in the refcount insertion pass.
  bool debug_refcount{false};
  // Log debug messages in the register allocation pass.
  bool debug_regalloc{false};

  // Log HIR, before any passes are run.
  bool dump_hir_initial{false};
  // Log HIR after every pass is run.
  bool dump_hir_passes{false};
  // Log HIR after all passes have been run.
  bool dump_hir_final{false};

  // Log LIR, across all stages.
  bool dump_lir{false};
  // Show the originating HIR instruction for LIR instruction blocks.
  bool lir_origin{true};

  // Log disassembly of compiled functions.
  bool dump_asm{false};
  // Symbolize functions in disassembled call instructions.
  bool symbolize_funcs{true};

  // Log general JIT stats.
  bool dump_stats{false};

  // The file where to write logs to.
  FILE* output_file{stderr};
};

enum class AsmSyntax : uint8_t {
  ATT,
  Intel,
};

// Collection of configuration values for the JIT.
//
// Note: It's fine to store non-trivially destructible objects like std::string
// in this.  It is *not fine* to store Python objects in this because it has
// process lifetime and outlives the Python runtime.
struct Config {
  // Current lifetime state of the JIT.
  State state{State::kNotInitialized};
  // Ignore other CLI arguments and environment variables, force the JIT
  // to be initialized or uninitialized.  Intended for testing.
  std::optional<bool> force_init;
  FrameMode frame_mode{
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
      FrameMode::kLightweight
#else
      FrameMode::kNormal
#endif
  };
  bool allow_jit_list_wildcards{false};
  bool compile_all_static_functions{false};
  bool multiple_code_sections{false};
  bool multithreaded_compile_test{false};
  bool use_huge_pages{true};
  // Use huge pages for cold code sections as well. Only applicable when
  // multiple_code_sections is enabled. Defaults to false (regular pages).
  bool cold_code_huge_pages{false};
  // Assume that data found in the Python frame is unchanged across function
  // calls.  This includes the code object, and the globals and builtins
  // dictionaries (but not their contents).
  bool stable_frame{true};
  // Use inline caches for attribute accesses.
  bool attr_caches{
#ifdef Py_GIL_DISABLED
      // TODO(T250369692): FT support for inline-caches.
      false
#else
      true
#endif
  };
  // Collect stats information about attribute caches.
  bool collect_attr_cache_stats{false};
  // Use type annotations to create runtime checks.
  bool emit_type_annotation_guards{false};
  // Whether or not to JIT specialized opcodes or to fall back to their generic
  // counterparts.
  bool specialized_opcodes{true};
  // Enable OSR hot-loop detection. OSR is production-off by default and must
  // be explicitly enabled by -X osr-enabled or CINDERX_OSR_ENABLED.
  bool osr_enabled{false};
  // Set only by the early JIT initialization path when backedges can still be
  // routed to JUMP_BACKWARD_JIT consistently.
  bool osr_capable{false};
  // Number of executions of a single backedge before attempting OSR.
  uint32_t osr_backedge_threshold{2000};
  // Compile budget knobs consumed by later OSR feature items.
  uint32_t osr_compile_budget_code_units{1024};
  uint32_t osr_compile_warn_threshold_ms{50};

  // Only emit exact-int guards for specialized numeric opcodes in code objects
  // that contain a loop backedge. Disable to restore the old unconditional
  // guard behavior.
  bool backedge_gated_int_guards{true};

  // Support instrumentation (monitoring/tracing/profiling) by falling back to
  // the interpreter
  bool support_instrumentation{false};

  // Permit CPython 3.11 synchronous generators on the execute surface.
  // Explicit force/canary compilation is enabled (MR-10); automatic
  // compilation of generators stays off by policy (MR-11): the measured
  // verdict is that compiling generators only on request beats both
  // compiling them all and interpreting them all.
  bool sync_generator_jit{true};

  // CPython 3.11 auto-JIT (MR-11): how many fresh function objects over an
  // already-compiled code object may attach to its artifact automatically.
  // 3.11 has no function-creation watcher, so a fresh function (a closure,
  // lambda or comprehension re-created per call) is noticed at its first
  // interpreted frame and attached for its later calls.  Each attachment
  // costs a full publication; the budget keeps churn-heavy shapes (a new
  // closure per call, called once) from paying it forever, while stable
  // instance sets attach in full.  0 disables automatic attachment;
  // force_compile() of a fresh function is never budgeted.
  uint32_t fresh_attach_budget{8};

  // Add RefineType instructions for Static Python values before they get
  // typechecked.  Enabled by default as HIR doesn't pass through Static Python
  // types very well right now.  Disable to expose new typing opportunities in
  // HIR.
  //
  // TASK(T195042385): Replace this with actual typing.
  bool refine_static_python{true};
  HIROptimizations hir_opts;
  LIROptimizations lir_opts;
  SimplifierConfig simplifier;
  // Limit on how much the inliner can inline.  The number here is internal to
  // the inliner, doesn't have any specific meaning, and can change as the
  // inliner's algorithm changes.
  size_t inliner_cost_limit{2000};
  size_t inliner_cold_call_threshold{20};
  // Number of workers to use for batch compilation, like in precompile_all().
  // If this number isn't configured then batch compilation will happen inline
  // on the calling thread.
  size_t batch_compile_workers{0};
  // When a function is being compiled, this is the maximum number of dependent
  // functions called by it that can be compiled along with it.
  size_t preload_dependent_limit{99};
  // Memory threshold after which we stop jitting.
  size_t max_code_size{0};
  // Size (in number of entries) of the LoadAttrCached and StoreAttrCached
  // inline caches used by the JIT.
  uint32_t attr_cache_size{4};
  std::optional<uint32_t> compile_after_n_calls;
  // Enable AutoJIT behavior classification for PYTHONJITAUTO=auto[:N]. Plain
  // numeric PYTHONJITAUTO and Python APIs keep this disabled.
  bool auto_classify{false};
  // Provider-before v1 keeps startup/import deferral disabled. This is only
  // for a later import-depth provider-backed slice.
  bool enable_startup_init_policy{false};
  // Minimal dynamic feedback for code that compiles successfully but then
  // repeatedly deopts. Enabled by default; disable with
  // CINDERX_AUTOJIT_ROI_BACKOFF=0 when isolating A/B or rolling back.
  bool roi_backoff_enabled{true};
  // Canonicalize exec-generated namespace-free content-twin code objects
  // (factory helpers recreated per instantiation) onto the first-seen
  // identity at compile time, so twins attach to the existing compiled
  // artifact instead of recompiling. Disable with
  // CINDERX_AUTOJIT_CODE_DEDUP=0 when isolating A/B.
  bool auto_code_twin_dedup{true};
  // Number of held calls a process must accumulate before steady-state
  // LowRoi shapes stop being deferred. The counter measures the interpreted
  // executions that the release would have compiled, so reaching the budget
  // demonstrates that speculative compilation can amortize. Short-lived
  // interpreter invocations make a few hundred such calls in total and never
  // reach it; workloads reach it within their first warmup moments. Call
  // counts are properties of the program, independent of machine speed,
  // architecture and integration topology. 0 releases immediately. Override
  // with CINDERX_AUTOJIT_LOWROI_WARM_CALLS.
  size_t auto_classify_low_roi_warm_calls{4096};
  size_t roi_deopt_budget_base{32};
  size_t roi_backoff_max_rounds{1};
  size_t roi_rewarm_factor{64};
  GdbOptions gdb;
  JitListOptions jit_list;
  LogOptions log;
  bool compile_perf_trampoline_prefork{false};
  bool dump_hir_stats{false};

  // The ASM syntax the JIT should use when disassembling.
  AsmSyntax asm_syntax{AsmSyntax::ATT};

  // List of function name patterns for which to capture compilation times.
  std::vector<std::string> capture_compilation_times_for;

  // Option to force compiled functions to be immortalized. By default a
  // CompiledFunction's timetime will be tied to a function via a reference put
  // in the function's __dict__. When we force CompiledFunction's to always be
  // immortalized no such reference will be created and the CompiledFunction
  // will be set to be immortal and never collected.
  bool immortalize_compiled_functions{false};

  // Use stable sentinel pointers in output (for deterministic test output).
  bool use_stable_pointers{false};

  // Delay adaptive specialization until a function has been called enough
  // times.
  bool delay_adaptive_code{false};
  // Number of calls before adaptive specialization kicks in.
  uint64_t adaptive_threshold{80};
};

// The JIT's config object. The accessors defined below are used in very hot
// paths in the JIT and need to be defined in a header to ensure that they are
// inlined reliably without LTO.
extern Config s_jit_config;

// Get the JIT's current config object.
inline const Config& getConfig() {
  return s_jit_config;
}

// Get the JIT's current config object with the intent of modifying it.
inline Config& getMutableConfig() {
  return s_jit_config;
}

// Check that the JIT has initialized state. It may be shadow-compiling, paused,
// or finalizing, so this does not imply that machine-code execution is usable.
bool isJitInitialized();

// Check that the JIT is initialized and is currently usable.
bool isJitUsable();

// Check that only the non-executing shadow compiler is initialized.
bool isJitShadow();

// Check that the JIT is initialized but currently paused and unusable.
bool isJitPaused();

} // namespace jit
