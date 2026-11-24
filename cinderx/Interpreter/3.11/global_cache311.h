// CPython 3.11 global-name cache. No host addresses, ELF parsing or keys
// version allocation. Only the stock-compatible generic LOAD_GLOBAL is wrapped.
#include "cinderx/Interpreter/3.11/observe.h"
#include "cinderx/UpstreamBorrow/borrowed.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CI_GLOBAL_CACHE_CAPACITY_311 1024
_Static_assert(
    sizeof(_PyLoadGlobalCache) == 10 &&
        offsetof(_PyLoadGlobalCache, index) == 2,
    "Expected CPython 3.11 LOAD_GLOBAL cache ABI");

typedef struct {
  PyDictObject* globals; // Borrowed identity; never dereferenced from cache.
  PyDictObject* builtins; // Borrowed identity; actual call arguments are live.
  PyObject* name; // Owned exact Unicode; prevents name-address ABA.
  PyObject* value; // Borrowed; read only after BOTH version guards.
  uint64_t globals_version;
  uint64_t builtins_version;
} CiGlobalCacheEntry311;

static CiGlobalCacheEntry311 ci_global_cache_311[CI_GLOBAL_CACHE_CAPACITY_311];
// PEP509 content versions identify the dictionary AND its current bindings.
// Host new_dict/copy/mutators draw from one 64-bit stream, including CinderX's
// private-clock bridge. The private clock dictionary is not exposed as a Python
// namespace. Zero is never cached; versions are never truncated or reissued.
typedef struct {
  uint64_t globals_version;
  uint64_t builtins_version;
  PyObject* name; // Owned exact Unicode, as in the conservative cache.
  PyObject* value; // Borrowed; both full versions and the name must match.
} CiCompactGlobalEntry311;
_Static_assert(sizeof(CiCompactGlobalEntry311) == 32, "Compact cache entry");
_Alignas(64) static CiCompactGlobalEntry311
    ci_compact_global_311[CI_GLOBAL_CACHE_CAPACITY_311];
static int ci_global_cache_enabled_311;
static int ci_global_cache_counting_311;
static int ci_global_cache_route_311;
static unsigned long long ci_global_cache_early_hits_311;
static unsigned long long ci_global_cache_early_misses_311;
static unsigned long long ci_global_cache_hashes_311;
static unsigned long long ci_global_cache_hits_311;
static unsigned long long ci_global_cache_misses_311;
static unsigned long long ci_global_cache_fallbacks_311;

void Ci_GlobalCacheClear311(void) {
  ci_global_cache_enabled_311 = 0;
  for (size_t i = 0; i < CI_GLOBAL_CACHE_CAPACITY_311; i++) {
    CiGlobalCacheEntry311* entry = &ci_global_cache_311[i];
    PyObject* name = entry->name;
    memset(entry, 0, sizeof(*entry));
    // Exact unicode destruction cannot invoke user-defined finalizers.
    Py_XDECREF(name);
    CiCompactGlobalEntry311* compact = &ci_compact_global_311[i];
    name = compact->name;
    memset(compact, 0, sizeof(*compact));
    Py_XDECREF(name);
  }
}

int Ci_GlobalCacheInit311(void) {
  const char* enabled = getenv("ATTR_PATH_GLOBAL_CACHE");
  if (enabled != NULL && strcmp(enabled, "0") && strcmp(enabled, "1")) {
    PyErr_SetString(PyExc_ValueError, "ATTR_PATH_GLOBAL_CACHE must be 0 or 1");
    return -1;
  }
  // Reject obsolete settings rather than silently selecting another cache
  // route.
  if (getenv("ATTR_PATH_KEYS_VERSION") != NULL) {
    PyErr_SetString(
        PyExc_ValueError,
        "ATTR_PATH_KEYS_VERSION is obsolete; use ATTR_PATH_GLOBAL_CACHE=0/1");
    return -1;
  }
  int use_cache = enabled == NULL ? Ci_Observe311_Mode() == CI_JIT_MODE_311_OFF
                                  : strcmp(enabled, "1") == 0;
  if (use_cache && Ci_Observe311_Mode() != CI_JIT_MODE_311_OFF) {
    PyErr_SetString(
        PyExc_RuntimeError, "Portable global cache requires JIT off");
    return -1;
  }
  const char* route = getenv("ATTR_PATH_GLOBAL_ROUTE");
  int selected_route;
  if (route == NULL || strcmp(route, "compact") == 0)
    selected_route = 4;
  else if (strcmp(route, "tick") == 0)
    selected_route = 3;
  else if (strcmp(route, "slot") == 0)
    selected_route = 2;
  else if (strcmp(route, "early") == 0)
    selected_route = 1;
  else if (strcmp(route, "helper") == 0)
    selected_route = 0;
  else {
    PyErr_SetString(
        PyExc_ValueError,
        "ATTR_PATH_GLOBAL_ROUTE must be helper, early, slot, tick or compact");
    return -1;
  }
  Ci_GlobalCacheClear311();
  ci_global_cache_route_311 = selected_route;
  ci_global_cache_enabled_311 = use_cache;
  ci_global_cache_counting_311 = ci_attr_diagnostics_config_311;
  return 0;
}

static inline size_t Ci_GlobalCacheHash311(
    PyDictObject* globals,
    PyObject* name) {
  uintptr_t index = ((uintptr_t)globals >> 6) ^ ((uintptr_t)name >> 3);
  index ^= index >> 11;
  if (ci_global_cache_counting_311)
    ci_global_cache_hashes_311++;
  return index & (CI_GLOBAL_CACHE_CAPACITY_311 - 1);
}

static inline PyObject* Ci_GlobalCacheHit311(
    CiGlobalCacheEntry311* entry,
    PyDictObject* globals,
    PyDictObject* builtins,
    PyObject* name) {
  if (entry->globals == globals && entry->builtins == builtins &&
      entry->name == name &&
      entry->globals_version == globals->ma_version_tag &&
      entry->builtins_version == builtins->ma_version_tag) {
    if (ci_global_cache_counting_311)
      ci_global_cache_hits_311++;
    return entry->value;
  }
  return NULL;
}

static inline PyObject* Ci_CompactGlobalHit311(
    CiCompactGlobalEntry311* entry,
    PyDictObject* globals,
    PyDictObject* builtins,
    PyObject* name) {
  if (entry->name == name &&
      entry->globals_version == globals->ma_version_tag &&
      entry->builtins_version == builtins->ma_version_tag) {
    if (ci_global_cache_counting_311)
      ci_global_cache_hits_311++;
    return entry->value;
  }
  return NULL;
}

static inline PyObject* Ci_TryEarlyGlobal311(
    PyDictObject* globals,
    PyDictObject* builtins,
    PyObject* name,
    _Py_CODEUNIT* site) {
  size_t index;
  if (ci_global_cache_route_311 >= 2) {
    // ADAPTIVE does not interpret the index word. Stock specialization writes
    // it before switching to MODULE/BUILTIN. Any stale/foreign index can only
    // select a candidate bucket: full dict identities, owned name and BOTH
    // 64-bit content-version guards still decide whether a value may be used.
    _PyLoadGlobalCache* inline_cache = (_PyLoadGlobalCache*)(site + 1);
    index = inline_cache->index & (CI_GLOBAL_CACHE_CAPACITY_311 - 1);
  } else {
    index = Ci_GlobalCacheHash311(globals, name);
  }
  PyObject* value = ci_global_cache_route_311 == 4
      ? Ci_CompactGlobalHit311(
            &ci_compact_global_311[index], globals, builtins, name)
      : Ci_GlobalCacheHit311(
            &ci_global_cache_311[index], globals, builtins, name);
  if (ci_global_cache_counting_311) {
    if (value != NULL)
      ci_global_cache_early_hits_311++;
    else
      ci_global_cache_early_misses_311++;
  }
  return value;
}

static inline PyObject* Ci_CachedLoadGlobalAt311(
    PyDictObject* globals,
    PyDictObject* builtins,
    PyObject* name,
    _Py_CODEUNIT* site) {
  if (!ci_global_cache_enabled_311) {
    return _PyDict_LoadGlobal(globals, builtins, name);
  }
  size_t index = Ci_GlobalCacheHash311(globals, name);
  // Do not overwrite any cache word belonging to a specialized opcode.
  if (ci_global_cache_route_311 >= 2 &&
      _Py_OPCODE(*site) == LOAD_GLOBAL_ADAPTIVE) {
    ((_PyLoadGlobalCache*)(site + 1))->index = (uint16_t)index;
  }
  CiGlobalCacheEntry311* entry = &ci_global_cache_311[index];
  CiCompactGlobalEntry311* compact = &ci_compact_global_311[index];
  PyObject* value = ci_global_cache_route_311 == 4
      ? Ci_CompactGlobalHit311(compact, globals, builtins, name)
      : Ci_GlobalCacheHit311(entry, globals, builtins, name);
  if (value != NULL)
    return value;
  uint64_t gv = globals->ma_version_tag;
  uint64_t bv = builtins->ma_version_tag;
  if (!PyUnicode_CheckExact(name) || globals->ma_values != NULL ||
      builtins->ma_values != NULL || !DK_IS_UNICODE(globals->ma_keys) ||
      !DK_IS_UNICODE(builtins->ma_keys) || gv == 0 || bv == 0) {
    if (ci_global_cache_counting_311)
      ci_global_cache_fallbacks_311++;
    return _PyDict_LoadGlobal(globals, builtins, name);
  }
  if (ci_global_cache_counting_311)
    ci_global_cache_misses_311++;
  value = _PyDict_LoadGlobal(globals, builtins, name);
  if (value == NULL || globals->ma_version_tag != gv ||
      builtins->ma_version_tag != bv)
    return value;
  if (ci_global_cache_route_311 == 4) {
    PyObject* previous_name = compact->name;
    Py_INCREF(name);
    compact->globals_version = gv;
    compact->builtins_version = bv;
    compact->name = name;
    compact->value = value;
    Py_XDECREF(previous_name);
    return value;
  }
  PyObject* previous_name = entry->name;
  Py_INCREF(name);
  entry->globals = globals;
  entry->builtins = builtins;
  entry->name = name;
  entry->value = value;
  entry->globals_version = gv;
  entry->builtins_version = bv;
  Py_XDECREF(previous_name);
  return value;
}
