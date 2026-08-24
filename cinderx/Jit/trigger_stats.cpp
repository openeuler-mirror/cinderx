// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/trigger_stats.h"

#include "cinderx/Common/ref.h"

#include <atomic>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>

namespace jit {

namespace {

std::atomic<uint64_t> s_executable_alloc_calls{0};
std::atomic<uint64_t> s_executable_alloc_bytes{0};
std::atomic<uint64_t> s_compiled_function_creations{0};
std::atomic<uint64_t> s_machine_code_entries{0};
std::atomic<uint64_t> s_shadow_compile_success{0};
std::atomic<uint64_t> s_shadow_specialized_opcodes_consumed{0};
std::atomic<uint64_t> s_shadow_codegen_bytes{0};
std::atomic<uint64_t> s_code_destroyed_notifications{0};
std::atomic<uint64_t> s_function_destroyed_notifications{0};
std::atomic<uint64_t> s_resident_code_buffers{0};
std::atomic<uint64_t> s_forced_deopt_hits{0};
std::atomic<uint64_t> s_organic_deopt_hits{0};

#if PY_VERSION_HEX < 0x030C0000
struct A1EntryLedgerRow {
  std::string filename;
  std::string qualname;
  int firstlineno;
  uint64_t entries;
};

std::atomic<bool> s_a1_entry_ledger_enabled{false};
std::atomic<uint64_t> s_a1_entry_ledger_dropped{0};
std::unordered_map<PyCodeObject*, A1EntryLedgerRow> s_a1_entry_ledger;
std::map<std::tuple<std::string, int, std::string>, uint64_t>
    s_a1_entry_ledger_archived;
#endif

} // namespace

void triggerStatsOnExecutableAlloc(std::size_t bytes) {
  s_executable_alloc_calls.fetch_add(1, std::memory_order_relaxed);
  s_executable_alloc_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void triggerStatsOnCompiledFunctionCreate() {
  s_compiled_function_creations.fetch_add(1, std::memory_order_relaxed);
}

void triggerStatsOnMachineCodeEntry(PyCodeObject* code) {
  s_machine_code_entries.fetch_add(1, std::memory_order_relaxed);
#if PY_VERSION_HEX < 0x030C0000
  if (code == nullptr ||
      !s_a1_entry_ledger_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  auto it = s_a1_entry_ledger.find(code);
  if (it != s_a1_entry_ledger.end()) {
    it->second.entries++;
    return;
  }
  try {
    const char* filename = PyUnicode_AsUTF8(code->co_filename);
    const char* qualname = PyUnicode_AsUTF8(code->co_qualname);
    if (filename == nullptr || qualname == nullptr) {
      PyErr_Clear();
      s_a1_entry_ledger_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    s_a1_entry_ledger.emplace(
        code,
        A1EntryLedgerRow{
            filename, qualname, code->co_firstlineno, 1});
  } catch (const std::bad_alloc&) {
    s_a1_entry_ledger_dropped.fetch_add(1, std::memory_order_relaxed);
  }
#else
  (void)code;
#endif
}

void triggerStatsOnShadowCompile(
    std::size_t code_bytes,
    uint64_t specialized_opcodes) {
  s_shadow_compile_success.fetch_add(1, std::memory_order_relaxed);
  s_shadow_specialized_opcodes_consumed.fetch_add(
      specialized_opcodes, std::memory_order_relaxed);
  s_shadow_codegen_bytes.fetch_add(code_bytes, std::memory_order_relaxed);
}

void triggerStatsOnCodeDestroyed(PyCodeObject* code) {
  s_code_destroyed_notifications.fetch_add(1, std::memory_order_relaxed);
#if PY_VERSION_HEX < 0x030C0000
  if (code == nullptr) {
    return;
  }
  auto it = s_a1_entry_ledger.find(code);
  if (it == s_a1_entry_ledger.end()) {
    return;
  }
  try {
    auto key = std::make_tuple(
        it->second.filename, it->second.firstlineno, it->second.qualname);
    s_a1_entry_ledger_archived[key] += it->second.entries;
  } catch (const std::bad_alloc&) {
    s_a1_entry_ledger_dropped.fetch_add(1, std::memory_order_relaxed);
  }
  s_a1_entry_ledger.erase(it);
#else
  (void)code;
#endif
}

void triggerStatsOnFunctionDestroyed() {
  s_function_destroyed_notifications.fetch_add(1, std::memory_order_relaxed);
}

void triggerStatsOnCodeBufferAcquired() {
  s_resident_code_buffers.fetch_add(1, std::memory_order_relaxed);
}

void triggerStatsOnCodeBufferReleased() {
  s_resident_code_buffers.fetch_sub(1, std::memory_order_relaxed);
}

void triggerStatsOnForcedDeopt() {
  s_forced_deopt_hits.fetch_add(1, std::memory_order_relaxed);
}

void triggerStatsOnOrganicDeopt() {
  s_organic_deopt_hits.fetch_add(1, std::memory_order_relaxed);
}

TriggerStats triggerStatsSnapshot() {
  return TriggerStats{
      s_executable_alloc_calls.load(std::memory_order_relaxed),
      s_executable_alloc_bytes.load(std::memory_order_relaxed),
      s_compiled_function_creations.load(std::memory_order_relaxed),
      s_machine_code_entries.load(std::memory_order_relaxed),
      s_shadow_compile_success.load(std::memory_order_relaxed),
      s_shadow_specialized_opcodes_consumed.load(std::memory_order_relaxed),
      s_shadow_codegen_bytes.load(std::memory_order_relaxed),
      s_code_destroyed_notifications.load(std::memory_order_relaxed),
      s_function_destroyed_notifications.load(std::memory_order_relaxed),
      s_resident_code_buffers.load(std::memory_order_relaxed),
      s_forced_deopt_hits.load(std::memory_order_relaxed),
      s_organic_deopt_hits.load(std::memory_order_relaxed),
  };
}

void a1EntryLedgerReset() {
#if PY_VERSION_HEX < 0x030C0000
  s_a1_entry_ledger_enabled.store(false, std::memory_order_relaxed);
  s_a1_entry_ledger.clear();
  s_a1_entry_ledger_archived.clear();
  s_a1_entry_ledger_dropped.store(0, std::memory_order_relaxed);
  s_a1_entry_ledger_enabled.store(true, std::memory_order_relaxed);
#endif
}

void a1EntryLedgerDisable() {
#if PY_VERSION_HEX < 0x030C0000
  s_a1_entry_ledger_enabled.store(false, std::memory_order_relaxed);
  s_a1_entry_ledger.clear();
  s_a1_entry_ledger_archived.clear();
#endif
}

PyObject* a1EntryLedgerSnapshot() {
#if PY_VERSION_HEX < 0x030C0000
  bool was_enabled =
      s_a1_entry_ledger_enabled.exchange(false, std::memory_order_relaxed);
  std::map<std::tuple<std::string, int, std::string>, uint64_t> rows;
  try {
    rows = s_a1_entry_ledger_archived;
    for (const auto& [code, row] : s_a1_entry_ledger) {
      (void)code;
      rows[std::make_tuple(row.filename, row.firstlineno, row.qualname)] +=
          row.entries;
    }
  } catch (const std::bad_alloc&) {
    s_a1_entry_ledger_enabled.store(was_enabled, std::memory_order_relaxed);
    PyErr_NoMemory();
    return nullptr;
  }
  Ref<> entries = Ref<>::steal(PyList_New(0));
  if (entries == nullptr) {
    s_a1_entry_ledger_enabled.store(was_enabled, std::memory_order_relaxed);
    return nullptr;
  }
  for (const auto& [key, count] : rows) {
    const auto& [filename, firstlineno, qualname] = key;
    Ref<> item = Ref<>::steal(Py_BuildValue(
        "{s:s,s:s,s:i,s:K}",
        "filename",
        filename.c_str(),
        "qualname",
        qualname.c_str(),
        "firstlineno",
        firstlineno,
        "entries",
        static_cast<unsigned long long>(count)));
    if (item == nullptr || PyList_Append(entries, item) < 0) {
      s_a1_entry_ledger_enabled.store(was_enabled, std::memory_order_relaxed);
      return nullptr;
    }
  }
  Ref<> result = Ref<>::steal(PyDict_New());
  Ref<> dropped = Ref<>::steal(PyLong_FromUnsignedLongLong(
      s_a1_entry_ledger_dropped.load(std::memory_order_relaxed)));
  if (result == nullptr || dropped == nullptr ||
      PyDict_SetItemString(result, "entries", entries) < 0 ||
      PyDict_SetItemString(result, "dropped", dropped) < 0) {
    s_a1_entry_ledger_enabled.store(was_enabled, std::memory_order_relaxed);
    return nullptr;
  }
  s_a1_entry_ledger_enabled.store(was_enabled, std::memory_order_relaxed);
  return result.release();
#else
  PyErr_SetString(
      PyExc_NotImplementedError,
      "the A1 per-code entry ledger exists only on CPython 3.11");
  return nullptr;
#endif
}

} // namespace jit
