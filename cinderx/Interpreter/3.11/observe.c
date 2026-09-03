// Copyright (c) Meta Platforms, Inc. and affiliates.

// Eval/Observe, shadow and execute runtime modes for CPython 3.11.
//
// The frame entry counts executions per code object and fires one scheduling
// request when a code object crosses the hot threshold.  Observe terminates
// at the capability gate; shadow passes the real function through the
// discard-only compile pipeline; execute compiles, installs and runs
// machine code.  Counting never changes what a frame computes.
//
// Counters live in a private table keyed by code address.  The table
// learns about key death through the code-extra free function -- 3.11 has
// no code watcher, and a weak reference would be visible to the observed
// program via weakref.getweakrefs().  Watching a code object therefore
// creates the co_extra block whose freefunc reports its death, which is
// what forces the walk past foreign slots below ours and why every mode
// above `off` refuses unless CinderX holds slot zero.
//
// Accepted deviation: CPython counts co_extra in code.__sizeof__(), so a
// code object that has run under any watching mode reports a larger size
// than under `off` (measured 200 -> 216 bytes for a small function).
// Only a code-object watcher, which 3.11 lacks, would remove this.

#include "cinderx/Interpreter/3.11/observe.h"

#include "cinderx/Common/code.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int Ci_Observe311_Enabled;

static int ci_observe_configured;
static Ci_JitMode311 ci_observe_mode;
static const char* ci_observe_spelling = "off";
// Non-zero in execute mode: dispatch installs machine code, and frames of
// an already-dispatched code object are offered for fresh attachment.
static int ci_observe_execute;
static uint64_t ci_observe_threshold;
static const char* ci_observe_threshold_source = "unconfigured";
static int ci_shared_autojit_set;
static int ci_shared_autojit_configured;
static int ci_shared_autojit_valid;
static int ci_shared_autojit_classify;
static uint64_t ci_shared_autojit_threshold;
static FILE* ci_observe_file;
// Compilation is synchronous under the 3.11 GIL.  Keep all frames entered by
// the compiler itself out of observation and protect the single JIT context
// from recursive compilation.
static int ci_observe_compiling;

static uint64_t ci_observe_codes_seen;
static uint64_t ci_observe_events_dropped;
static uint64_t ci_observe_fresh_attachments;
static uint64_t ci_observe_auto_jit_disabled_codes;
static uint64_t ci_observe_late_deferrals;
static uint64_t ci_observe_post_publication_interpreted_frames;

// One slot per code object ever observed.  The key is the code's address,
// which is only ever trusted together with `dead`: the code-extra free
// function reports the death of every code object this table watches, and
// a slot marked dead is recycled from scratch when its address comes back,
// so no bare address ever acts as a long-lived key.  All access runs under
// the GIL.
typedef struct {
  uintptr_t key; // 0 = empty
  uint64_t count;
  int dispatched;
  // Execute mode: the dispatch installed machine code, so later frames of
  // this code object (fresh function objects) are offered for attachment.
  // Cleared once the attach entry point answers "never again".
  int attachable;
  // Sticky after a successful publication. Any later interpreter frame for
  // this code object is evidence that publication did have a re-entry
  // opportunity, even if the artifact was retired in the meantime.
  int published;
  uint64_t post_publication_interpreted_frames;
  Py_ssize_t event_index; // -1 when the bounded event ledger dropped it
  // The keyed code object has been destroyed.  The key stays so probe
  // chains through this slot survive; the state does not.
  int dead;
} Ci_ObserveSlot;

static Ci_ObserveSlot* ci_observe_table;
static size_t ci_observe_capacity; // power of two, 0 until first insert
// Slots carrying a key, dead ones included.  This is what bounds the load
// factor: a dead slot keeps its key so the probe chains through it stay
// intact, so it still occupies the table and the open-addressed lookup
// would spin forever if the count stopped including it.
static size_t ci_observe_live;
// Slots keyed to a code object that is still alive.  A gauge, not a load
// factor: it comes back down as code objects die, which is what makes it
// usable as a lifecycle invariant.
static size_t ci_observe_watched;

enum {
  CI_OBSERVE_INITIAL_EVENTS = 1024,
  CI_OBSERVE_MAX_EVENTS = 65536,
};

// One recorded scheduling decision.
//
// The two names are COPIES, not references to the code object's strings.
// Holding the originals would keep them alive for the life of the process
// -- the ledger is only cleared at finalization -- and
// sys.getrefcount(code.co_filename) would read one higher under
// observation than it does without.  A diagnostic must not be visible in
// the reference counts of the program it is observing.
typedef struct {
  char* qualname; // owned malloc'd UTF-8; NULL when unavailable
  char* filename; // owned malloc'd UTF-8; NULL when unavailable
  uint64_t count;
  const char* result; // static string from the compile entry point
  uint64_t post_publication_interpreted_frames;
} Ci_ObserveEvent;

static Ci_ObserveEvent* ci_observe_events;
static Py_ssize_t ci_observe_event_count;
static size_t ci_observe_event_capacity;

// The product's "no machine code" switches, spelled the way the startup
// module (_cinderx_auto.py) reads them.
static int env_flag_enabled(const char* name) {
  const char* value = getenv(name);
  if (value == NULL) {
    return 0;
  }
  return strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
      strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0;
}

void Ci_Observe311_SetResolvedAutoJitConfig(
    int configured,
    uint64_t threshold,
    int auto_classify,
    int valid) {
  ci_shared_autojit_set = 1;
  ci_shared_autojit_configured = configured != 0;
  ci_shared_autojit_threshold = threshold;
  ci_shared_autojit_classify = auto_classify != 0;
  ci_shared_autojit_valid = valid != 0;
}

static int parse_autojit_threshold(const char* raw, uint64_t* threshold) {
  if (strcmp(raw, "auto") == 0 || strncmp(raw, "auto:", 5) == 0) {
    PyErr_Format(
        PyExc_RuntimeError,
        "PYTHONJITAUTO=%s is not usable on CPython 3.11: expected a "
        "non-negative integer threshold; "
        "classification is not supported",
        raw);
    return -1;
  }
  const char* number = raw;
  int digits_only = *number != '\0';
  for (const char* cursor = number; *cursor != '\0'; cursor++) {
    if (*cursor < '0' || *cursor > '9') {
      digits_only = 0;
      break;
    }
  }
  errno = 0;
  char* end = NULL;
  unsigned long long parsed = strtoull(number, &end, 10);
  if (!digits_only || end == number || *end != '\0' || errno == ERANGE ||
      parsed > UINT32_MAX) {
    PyErr_Format(
        PyExc_RuntimeError,
        "PYTHONJITAUTO=%s is not usable on CPython 3.11: expected a "
        "non-negative integer threshold",
        raw);
    return -1;
  }
  *threshold = (uint64_t)parsed;
  return 0;
}

static int resolve_autojit_threshold_from_env(uint64_t* threshold) {
  *threshold = 50;
  ci_shared_autojit_classify = 0;
  const char* raw_all = getenv("PYTHONJITALL");
  if (raw_all != NULL && *raw_all != '\0') {
    errno = 0;
    char* end = NULL;
    (void)strtol(raw_all, &end, 10);
    if (end != raw_all && *end == '\0' && errno != ERANGE) {
      *threshold = 0;
    }
  }
  const char* raw_auto = getenv("PYTHONJITAUTO");
  if (raw_auto != NULL && *raw_auto != '\0' &&
      parse_autojit_threshold(raw_auto, threshold) < 0) {
    return -1;
  }
  return 0;
}

int Ci_Observe311_ResolveMode(Ci_JitMode311* mode, const char** spelling) {
  const char* raw = getenv("CINDERX_JIT_MODE");
  if (raw == NULL || *raw == '\0') {
    raw = "off";
  }
  // The spelling handed back is a static string, never the environment
  // block itself: a later putenv() may move or free that.
  static const char* const spellings[] = {
      "off", "observe", "shadow", "execute", "canary"};
  const char* known = NULL;
  for (size_t i = 0; i < sizeof(spellings) / sizeof(spellings[0]); i++) {
    if (strcmp(raw, spellings[i]) == 0) {
      known = spellings[i];
      break;
    }
  }
  if (spelling != NULL) {
    *spelling = known != NULL ? known : "unknown";
  }
  if (known == NULL) {
    PyErr_Format(
        PyExc_RuntimeError,
        "CINDERX_JIT_MODE=%s is not accepted on CPython 3.11: this build "
        "takes \"off\", \"observe\", \"shadow\", \"execute\" or \"canary\"",
        raw);
    return -1;
  }
  if (strcmp(known, "off") == 0) {
    *mode = CI_JIT_MODE_311_OFF;
    return 0;
  }
  if (strcmp(known, "observe") == 0) {
    *mode = CI_JIT_MODE_311_OBSERVE;
    return 0;
  }
  if (strcmp(known, "shadow") == 0) {
    *mode = CI_JIT_MODE_311_SHADOW;
    return 0;
  }
  {
    // execute is the product auto-JIT; canary is its test-only spelling
    // (the MR-04 execution mode), kept so the earlier gate legs keep their
    // configuration.  Both resolve to the same machinery and policy.
    // The disable switches outrank the mode selector: a process that asks
    // for no machine code gets none, whatever the mode says.
    if (env_flag_enabled("PYTHONJITDISABLE") ||
        env_flag_enabled("CINDERX_JIT_DISABLE")) {
      *mode = CI_JIT_MODE_311_OFF;
      return 0;
    }
    *mode = CI_JIT_MODE_311_EXECUTE;
    return 0;
  }
}

Ci_JitMode311 Ci_Observe311_Mode(void) {
  return ci_observe_mode;
}

int Ci_Observe311_Configure(void) {
  if (ci_observe_configured) {
    return 0;
  }

  Ci_JitMode311 mode;
  const char* spelling;
  if (Ci_Observe311_ResolveMode(&mode, &spelling) < 0) {
    return -1;
  }
  if (mode == CI_JIT_MODE_311_OFF) {
    ci_observe_configured = 1;
    ci_observe_mode = mode;
    ci_observe_spelling = spelling;
    ci_observe_execute = 0;
    Ci_Observe311_Enabled = 0;
    return 0;
  }

  // code_dealloc walks co_extra slots 0..ce_size calling every freefunc,
  // foreign slots below ours included; a mortal foreign freefunc can be
  // dead by shutdown and the walk jumps through a freed trampoline.
  // Nothing protects the slots below ours, so every watching mode refuses
  // unless CinderX holds slot 0 (a normal startup always does).
  Py_ssize_t extra_slot = codeExtraSlotIndex();
  if (extra_slot != 0) {
    PyErr_Format(
        PyExc_RuntimeError,
        "CINDERX_JIT_MODE=%s requires the first code-extra slot, but "
        "CinderX holds slot %zd: another component registered before it "
        "loaded, and CPython 3.11 would run that component's free "
        "function for every code object this mode watches",
        spelling,
        extra_slot);
    return -1;
  }

  uint64_t threshold = 50;
  if (ci_shared_autojit_set) {
    if (!ci_shared_autojit_valid) {
      PyErr_SetString(
          PyExc_RuntimeError,
          "shared CPython 3.11 Auto-JIT threshold resolution failed");
      return -1;
    }
    threshold = ci_shared_autojit_configured ? ci_shared_autojit_threshold : 50;
    ci_observe_threshold_source = "shared-jit-config";
  } else {
    if (resolve_autojit_threshold_from_env(&threshold) < 0) {
      return -1;
    }
    ci_observe_threshold_source = "environment-fallback";
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
  ci_observe_mode = mode;
  ci_observe_spelling = spelling;
  ci_observe_execute = mode == CI_JIT_MODE_311_EXECUTE;
  Ci_Observe311_Enabled = 1;
  return 0;
}

static size_t slot_index(uintptr_t key, size_t capacity) {
  // Fibonacci scatter over the aligned pointer bits.
  return ((key >> 4) * (uintptr_t)11400714819323198485ULL) & (capacity - 1);
}

static void slot_clear(Ci_ObserveSlot* slot) {
  memset(slot, 0, sizeof(*slot));
}

// A code object this table has an entry for is being destroyed.  Called
// from code_dealloc; the argument is a key, never dereferenced, and
// nothing here allocates, grows or frees -- the table shape cannot change
// under a frame that is mid-lookup.
void Ci_Observe311_OnCodeDeath(PyCodeObject* code) {
  if (ci_observe_capacity == 0) {
    return;
  }
  uintptr_t key = (uintptr_t)code;
  size_t probe = slot_index(key, ci_observe_capacity);
  for (size_t steps = 0; steps < ci_observe_capacity; steps++) {
    Ci_ObserveSlot* slot = &ci_observe_table[probe];
    if (slot->key == key) {
      if (slot->dead) {
        // A tombstone for an earlier tenant of this address.  Every
        // CinderX code-extra block reports its death here, not just the
        // ones this table asked for, so a code object that never entered
        // the observer -- one force_compile() gave a block to -- can be
        // allocated at a dead slot's address and land on it.  Retiring an
        // entry that is already retired would take a second decrement out
        // of the live population, which is a gauge the rehash and the
        // lifecycle census both read as truth.
        return;
      }
      // The key stays: clearing it would cut every probe chain running
      // through this slot.  A keyed slot with no state is recycled in
      // place by the next tenant and swept when the table grows.
      slot->count = 0;
      slot->dispatched = 0;
      slot->attachable = 0;
      slot->published = 0;
      slot->post_publication_interpreted_frames = 0;
      slot->event_index = -1;
      slot->dead = 1;
      if (ci_observe_watched > 0) {
        ci_observe_watched--;
      }
      return;
    }
    if (slot->key == 0) {
      return;
    }
    probe = (probe + 1) & (ci_observe_capacity - 1);
  }
}

int Ci_Observe311_GetCodeState(
    PyCodeObject* code,
    uint64_t* count,
    int* dispatched,
    int* attachable) {
  if (count != NULL) {
    *count = 0;
  }
  if (dispatched != NULL) {
    *dispatched = 0;
  }
  if (attachable != NULL) {
    *attachable = 0;
  }
  if (code == NULL || ci_observe_capacity == 0) {
    return 0;
  }
  uintptr_t key = (uintptr_t)code;
  size_t probe = slot_index(key, ci_observe_capacity);
  for (size_t steps = 0; steps < ci_observe_capacity; steps++) {
    Ci_ObserveSlot* slot = &ci_observe_table[probe];
    if (slot->key == key) {
      if (slot->dead) {
        return 0;
      }
      if (count != NULL) {
        *count = slot->count;
      }
      if (dispatched != NULL) {
        *dispatched = slot->dispatched;
      }
      if (attachable != NULL) {
        *attachable = slot->attachable;
      }
      return 1;
    }
    if (slot->key == 0) {
      return 0;
    }
    probe = (probe + 1) & (ci_observe_capacity - 1);
  }
  return 0;
}

// Guarantee that this code object's death will be reported: the co_extra
// block must exist before the table entry does.  It allocates from the
// raw allocator, not the GC's, so the table path cannot trigger a
// collection mid-lookup.
static int observe_arm_death_notice(PyCodeObject* code) {
  return codeExtra(code) != NULL ? 0 : -1;
}

// Rehash, dropping entries whose code died.  Capacity is chosen from
// what is still WATCHED, not what is keyed: dead slots keep their keys
// (tombstones), so doubling on the load factor would ratchet an octave
// per churn cycle while the live population stays flat.  Grow only past
// half, or a compaction at 74% would rehash again within a few inserts.
static int observe_table_grow(void) {
  size_t capacity = ci_observe_capacity ? ci_observe_capacity : 1024;
  if (ci_observe_capacity != 0 &&
      ci_observe_watched * 2 >= ci_observe_capacity) {
    capacity = ci_observe_capacity * 2;
  }
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
    if (slot->dead) {
      // Notified dead and never reused: this is where such an entry
      // finally leaves the table.
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
  // The sweep drops dead slots, so after it the two counts agree again.
  ci_observe_live = live;
  ci_observe_watched = live;
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
      if (!slot->dead) {
        return slot;
      }
      // Same address, different lifetime: the old tenant was notified
      // dead and the address was reused, so the state starts over in
      // place.  The key stays, which keeps every probe chain intact;
      // entries whose address never comes back are swept when the table
      // grows.
      if (observe_arm_death_notice(code) < 0) {
        return NULL;
      }
      slot->count = 0;
      slot->dispatched = 0;
      slot->attachable = 0;
      slot->published = 0;
      slot->post_publication_interpreted_frames = 0;
      slot->event_index = -1;
      slot->dead = 0;
      ci_observe_watched++;
      ci_observe_codes_seen++;
      return slot;
    }
    if (slot->key == 0) {
      if (observe_arm_death_notice(code) < 0) {
        return NULL;
      }
      slot->key = key;
      slot->count = 0;
      slot->dispatched = 0;
      slot->attachable = 0;
      slot->published = 0;
      slot->post_publication_interpreted_frames = 0;
      slot->event_index = -1;
      slot->dead = 0;
      ci_observe_live++;
      ci_observe_watched++;
      ci_observe_codes_seen++;
      return slot;
    }
    probe = (probe + 1) & (ci_observe_capacity - 1);
  }
}

// A private UTF-8 copy of a Python string, or NULL.
//
// NULL covers both "the code object carries no such name" and "the name
// has no UTF-8 form" -- a lone surrogate in a qualname, which this
// runtime already refuses to compile for.  Neither is worth failing a
// diagnostic record over, and the error must not escape into the
// interpreted call that is merely passing through.
static char* observe_copy_utf8(PyObject* text) {
  if (text == NULL || !PyUnicode_Check(text)) {
    return NULL;
  }
  Py_ssize_t size = 0;
  const char* utf8 = PyUnicode_AsUTF8AndSize(text, &size);
  if (utf8 == NULL) {
    PyErr_Clear();
    return NULL;
  }
  char* copy = malloc((size_t)size + 1);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, utf8, (size_t)size);
  copy[size] = '\0';
  return copy;
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
// never arguments, return values or any other program data.  Returns the
// scheduling result.
static void observe_on_frame_locked(
    PyFunctionObject* func,
    PyCodeObject* code,
    struct _PyInterpreterFrame* frame);

static const char* observe_emit(
    PyFunctionObject* func,
    PyCodeObject* code,
    uint64_t count,
    Py_ssize_t* event_index) {
  *event_index = -1;
  const char* result = Ci_JitShell311_RequestCompile(func, code);
  // The observer only runs when no exception is active.  Compilation is
  // diagnostic and may fail, but that failure must not leak into evaluation.
  PyErr_Clear();

  PyObject* qualname = code->co_qualname;
  PyObject* filename = code->co_filename;
  if ((size_t)ci_observe_event_count < ci_observe_event_capacity ||
      observe_events_grow() == 0) {
    *event_index = ci_observe_event_count;
    Ci_ObserveEvent* event = &ci_observe_events[ci_observe_event_count++];
    event->qualname = observe_copy_utf8(qualname);
    event->filename = observe_copy_utf8(filename);
    event->count = count;
    event->result = result;
    event->post_publication_interpreted_frames = 0;
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
  return result;
}

void Ci_Observe311_OnFrame(
    PyFunctionObject* func,
    PyCodeObject* code,
    struct _PyInterpreterFrame* frame) {
  if (!Ci_Observe311_Enabled || func == NULL || code == NULL ||
      ci_observe_compiling) {
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

  // Raised for the WHOLE body, not just the dispatch.  The scheduling and
  // publication below run arbitrary Python, and a finalizer calls back
  // through this very hook; a re-entrant call that inserted enough code
  // objects would rehash -- and free -- the table this frame is holding a
  // slot pointer into.  (The table path itself no longer allocates from
  // the GC, so it cannot start a collection on its own, but the compile
  // it leads to certainly can.)  Frames entered while we are bookkeeping
  // simply are not counted, which is the same contract the compile
  // dispatch already had.
  ci_observe_compiling = 1;
  observe_on_frame_locked(func, code, frame);
  ci_observe_compiling = 0;
}

static void observe_on_frame_locked(
    PyFunctionObject* func,
    PyCodeObject* code,
    struct _PyInterpreterFrame* frame) {
  // Which code is running is the FRAME's answer, not the function's: a
  // suspended generator resumes the frame it already has after a
  // __code__ swap.  Counts and the single attempt belong to the code
  // that actually runs, and this door cannot publish for a function
  // whose code is not the one compiled -- skip outright.
  if (code != (PyCodeObject*)func->func_code) {
    return;
  }
  Ci_ObserveSlot* slot = observe_slot_for(code);
  if (slot == NULL) {
    return;
  }
  if (slot->dispatched) {
    // A dispatched code object reaching the interpreter again in execute
    // mode is either a function that fell back (tracing, a paused JIT, a
    // foreign evaluator) or a fresh function object over compiled code.
    // Only the latter has anything to gain, and only while the dispatch
    // installed an artifact; the attach entry point tells the two apart
    // cheaply and says when to stop asking for this code object.
    if (slot->published) {
      slot->post_publication_interpreted_frames++;
      ci_observe_post_publication_interpreted_frames++;
      if (slot->event_index >= 0 &&
          slot->event_index < ci_observe_event_count) {
        ci_observe_events[slot->event_index]
            .post_publication_interpreted_frames =
            slot->post_publication_interpreted_frames;
      }
    }
    if (slot->attachable) {
      int attached = Ci_JitShell311_AttachFresh(func);
      // Attachment is scheduling, not evaluation: nothing it raised may
      // reach the frame that is about to run.
      PyErr_Clear();
      if (attached > 0) {
        ci_observe_fresh_attachments++;
      } else if (attached < 0) {
        slot->attachable = 0;
      }
      // A published artifact that has since disappeared is deliberately
      // NOT grounds for another attempt: the commonest way for one to
      // disappear is force_uncompile(), and rescheduling there would undo
      // an explicit decision a few hundred calls later.  A code object
      // whose outer function could not be identified therefore keeps the
      // instance that was compiled and interprets the rest -- the residual
      // limit of the outer walk, not a retry policy.
    }
    return;
  }
  slot->count++;

  if (slot->count >= ci_observe_threshold) {
    if (ci_observe_execute) {
      // A code object gets one automatic attempt.  Spending it on a
      // condition that describes the process rather than the code -- a
      // trace or profile function on this thread, a paused JIT, an
      // evaluator that is not ours -- would make a transient state
      // permanent, so the dispatch waits instead.  Counting continues, and
      // the first frame that finds the condition gone dispatches; a
      // function that only ever runs under one (a workload living entirely
      // inside a coverage tracer) stays interpreted, as it should.
      if (Ci_JitShell311_DispatchDeferred()) {
        return;
      }
      // The scheduling entry point checks the disable bit too; this is
      // the fast answer for a code object another door already judged.
      if (Ci_JitShell311_CodeAutoJitDisabled(code)) {
        return;
      }
    }
    // Mark before dispatching, so nothing the compile entry point ever does
    // can re-enter into a second event for this code object.
    slot->dispatched = 1;
    if (ci_observe_execute) {
      Ci_JitShell311_TrackOuterFromFrame(func, frame);
    }
    Py_ssize_t event_index = -1;
    const char* result = observe_emit(func, code, slot->count, &event_index);
    slot->event_index = event_index;
    if (ci_observe_execute) {
      if (strcmp(result, CI_JIT_RESULT_311_INSTALLED) == 0 ||
          (strcmp(result, CI_JIT_RESULT_311_PUBLISHED_ELSEWHERE) == 0 &&
           Ci_JitShell311_CodeHasArtifact(code))) {
        // Either this dispatch published the artifact, or it was refused
        // precisely because one is already there for another namespace.
        // The second is a fact about the function that reached the
        // threshold, not about the code, and it says nothing about
        // whether OTHER instances of this code may attach -- the ones
        // sharing the artifact's namespace still may.
        //
        // Narrow on purpose: offering attachment for every code object
        // that merely has an artifact changes which functions get their
        // own compile and which share one, and that moves deopt
        // behaviour across the whole import preamble.  This is a
        // correctness fix for one refusal, not a scheduling policy
        // change.
        slot->attachable = 1;
        slot->published = 1;
      } else if (strcmp(result, CI_JIT_RESULT_311_DEFERRED) == 0) {
        // The attempt was withheld rather than made: everything the
        // scheduler did is undone, and the next frame that finds the JIT
        // usable dispatches this code object again.
        slot->dispatched = 0;
        ci_observe_late_deferrals++;
      } else {
        // One attempt per code object, and it has now been made: the
        // verdict is recorded on the code object itself so every
        // scheduling door -- this dispatch, fresh attachment, the compile
        // entry point -- reads the same answer.  Conditions that say
        // nothing about the code (tracing, a paused JIT, an evaluator that
        // is not ours) never get this far: DispatchDeferred() holds the
        // attempt back before it is spent.
        ci_observe_auto_jit_disabled_codes++;
      }
    }
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

// A recorded name, or None when the record has none.
static int
stats_set_str_or_none(PyObject* dict, const char* key, const char* value) {
  if (value == NULL) {
    return PyDict_SetItemString(dict, key, Py_None);
  }
  PyObject* text = PyUnicode_FromString(value);
  if (text == NULL) {
    return -1;
  }
  int rc = PyDict_SetItemString(dict, key, text);
  Py_DECREF(text);
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

void Ci_Observe311_GetLifecycleState(
    uint64_t* watched_codes,
    uint64_t* keyed_slots,
    uint64_t* table_capacity,
    uint64_t* events,
    uint64_t* post_publication_interpreted_frames) {
  *watched_codes = ci_observe_watched;
  *keyed_slots = ci_observe_live;
  *table_capacity = ci_observe_capacity;
  *events = (uint64_t)ci_observe_event_count;
  *post_publication_interpreted_frames =
      ci_observe_post_publication_interpreted_frames;
}

PyObject* Ci_Observe311_Stats(void) {
  // Reporting is bookkeeping, and it allocates: every dict, string and
  // list append below can collect, a collection runs finalizers, a
  // finalizer runs Python, and a Python frame reaches the observer --
  // which appends to the very array this walks.  Appending can realloc()
  // it, and the element pointer this loop is holding is then freed
  // memory.  (Under ASAN: heap-use-after-free, freed by
  // observe_events_grow, read here.)
  //
  // The frame hook excludes itself for exactly this reason and the same
  // flag serves here; frames entered while the observer is bookkeeping
  // are not counted, which is the contract the hook already documents.
  // The flag is saved and restored rather than cleared, because a report
  // requested from inside the hook must not lower a guard it did not
  // raise.
  int was_bookkeeping = ci_observe_compiling;
  ci_observe_compiling = 1;
  PyObject* events = PyList_New(0);
  if (events == NULL) {
    ci_observe_compiling = was_bookkeeping;
    return NULL;
  }
  for (Py_ssize_t i = 0; i < ci_observe_event_count; i++) {
    // Read the record out before allocating anything, and hold the two
    // strings by owning reference for as long as they are used.  The
    // exclusion above is what makes the array stable; taking the copy is
    // what makes that independent of it, so a future caller that reaches
    // this without the flag cannot reintroduce the same bug silently.
    // The record holds copies, so the strings handed out here are built
    // fresh: reporting cannot hand a caller a reference into anything the
    // observed program owns.
    Ci_ObserveEvent* event = &ci_observe_events[i];
    const char* qualname = event->qualname;
    const char* filename = event->filename;
    uint64_t count = event->count;
    const char* result = event->result;
    uint64_t post_publication_interpreted_frames =
        event->post_publication_interpreted_frames;

    PyObject* entry = PyDict_New();
    int rc = entry == NULL ? -1 : 0;
    if (rc == 0 &&
        (stats_set_str_or_none(entry, "qualname", qualname) < 0 ||
         stats_set_str_or_none(entry, "filename", filename) < 0 ||
         stats_set_uint(entry, "count", count) < 0 ||
         stats_set_str(entry, "result", result) < 0 ||
         stats_set_uint(
             entry,
             "post_publication_interpreted_frames",
             post_publication_interpreted_frames) < 0 ||
         PyList_Append(events, entry) < 0)) {
      rc = -1;
    }
    Py_XDECREF(entry);
    if (rc < 0) {
      goto error;
    }
  }

  PyObject* stats = PyDict_New();
  if (stats == NULL) {
    goto error;
  }
  static const char* const mode_names[] = {
      "off", "observe", "shadow", "execute"};
  if (PyDict_SetItemString(
          stats, "enabled", Ci_Observe311_Enabled ? Py_True : Py_False) < 0 ||
      stats_set_str(stats, "mode", mode_names[ci_observe_mode]) < 0 ||
      stats_set_str(stats, "requested_mode", ci_observe_spelling) < 0 ||
      stats_set_uint(stats, "threshold", ci_observe_threshold) < 0 ||
      stats_set_str(stats, "threshold_source", ci_observe_threshold_source) <
          0 ||
      PyDict_SetItemString(
          stats,
          "auto_classify",
          ci_shared_autojit_classify ? Py_True : Py_False) < 0 ||
      stats_set_uint(stats, "codes_seen", ci_observe_codes_seen) < 0 ||
      stats_set_uint(stats, "events_dropped", ci_observe_events_dropped) < 0 ||
      stats_set_uint(stats, "fresh_attachments", ci_observe_fresh_attachments) <
          0 ||
      stats_set_uint(
          stats,
          "auto_jit_disabled_codes",
          ci_observe_auto_jit_disabled_codes) < 0 ||
      stats_set_uint(stats, "late_deferrals", ci_observe_late_deferrals) < 0 ||
      stats_set_uint(
          stats,
          "post_publication_interpreted_frames",
          ci_observe_post_publication_interpreted_frames) < 0 ||
      stats_set_uint(stats, "watched_codes", ci_observe_watched) < 0 ||
      stats_set_uint(stats, "keyed_slots", ci_observe_live) < 0 ||
      stats_set_uint(stats, "table_capacity", ci_observe_capacity) < 0 ||
      PyDict_SetItemString(stats, "events", events) < 0) {
    Py_DECREF(stats);
    goto error;
  }
  Py_DECREF(events);
  ci_observe_compiling = was_bookkeeping;
  return stats;

error:
  Py_DECREF(events);
  ci_observe_compiling = was_bookkeeping;
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
  ci_observe_watched = 0;

  for (Py_ssize_t i = 0; i < ci_observe_event_count; i++) {
    free(ci_observe_events[i].qualname);
    free(ci_observe_events[i].filename);
    ci_observe_events[i].qualname = NULL;
    ci_observe_events[i].filename = NULL;
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
  ci_observe_mode = CI_JIT_MODE_311_OFF;
  ci_observe_spelling = "off";
  ci_observe_execute = 0;
  ci_observe_threshold = 0;
  ci_observe_threshold_source = "unconfigured";
  ci_shared_autojit_set = 0;
  ci_shared_autojit_configured = 0;
  ci_shared_autojit_valid = 0;
  ci_shared_autojit_classify = 0;
  ci_shared_autojit_threshold = 0;
  ci_observe_codes_seen = 0;
  ci_observe_events_dropped = 0;
  ci_observe_fresh_attachments = 0;
  ci_observe_auto_jit_disabled_codes = 0;
  ci_observe_late_deferrals = 0;
  ci_observe_post_publication_interpreted_frames = 0;
}
