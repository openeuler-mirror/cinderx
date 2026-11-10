// Copyright (c) Meta Platforms, Inc. and affiliates.
#include "cinderx/Common/spike_env.h"

#include "cinderx/python.h"

#include <cstring>
#include <string>
#include <vector>

extern "C" char** environ;

namespace {

struct SpikeEnvEntry {
  std::string name;
  std::string value;
};

const std::vector<SpikeEnvEntry>& spikeEnvTable() {
  // Function-local static initialization publishes the complete snapshot
  // once, including when distinct compile workers request different flags.
  static const std::vector<SpikeEnvEntry> table = [] {
    std::vector<SpikeEnvEntry> entries;
    if (environ != nullptr) {
      for (char** ep = environ; *ep != nullptr; ++ep) {
        const char* entry = *ep;
        if (std::strncmp(entry, "SPIKE_", 6) != 0) {
          continue;
        }
        const char* eq = std::strchr(entry, '=');
        if (eq != nullptr) {
          entries.push_back(
              {std::string(entry, static_cast<size_t>(eq - entry)),
               std::string(eq + 1)});
        }
      }
    }
    return entries;
  }();
  return table;
}

} // namespace

extern "C" void Ci_SpikeEnvSnapshot(void) {
  (void)spikeEnvTable();
}

// CPython 3.11 features enabled by default ("NAME=value"); consulted before
// the environment. Keep other Python versions on their existing defaults.
// MRSIM_DEFAULTS_BEGIN
static const char* const kSimDefaults[] = {
#if PY_VERSION_HEX < 0x030C0000
    "SPIKE_SUBSCR=1",
    "SPIKE_ARRAYSTUCK=1",
    "SPIKE_ARRIDX=1",
    "SPIKE_SLICEFAST=1",
    "SPIKE_STRSUB=1",
    "SPIKE_GISUB=1",
    "SPIKE_STRCHR=1",
    "SPIKE_STRSPEC=1",
    "SPIKE_UEQ=1",
    "SPIKE_ISINSTFAST=1",
    "SPIKE_ISINSTCACHE=1",
    "SPIKE_ISINSTINLINE=1",
    "SPIKE_STRLITEQ=1",
    "SPIKE_ISINSTGEN=1",
    "SPIKE_GETATTRDFL=1",
    "SPIKE_IPTYPED=1",
    "SPIKE_CMPTYPED=1",
    "SPIKE_CMPJUMP=1",
    "SPIKE_FLOATSPEC=1",
    "SPIKE_BINTYPED=1",
    "SPIKE_FLOATFL=1",
    "SPIKE_FLOATFLASM=1",
    "SPIKE_LONGSPEC=1",
    "SPIKE_INTBITSPEC=1",
    "SPIKE_INT64ARITH=1",
    "SPIKE_MIXEDNUM=1",
    "SPIKE_ARRTC=1",
    "SPIKE_RANGEITER=1",
    "SPIKE_BOXDIRECT=1",
    "SPIKE_PHIUNBOX=1",
    "SPIKE_PHIUNBOXF=1",
    "SPIKE_TUPLEFL=1",
    "SPIKE_HIRCSE=1",
    "SPIKE_COLDSINK=1",
    "SPIKE_DEOPTSINK=1",
#endif
    nullptr,
};
// MRSIM_DEFAULTS_END

extern "C" const char* Ci_SpikeGetenv(const char* name) {
  for (const char* const* d = kSimDefaults; *d != nullptr; ++d) {
    const char* eq = std::strchr(*d, '=');
    if (eq != nullptr &&
        std::strncmp(*d, name, static_cast<size_t>(eq - *d)) == 0 &&
        name[eq - *d] == '\0') {
      return eq + 1;
    }
  }
  Ci_SpikeEnvSnapshot();
  for (const SpikeEnvEntry& entry : spikeEnvTable()) {
    if (entry.name == name) {
      return entry.value.c_str();
    }
  }
  return nullptr;
}
