// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/autojit_import.h"

#include <cstdlib>
#include <limits>
#include <string_view>

namespace jit {

namespace {

thread_local uint32_t t_auto_jit_import_depth{0};
thread_local uint32_t t_auto_jit_setup_depth{0};

bool isEnabledProvider(std::string_view provider) {
  return provider == "builtins" || provider == "find_and_load";
}

} // namespace

} // namespace jit

// The CPython 3.11 frame observer skips threads inside an import/setup scope:
// scheduling defers there anyway, so counting and asking the scheduler on
// every call is pure overhead.  Ci_AutoJit_OpenScopes counts open scopes over
// all threads, so the common case (no scope open) costs one relaxed load
// before the thread-local check.
extern "C" {
uint32_t Ci_AutoJit_OpenScopes = 0;
int Ci_AutoJit_ThreadInScope(void) {
  return jit::t_auto_jit_import_depth > 0 || jit::t_auto_jit_setup_depth > 0;
}
}

namespace jit {

void autoJitImportEnter() {
  if (t_auto_jit_import_depth != std::numeric_limits<uint32_t>::max()) {
    ++t_auto_jit_import_depth;
    __atomic_fetch_add(&Ci_AutoJit_OpenScopes, 1, __ATOMIC_RELAXED);
  }
}

void autoJitImportLeave() {
  if (t_auto_jit_import_depth > 0) {
    --t_auto_jit_import_depth;
    __atomic_fetch_sub(&Ci_AutoJit_OpenScopes, 1, __ATOMIC_RELAXED);
  }
}

uint32_t autoJitImportDepth() {
  return t_auto_jit_import_depth;
}

void autoJitSetupEnter() {
  if (t_auto_jit_setup_depth != std::numeric_limits<uint32_t>::max()) {
    ++t_auto_jit_setup_depth;
    __atomic_fetch_add(&Ci_AutoJit_OpenScopes, 1, __ATOMIC_RELAXED);
  }
}

void autoJitSetupLeave() {
  if (t_auto_jit_setup_depth > 0) {
    --t_auto_jit_setup_depth;
    __atomic_fetch_sub(&Ci_AutoJit_OpenScopes, 1, __ATOMIC_RELAXED);
  }
}

uint32_t autoJitSetupDepth() {
  return t_auto_jit_setup_depth;
}

uint32_t autoJitStartupDepth() {
  uint32_t import_depth = autoJitImportDepth();
  uint32_t setup_depth = autoJitSetupDepth();
  if (std::numeric_limits<uint32_t>::max() - import_depth < setup_depth) {
    return std::numeric_limits<uint32_t>::max();
  }
  return import_depth + setup_depth;
}

bool autoJitImportProviderEnabledFromEnv() {
  const char* provider = std::getenv("CINDERX_AUTOJIT_IMPORT_PROVIDER");
  if (provider == nullptr) {
    return true;
  }
  return isEnabledProvider(provider);
}

} // namespace jit
