// Copyright (c) Meta Platforms, Inc. and affiliates.

// Eval/Observe and shadow runtime modes for CPython 3.11.
//
// The frame entry counts executions per code object and fires one scheduling
// request when a code object crosses the hot threshold.  Observe terminates
// at the capability gate; shadow passes the real function through the
// discard-only compile pipeline.  Neither mode changes Python execution.
//
// Counters live in a private table, deliberately not in co_extra: code
// deallocation runs every registered co_extra free function for every slot
// a code object carries, so a widely-used slot of ours would multiply how
// often other tenants' callbacks fire -- including ctypes callbacks into
// Python during interpreter shutdown (test_code does exactly this).  The
// private table touches nothing of the code object itself.

#include "cinderx/Interpreter/3.11/observe.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int Ci_Observe311_Enabled;

static int ci_observe_configured;
static uint64_t ci_observe_threshold;
static FILE* ci_observe_file;
// Compilation is synchronous under the 3.11 GIL.  Keep all frames entered by
// the compiler itself out of observation and protect the single JIT context
// from recursive compilation.
static int ci_observe_compiling;

static uint64_t ci_observe_codes_seen;
static uint64_t ci_observe_events_dropped;

// One slot per code object ever observed.  The key is the code's address,
// valid only while the entry's weakref still resolves to that address: a
// dead or reused address fails the check and the slot is recycled, so no
// bare address ever acts as a long-lived key.  All access runs under the
// GIL.
typedef struct {
  uintptr_t key; // 0 = empty
  uint64_t count;
  int dispatched;
  PyObject* weakref; // no callback; used purely to validate liveness
} Ci_ObserveSlot;

static Ci_ObserveSlot* ci_observe_table;
static size_t ci_observe_capacity; // power of two, 0 until first insert
static size_t ci_observe_live;

enum {
  CI_OBSERVE_INITIAL_EVENTS = 1024,
  CI_OBSERVE_MAX_EVENTS = 65536,
};

typedef struct {
  PyObject* qualname; // owned; NULL when the code object carries none
  PyObject* filename; // owned; NULL when the code object carries none
  uint64_t count;
  const char* result; // static string from the compile entry point
} Ci_ObserveEvent;

static Ci_ObserveEvent* ci_observe_events;
static Py_ssize_t ci_observe_event_count;
static size_t ci_observe_event_capacity;

int Ci_Observe311_Configure(void) {
  if (ci_observe_configured) {
    return 0;
  }

  const char* mode = getenv("CINDERX_JIT_MODE");
  if (mode == NULL || *mode == '\0' || strcmp(mode, "off") == 0) {
    ci_observe_configured = 1;
    Ci_Observe311_Enabled = 0;
    return 0;
  }
  if (strcmp(mode, "observe") != 0 && strcmp(mode, "shadow") != 0 &&
      strcmp(mode, "canary") != 0) {
    // canary is the MR-04 test-only execution mode: eligible simple
    // functions compile, install and execute machine code.  The product
    // auto-JIT stays unavailable on 3.11.
    PyErr_Format(
        PyExc_RuntimeError,
        "CINDERX_JIT_MODE=%s is not accepted on CPython 3.11: this build "
        "takes \"off\", \"observe\", \"shadow\" or \"canary\"",
        mode);
    return -1;
  }

  // The hot threshold reuses the auto-JIT knob rather than growing a new
  // one.  Observe is explicit, so only a plain positive count is accepted.
  uint64_t threshold = 50;
  const char* raw_threshold = getenv("PYTHONJITAUTO");
  if (raw_threshold != NULL && *raw_threshold != '\0') {
    char* end = NULL;
    unsigned long long parsed = strtoull(raw_threshold, &end, 10);
    if (end == raw_threshold || *end != '\0' || parsed == 0) {
      PyErr_Format(
          PyExc_RuntimeError,
          "PYTHONJITAUTO=%s is not a usable observe threshold: expected a "
          "positive integer",
          raw_threshold);
      return -1;
    }
    threshold = (uint64_t)parsed;
  }

  FILE* file = NULL;
  const char* path = getenv("CINDERX_JIT_OBSERVE_FILE");
  if (path != NULL && *path != '\0') {
    file = fopen(path, "a");
    if (file == NULL) {
      PyErr_Format(
          PyExc_RuntimeError,
          "could not open CINDERX_JIT_OBSERVE_FILE=%s for appending",
          path);
      return -1;
    }
  }

  ci_observe_threshold = threshold;
  ci_observe_file = file;
  ci_observe_configured = 1;
  Ci_Observe311_Enabled = 1;
  return 0;
}

static size_t slot_index(uintptr_t key, size_t capacity) {
  // Fibonacci scatter over the aligned pointer bits.
  return ((key >> 4) * (uintptr_t)11400714819323198485ULL) & (capacity - 1);
}

static int slot_is_live(Ci_ObserveSlot* slot) {
  return slot->key != 0 &&
      (uintptr_t)PyWeakref_GET_OBJECT(slot->weakref) == slot->key;
}

static void slot_clear(Ci_ObserveSlot* slot) {
  Py_CLEAR(slot->weakref);
  memset(slot, 0, sizeof(*slot));
}

// A liveness handle for one code object.  Freshly created, exclusively-ours
// weakrefs are taken out of the GC's object census: observation must stay
// invisible to gc.get_objects() (test_descr literally counts it).  A basic
// weakref holds no strong references and cannot sit on a cycle, and clearing
// at referent death walks the referent's weakref list, not the GC -- so
// untracking costs nothing.  A shared handle (the referent already had a
// plain weakref, or someone made one after ours) stays tracked, because it
// is not ours to hide.
static PyObject* observe_new_weakref(PyCodeObject* code) {
  PyObject* weakref = PyWeakref_NewRef((PyObject*)code, NULL);
  if (weakref == NULL) {
    PyErr_Clear();
    return NULL;
  }
  if (Py_REFCNT(weakref) == 1) {
    PyObject_GC_UnTrack(weakref);
  }
  return weakref;
}

// Grow (or create) the table, dropping entries whose code died: the sweep
// keeps the table bounded by the set of live observed code objects.
static int observe_table_grow(void) {
  size_t capacity = ci_observe_capacity ? ci_observe_capacity * 2 : 1024;
  Ci_ObserveSlot* table = calloc(capacity, sizeof(Ci_ObserveSlot));
  if (table == NULL) {
    return -1;
  }

  size_t live = 0;
  for (size_t i = 0; i < ci_observe_capacity; i++) {
    Ci_ObserveSlot* slot = &ci_observe_table[i];
    if (slot->key == 0) {
      continue;
    }
    if (!slot_is_live(slot)) {
      slot_clear(slot);
      continue;
    }
    size_t probe = slot_index(slot->key, capacity);
    while (table[probe].key != 0) {
      probe = (probe + 1) & (capacity - 1);
    }
    table[probe] = *slot;
    live++;
  }

  free(ci_observe_table);
  ci_observe_table = table;
  ci_observe_capacity = capacity;
  ci_observe_live = live;
  return 0;
}

// Find the slot for this code object, recycling a slot whose previous
// tenant died and reusing its address.  Returns NULL when the table cannot
// hold the entry (allocation failure); observation is then skipped.
static Ci_ObserveSlot* observe_slot_for(PyCodeObject* code) {
  uintptr_t key = (uintptr_t)code;
  if (ci_observe_capacity == 0 ||
      ci_observe_live * 4 >= ci_observe_capacity * 3) {
    if (observe_table_grow() < 0) {
      return NULL;
    }
  }

  size_t probe = slot_index(key, ci_observe_capacity);
  while (1) {
    Ci_ObserveSlot* slot = &ci_observe_table[probe];
    if (slot->key == key) {
      if (slot_is_live(slot)) {
        return slot;
      }
      // Same address, different lifetime: the old tenant died and the
      // address was reused, so the state starts over in place.  The key
      // stays, which keeps every probe chain intact; entries whose address
      // never comes back are swept when the table grows.
      PyObject* weakref = observe_new_weakref(code);
      if (weakref == NULL) {
        return NULL;
      }
      Py_CLEAR(slot->weakref);
      slot->weakref = weakref;
      slot->count = 0;
      slot->dispatched = 0;
      ci_observe_codes_seen++;
      return slot;
    }
    if (slot->key == 0) {
      PyObject* weakref = observe_new_weakref(code);
      if (weakref == NULL) {
        return NULL;
      }
      slot->key = key;
      slot->count = 0;
      slot->dispatched = 0;
      slot->weakref = weakref;
      ci_observe_live++;
      ci_observe_codes_seen++;
      return slot;
    }
    probe = (probe + 1) & (ci_observe_capacity - 1);
  }
}

static int observe_events_grow(void) {
  if (ci_observe_event_capacity >= CI_OBSERVE_MAX_EVENTS) {
    return -1;
  }
  size_t capacity = ci_observe_event_capacity == 0
      ? CI_OBSERVE_INITIAL_EVENTS
      : ci_observe_event_capacity * 2;
  if (capacity > CI_OBSERVE_MAX_EVENTS) {
    capacity = CI_OBSERVE_MAX_EVENTS;
  }
  Ci_ObserveEvent* events =
      realloc(ci_observe_events, capacity * sizeof(Ci_ObserveEvent));
  if (events == NULL) {
    return -1;
  }
  ci_observe_events = events;
  ci_observe_event_capacity = capacity;
  return 0;
}

// Record the event and walk it into the compile entry point.  Only the
// function's identity, its count and the scheduling result are recorded --
// never arguments, return values or any other program data.
static void
observe_emit(PyFunctionObject* func, PyCodeObject* code, uint64_t count) {
  const char* result = Ci_JitShell311_RequestCompile(func);
  // The observer only runs when no exception is active.  Compilation is
  // diagnostic and may fail, but that failure must not leak into evaluation.
  PyErr_Clear();

  PyObject* qualname = code->co_qualname;
  PyObject* filename = code->co_filename;
  if ((size_t)ci_observe_event_count < ci_observe_event_capacity ||
      observe_events_grow() == 0) {
    Ci_ObserveEvent* event = &ci_observe_events[ci_observe_event_count++];
    Py_XINCREF(qualname);
    Py_XINCREF(filename);
    event->qualname = qualname;
    event->filename = filename;
    event->count = count;
    event->result = result;
  } else {
    ci_observe_events_dropped++;
  }

  if (ci_observe_file != NULL) {
    const char* name = NULL;
    if (qualname != NULL) {
      name = PyUnicode_AsUTF8(qualname);
      if (name == NULL) {
        PyErr_Clear();
      }
    }
    fprintf(
        ci_observe_file,
        "%s %" PRIu64 " %s\n",
        name != NULL ? name : "<unknown>",
        count,
        result);
    fflush(ci_observe_file);
  }
}

void Ci_Observe311_OnFrame(PyFunctionObject* func) {
  if (!Ci_Observe311_Enabled || func == NULL || ci_observe_compiling) {
    return;
  }
  // Observation stops where normal execution stops: nothing is counted
  // while the interpreter finalizes, and a frame entered with an exception
  // in flight (generator throw) is skipped so the C-API calls below never
  // run under, or disturb, an in-flight exception.  The lost counts are
  // bounded and predictable.
  if (_Py_IsFinalizing() || PyErr_Occurred() != NULL) {
    return;
  }

  PyCodeObject* code = (PyCodeObject*)func->func_code;
  Ci_ObserveSlot* slot = observe_slot_for(code);
  if (slot == NULL || slot->dispatched) {
    return;
  }
  slot->count++;

  if (slot->count >= ci_observe_threshold) {
    // Mark before dispatching, so nothing the compile entry point ever does
    // can re-enter into a second event for this code object.
    slot->dispatched = 1;
    ci_observe_compiling = 1;
    observe_emit(func, code, slot->count);
    ci_observe_compiling = 0;
  }
}

static int stats_set_uint(PyObject* dict, const char* key, uint64_t value) {
  PyObject* number = PyLong_FromUnsignedLongLong(value);
  if (number == NULL) {
    return -1;
  }
  int rc = PyDict_SetItemString(dict, key, number);
  Py_DECREF(number);
  return rc;
}

static int stats_set_str(PyObject* dict, const char* key, const char* value) {
  PyObject* text = PyUnicode_FromString(value);
  if (text == NULL) {
    return -1;
  }
  int rc = PyDict_SetItemString(dict, key, text);
  Py_DECREF(text);
  return rc;
}

PyObject* Ci_Observe311_Stats(void) {
  PyObject* events = PyList_New(0);
  if (events == NULL) {
    return NULL;
  }
  for (Py_ssize_t i = 0; i < ci_observe_event_count; i++) {
    Ci_ObserveEvent* event = &ci_observe_events[i];
    PyObject* entry = PyDict_New();
    if (entry == NULL) {
      goto error;
    }
    int rc = PyDict_SetItemString(
        entry, "qualname", event->qualname != NULL ? event->qualname : Py_None);
    if (rc < 0 ||
        PyDict_SetItemString(
            entry,
            "filename",
            event->filename != NULL ? event->filename : Py_None) < 0 ||
        stats_set_uint(entry, "count", event->count) < 0 ||
        stats_set_str(entry, "result", event->result) < 0 ||
        PyList_Append(events, entry) < 0) {
      Py_DECREF(entry);
      goto error;
    }
    Py_DECREF(entry);
  }

  PyObject* stats = PyDict_New();
  if (stats == NULL) {
    goto error;
  }
  if (PyDict_SetItemString(
          stats, "enabled", Ci_Observe311_Enabled ? Py_True : Py_False) < 0 ||
      stats_set_uint(stats, "threshold", ci_observe_threshold) < 0 ||
      stats_set_uint(stats, "codes_seen", ci_observe_codes_seen) < 0 ||
      stats_set_uint(stats, "events_dropped", ci_observe_events_dropped) < 0 ||
      PyDict_SetItemString(stats, "events", events) < 0) {
    Py_DECREF(stats);
    goto error;
  }
  Py_DECREF(events);
  return stats;

error:
  Py_DECREF(events);
  return NULL;
}

void Ci_Observe311_Finalize(void) {
  // Disable the hot path before dropping anything it can reach.
  Ci_Observe311_Enabled = 0;
  ci_observe_compiling = 0;

  for (size_t i = 0; i < ci_observe_capacity; i++) {
    slot_clear(&ci_observe_table[i]);
  }
  free(ci_observe_table);
  ci_observe_table = NULL;
  ci_observe_capacity = 0;
  ci_observe_live = 0;

  for (Py_ssize_t i = 0; i < ci_observe_event_count; i++) {
    Py_CLEAR(ci_observe_events[i].qualname);
    Py_CLEAR(ci_observe_events[i].filename);
  }
  free(ci_observe_events);
  ci_observe_events = NULL;
  ci_observe_event_count = 0;
  ci_observe_event_capacity = 0;

  if (ci_observe_file != NULL) {
    fclose(ci_observe_file);
    ci_observe_file = NULL;
  }
  ci_observe_configured = 0;
  ci_observe_threshold = 0;
  ci_observe_codes_seen = 0;
  ci_observe_events_dropped = 0;
}
