// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/dict.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/StaticPython/typed-args-info.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <unordered_map>

namespace jit {

// Mutator for an instance attribute that is stored in a split dictionary
struct SplitMutator {
  PyObject* getAttr(PyObject* obj, PyObject* name);
  int setAttr(PyObject* obj, PyObject* name, PyObject* value);
#if PY_VERSION_HEX >= 0x030E0000
  int setAttrKnownOffset(PyObject* obj, PyObject* name, PyObject* value);
  PyObject* getAttrInline(PyObject* obj, PyObject* name);
  PyObject* getAttrSlowPath(
      PyObject* obj,
      PyObject* name,
      BorrowedRef<PyDictObject> dict);
  int setAttrInline(PyObject* obj, PyObject* name, PyObject* value);
  PyObject* getAttrInlineKnownOffset(PyObject* obj, PyObject* name);
  int setAttrInlineKnownOffset(PyObject* obj, PyObject* name, PyObject* value);
#endif
  bool canInsertToSplitDict(BorrowedRef<PyDictObject> dict, BorrowedRef<> name);

  Py_ssize_t val_offset;
  PyDictKeysObject* keys; // Borrowed
};

// Mutator for an instance attribute that is stored in a combined dictionary
// (non-managed-dict types with tp_dictoffset).
struct CombinedMutator {
  PyObject* getAttr(PyObject* obj, PyObject* name);
  int setAttr(PyObject* obj, PyObject* name, PyObject* value);

  Py_ssize_t dict_offset;
  BorrowedRef<> getattr_method;
};

// Mutator for an instance attribute on a managed-dict type where the attribute
// is not in the shared keys (e.g. shared keys are full). Uses the managed dict
// APIs directly rather than a stored dict_offset.
struct DictMutator {
  PyObject* getAttr(PyObject* obj, PyObject* name);
  int setAttr(PyObject* obj, PyObject* name, PyObject* value);

  BorrowedRef<> getattr_method;
#if PY_VERSION_HEX < 0x030C0000
  // me_key-self-validating entry indexes, refreshed on
  // every slow-path hit.  dict_hint indexes the receiver dict keys
  // (materialized instances); values_hint indexes the type's cached
  // shared keys (values-form instances).  A wrong hint just falls back,
  // so per-instance keys divergence costs a refresh, not correctness.
  Py_ssize_t dict_hint{-1};
  Py_ssize_t values_hint{-1};
#endif
};

// Mutator for a data descriptor
struct DataDescrMutator {
  PyObject* getAttr(PyObject* obj);
  int setAttr(PyObject* obj, PyObject* value);

  BorrowedRef<> descr;
  BorrowedRef<PyTypeObject> descr_type;
#if PY_VERSION_HEX < 0x030C0000
  // The descriptor type's tp_version_tag captured at fill.  The kind
  // selection baked descr_type's tp_descr_get/tp_descr_set into the
  // dispatch, and mutating the DESCRIPTOR's type (del D.__set__) never
  // touches the receiver type's version -- so a hit must pull-validate
  // this tag as well (see AttributeMutator::descrVersionMatches).
  uint32_t descr_type_version{0};
#endif
};

// Mutator for a member descriptor
struct MemberDescrMutator {
  PyObject* getAttr(PyObject* obj);
  int setAttr(PyObject* obj, PyObject* value);

  PyMemberDef* memberdef;
  BorrowedRef<> getattr_method; // Cached __getattr__ if the type has one
};

// Attribute corresponds to a non-data descriptor or a class variable
struct DescrOrClassVarMutator {
  PyObject* getAttr(PyObject* obj, PyObject* name);
  int setAttr(PyObject* obj, PyObject* name, PyObject* value);

  BorrowedRef<> descr;
  uint32_t keys_version;
};

// Mutator for attribute lookups on types that define __getattr__.
// Used when a particular attribute name is absent from both the type's MRO
// and the instance dict, causing __getattr__ to be invoked.
struct GetAttrMutator {
  PyObject* getAttr(PyObject* obj, PyObject* name);

  BorrowedRef<> getattr_method;
  uint32_t keys_version;
};

// An instance of AttributeMutator is specialized to more efficiently perform a
// get/set of a particular kind of attribute.
class AttributeMutator {
 public:
  // Kind enum is designed to fit within 3 bits and it's value is embedded into
  // the type_ pointer
  enum class Kind : uint8_t {
    kSplit,
    kSplitInline,
    kCombined,
    kDataDescr,
    kMemberDescr,
    kDescrOrClassVar,
    kGetAttr,
    kDict,
    kMaxValue,
  };
  static_assert(
      static_cast<uint8_t>(Kind::kMaxValue) <= 8,
      "Kind enum should fit in 3 bits");

  AttributeMutator();
  PyTypeObject* type() const {
    // clear tagged bits and return
    return reinterpret_cast<PyTypeObject*>(type_ & ~kindMask());
  }
  void reset();
  bool isEmpty() const {
    return type_ == 0;
  }
  void set_combined(PyTypeObject* type);
  void set_dict(PyTypeObject* type);
  void set_data_descr(PyTypeObject* type, PyObject* descr);
  void set_member_descr(PyTypeObject* type, PyObject* descr);
  void set_descr_or_classvar(
      PyTypeObject* type,
      PyObject* descr,
      uint32_t keys_version);
  void set_split(
      PyTypeObject* type,
      Py_ssize_t val_offset,
      PyDictKeysObject* keys,
      bool values_inline);
  void set_getattr(
      PyTypeObject* type,
      PyObject* getattr_method,
      uint32_t keys_version);
  BorrowedRef<PyTypeObject> watchedDescrType() const;

  PyObject* getAttr(PyObject* obj, PyObject* name);
  int setAttr(PyObject* obj, PyObject* name, PyObject* value);

  static void changeKindFromSplitInline(SplitMutator* split, Kind new_kind);
#if PY_VERSION_HEX < 0x030C0000
  // re-kind a kSplit entry whose receivers have
  // materialized and re-keyed their dicts to kDict, so subsequent hits
  // take the hint-indexed read (asm kdict arm / DictMutator) instead of
  // a full hash probe per access, and answer the demoting access.
  static PyObject*
  demoteSplitToDict(SplitMutator* split, PyObject* obj, PyObject* name);
  // Store-side twin: same demotion, answering the demoting store.
  static int demoteSplitToDictStore(
      SplitMutator* split,
      PyObject* obj,
      PyObject* name,
      PyObject* value);
#endif
  static constexpr uintptr_t kindMask() {
    return 0x07;
  }
  static constexpr uintptr_t splitInlineKnownOffsetKind() {
    // e3f93f10 makes kSplitInline entries store a resolved val_offset at fill
    // time, so the old codegen "known offset" check maps to kSplitInline.
    return static_cast<uintptr_t>(Kind::kSplitInline);
  }
  static constexpr size_t typeOffset() {
    return offsetof(AttributeMutator, type_);
  }
  static constexpr size_t splitValOffsetOffset() {
    return offsetof(AttributeMutator, split_) +
        offsetof(SplitMutator, val_offset);
  }
#if PY_VERSION_HEX < 0x030C0000
  // the 3.11 stubs pull-validate against the captured
  // receiver type version, exactly like doInvoke.
  static constexpr size_t typeVersionOffset() {
    return offsetof(AttributeMutator, type_version_);
  }
  static constexpr size_t memberDescrMemberdefOffset() {
    return offsetof(AttributeMutator, member_descr_) +
        offsetof(MemberDescrMutator, memberdef);
  }
  static constexpr uintptr_t memberDescrKind() {
    return static_cast<uintptr_t>(Kind::kMemberDescr);
  }
  static constexpr size_t splitKeysOffset() {
    return offsetof(AttributeMutator, split_) + offsetof(SplitMutator, keys);
  }
  static constexpr size_t dictHintOffset() {
    return offsetof(AttributeMutator, dict_) + offsetof(DictMutator, dict_hint);
  }
  static constexpr uintptr_t dictKind() {
    return static_cast<uintptr_t>(Kind::kDict);
  }
  static constexpr uintptr_t descrOrClassVarKind() {
    return static_cast<uintptr_t>(Kind::kDescrOrClassVar);
  }
  static constexpr size_t descrOrClassVarDescrOffset() {
    return offsetof(AttributeMutator, descr_or_cvar_) +
        offsetof(DescrOrClassVarMutator, descr);
  }
  static constexpr size_t descrOrClassVarKeysVersionOffset() {
    return offsetof(AttributeMutator, descr_or_cvar_) +
        offsetof(DescrOrClassVarMutator, keys_version);
  }
  // kDataDescr entry fields for the inline stub's direct
  // descriptor call.
  static constexpr uintptr_t dataDescrKind() {
    return static_cast<uintptr_t>(Kind::kDataDescr);
  }
  static constexpr size_t dataDescrDescrOffset() {
    return offsetof(AttributeMutator, data_descr_) +
        offsetof(DataDescrMutator, descr);
  }
  static constexpr size_t dataDescrTypeOffset() {
    return offsetof(AttributeMutator, data_descr_) +
        offsetof(DataDescrMutator, descr_type);
  }
  static constexpr size_t dataDescrTypeVersionOffset() {
    return offsetof(AttributeMutator, data_descr_) +
        offsetof(DataDescrMutator, descr_type_version);
  }
#endif
  template <typename T>
  static AttributeMutator* from(T* mutator) {
    return reinterpret_cast<AttributeMutator*>(
        reinterpret_cast<uintptr_t>(mutator) -
        offsetof(AttributeMutator, split_));
  }

#if PY_VERSION_HEX < 0x030C0000
  // Pull-based validity on 3.11 (no type watchers exist there): the
  // receiver type's tp_version_tag captured at fill time.  A hit requires
  // the live receiver's tag to equal this; PyType_Modified zeroes the tag
  // and reassignment draws a fresh number from a monotonic global stream,
  // so a stale -- or dead-and-reallocated -- type can never revalidate.
  bool typeVersionMatches(PyTypeObject* live_type) const {
    return live_type->tp_version_tag == type_version_;
  }

  // Second half of the 3.11 pull validation, for the one kind whose
  // dispatch bakes in another type's protocol: kDataDescr captured
  // descr_type's tp_descr_get/tp_descr_set at fill, and deleting
  // D.__set__ mutates D -- not the receiver -- so the receiver-version
  // gate cannot see it.  The stale entry would misroute precedence
  // (an instance shadow must win once the descriptor stops being a data
  // descriptor) or call a now-NULL slot.
  //
  // The POINTER comparison comes first and gates the version load:
  // d.__class__ = D2 swaps the descriptor's type without touching the
  // receiver or D1's version -- and can leave the captured descr_type
  // pointing at a dead type if the swap dropped D1's last reference.
  // Py_TYPE(descr) is safe to read (typeVersionMatches passing means the
  // receiver still holds the descriptor), the captured pointer is only
  // compared, and the version is loaded through Py_TYPE(descr) itself
  // only after they are proven identical.  Other kinds need no tag:
  // kDescrOrClassVar re-reads the slots on every call, and kMemberDescr
  // only ever captures the immutable PyMemberDescr_Type.
  bool descrVersionMatches() const {
    if (get_kind() != Kind::kDataDescr) {
      return true;
    }
    PyTypeObject* live_descr_type = Py_TYPE(data_descr_.descr.get());
    return live_descr_type == data_descr_.descr_type &&
        live_descr_type->tp_version_tag == data_descr_.descr_type_version;
  }
#endif

 private:
  void set_type(PyTypeObject* type, Kind kind);
  Kind get_kind() const {
    return static_cast<Kind>(type_ & kindMask());
  }

  uintptr_t type_; // This value stores both a PyTypeObject* for the type object
                   // and the Kind enum value which are bitpacked together to
                   // reduce memory consumption
#if PY_VERSION_HEX < 0x030C0000
  uint32_t type_version_{0};
#endif
  union {
    SplitMutator split_;
    CombinedMutator combined_;
    DictMutator dict_;
    DataDescrMutator data_descr_;
    MemberDescrMutator member_descr_;
    DescrOrClassVarMutator descr_or_cvar_;
    GetAttrMutator getattr_;
  };
};

class AttributeCache {
 public:
  AttributeCache();
  ~AttributeCache();

  void typeChanged(PyTypeObject* type);
  void descrTypeChanged(PyTypeObject* type);
  static constexpr size_t entriesOffset() {
    return offsetof(AttributeCache, entries_);
  }

#if PY_VERSION_HEX < 0x030C0000
  // Type-receiver entries.  The
  // general fill refuses class receivers (metatype getattro), so class
  // constant reads (deltablue Strength.FORWARD: 12.9M futile fill calls
  // per run) re-ran the full lookup every time.  Two forms: kValue keys
  // on the receiver type (identity-stable class vars / functions /
  // wrapper+method descriptors, staticmethod unwrap), kMetaDescr keys on
  // the metatype (data descriptors like type.__name__, shared by every
  // class of that metatype).  Both pull-validate tp_version_tag before
  // the borrowed payload is touched; entries round-robin to absorb
  // polymorphic receivers; lazily allocated so non-type sites pay
  // nothing.
  struct TypeRecvEntry {
    PyTypeObject* recv{nullptr}; // pointer compare only; version-gated
    uint32_t recv_version{0};
    uint32_t meta_version{0};
    // kDescrGet: a descriptor found on the receiver class whose class-level
    // access is a fresh object each time (classmethod, classmethod_descriptor,
    // custom __get__): the hit runs type_getattro's own step,
    // tp_descr_get(descr, NULL, receiver), with the descriptor pinned by the
    // receiver version and the metatype version proving no data descriptor
    // shadows it.
    enum class Form : uint8_t {
      kNone,
      kValue,
      kMetaDescr,
      kDescrGet
    } form{Form::kNone};
    PyObject* payload{nullptr}; // borrowed
    // The payload's own type version: a value whose type later grows
    // __get__ becomes a descriptor, a descriptor whose type loses __get__
    // stops being one; neither touches the receiver or metatype version.
    uint32_t payload_version{0};
  };
  struct TypeRecvEntries {
    static constexpr size_t kNumEntries = 8;
    TypeRecvEntry entries[kNumEntries];
    size_t rr{0};
  };

  PyObject* typeRecvGetAttr(PyObject* obj, PyObject* name);
  void typeRecvTryFill(PyObject* obj, PyObject* name, PyObject* result);

  std::unique_ptr<TypeRecvEntries> type_recv_;
  // bound-method receivers.  method_getattro delegates any
  // name the method type itself lacks to im_func; that verdict is fixed
  // for the static method type, so it is taken once per site and the
  // function is then served by the ordinary entries.  0 unknown, 1
  // delegates, 2 the method type has the name.
  uint8_t method_delegate_{0};
#endif

 protected:
  std::span<AttributeMutator> entries() {
    return {entries_, getConfig().attr_cache_size};
  }

  AttributeMutator* findEmptyEntry();

  void fill(BorrowedRef<> obj, BorrowedRef<> name, bool is_set);

  void
  fill(BorrowedRef<> obj, BorrowedRef<> name, BorrowedRef<> descr, bool is_set);

  AttributeMutator entries_[0];
};

struct AttributeCacheSizeTrait {
  static size_t size() {
    auto base = sizeof(AttributeCache);
    auto extra = sizeof(AttributeMutator) * getConfig().attr_cache_size;
    return base + extra;
  }
};

// A cache for an individual StoreAttrCached instruction.
//
// The logic of StoreAttrCache::invoke is equivalent to PyObject_SetAttr,
// however, it can be specialized and accelerated depending on the kinds of
// receiver types that are seen.
class StoreAttrCache : public AttributeCache {
 public:
  StoreAttrCache() = default;

  // Return 0 on success and a negative value on failure.
  static int
  invoke(StoreAttrCache* cache, PyObject* obj, PyObject* name, PyObject* value);

 private:
  DISALLOW_COPY_AND_ASSIGN(StoreAttrCache);

  int doInvoke(PyObject* obj, PyObject* name, PyObject* value);
  int invokeSlowPath(PyObject* obj, PyObject* name, PyObject* value);
};

// A cache for an individual LoadAttrCached instruction.
//
// The logic of LoadAttrCache::invoke is equivalent to PyObject_GetAttr,
// however, it can be specialized and accelerated depending on the kinds of
// receiver types that are seen.
class LoadAttrCache : public AttributeCache {
 public:
  LoadAttrCache() = default;

  // Returns a new reference to the value or NULL on error.
  static PyObject* invoke(LoadAttrCache* cache, PyObject* obj, PyObject* name);

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadAttrCache);

  PyObject* doInvoke(PyObject* obj, PyObject* name);
  PyObject* invokeSlowPath(PyObject* obj, PyObject* name);
};

// A cache for LoadAttr instructions where we expect the receiver to be a type
// object.
//
// The code for loading an attribute where the expected receiver is a type is
// specialized into a fast path and a slow path. The first element is loaded
// from the cache and compared against the receiver. If they are equal, the
// second element (the cached value) is loaded. If they are not equal,
// `invoke()` is called, which performs the full lookup and potentially fills
// the cache.
class LoadTypeAttrCache {
 public:
  LoadTypeAttrCache();
  ~LoadTypeAttrCache();

  static PyObject*
  invoke(LoadTypeAttrCache* cache, PyObject* obj, PyObject* name);

  // Get the addresses of the type and value cache entries.
  PyTypeObject** typeAddr();
  PyObject** valueAddr();

  void typeChanged(BorrowedRef<PyTypeObject> type);

 private:
  PyObject* invokeSlowPath(BorrowedRef<> obj, BorrowedRef<> name);

  // `metatype` and `value_guard_type` carry the 3.11 pull-validation facts
  // (see the members below); both are ignored on 3.12+, where the type
  // watcher retires entries instead.
  void fill(
      BorrowedRef<PyTypeObject> type,
      BorrowedRef<> value,
      [[maybe_unused]] BorrowedRef<PyTypeObject> metatype,
      [[maybe_unused]] BorrowedRef<PyTypeObject> value_guard_type);
  void reset();

  // Cached type and value, stored as raw pointers so codegen can access them by
  // address.
  PyTypeObject* type_;
  PyObject* value_;

#if PY_VERSION_HEX < 0x030C0000
 public:
  // Pull-based validity on 3.11, which has no type watcher to retire this
  // entry.  type_getattro's cached answer rests on three mutable facts, and
  // a hit must re-prove all of them:
  //
  //   * the OWNER type's MRO still resolves the name the same way
  //     (type_version_);
  //   * the METATYPE still routes through type_getattro and still has no
  //     data descriptor of this name (metatype_ + metatype_version_) --
  //     mutating the metaclass never touches the owner's version;
  //   * the cached value is still a non-descriptor, when that is what made
  //     it cachable (value_guard_type_ + value_guard_version_); a heap type
  //     that later gains __get__ turns the plain value into a descriptor.
  //
  // Entries whose facts cannot all be pinned (a type without a valid
  // version tag) are simply not filled, so the site keeps using the
  // generic path rather than a half-guarded cache.
  bool hitIsValid311(BorrowedRef<PyTypeObject> receiver) const;

 private:
  uint32_t type_version_{0};
  PyTypeObject* metatype_{nullptr};
  uint32_t metatype_version_{0};
  // nullptr when the cachability decision did not depend on a mutable
  // descriptor protocol (plain functions and staticmethods are pinned to
  // static builtin types that cannot gain __get__ from Python).
  PyTypeObject* value_guard_type_{nullptr};
  uint32_t value_guard_version_{0};
  bool pull_valid_{false};
#endif
};

#define FOREACH_CACHE_MISS_REASON(V) \
  V(WrongTpGetAttro)                 \
  V(PyDescrIsData)                   \
  V(Uncategorized)

enum class CacheMissReason {
#define DECLARE_CACHE_MISS_REASON(name) k##name,
  FOREACH_CACHE_MISS_REASON(DECLARE_CACHE_MISS_REASON)
#undef DECLARE_CACHE_MISS_REASON
};

std::string_view cacheMissReason(CacheMissReason reason);

struct CacheMiss {
  int count{0};
  CacheMissReason reason{CacheMissReason::kUncategorized};
};

struct CacheStats {
  std::string filename;
  std::string method_name;
  std::unordered_map<std::string, CacheMiss> misses;
};

class LoadTypeMethodCache;

class LoadMethodCache {
 public:
  struct Entry {
    BorrowedRef<PyTypeObject> type;
    BorrowedRef<> value;
    uint32_t keys_version;
#if PY_VERSION_HEX < 0x030C0000
    // Pull-based validity (see AttributeMutator::typeVersionMatches).
    uint32_t type_version{0};
    // the shared-keys version could not prove the instance
    // dict never shadows this name (dk_version == 0 after late-attr
    // growth); a hit must peek the instance storage instead.
    bool peek_shadow{false};
    // memo for the values-form peek.  peek_keys is
    // compared against the LIVE type's cached keys (never dereferenced
    // stale); while identical and dk_nentries == peek_nentries the name
    // membership answer is pinned: peek_hint >= 0 gives the entry index
    // (me_key-validated at hit), kPeekAbsent pins non-membership, and a
    // shadow answer still takes the slow path.
    static constexpr Py_ssize_t kPeekAbsent = -2;
    PyDictKeysObject* peek_keys{nullptr};
    Py_ssize_t peek_nentries{0};
    Py_ssize_t peek_hint{-1};
    // For a materialized combined-keys receiver (anchor) the
    // keys table is owned by the dict alone: dict.clear() frees it and the
    // next insert can allocate a new table at the same address with the
    // same dk_nentries, so the pointer-and-count memo would pin a stale
    // absence.  The anchor therefore also records the table's keys
    // version, which a fresh table does not have (dk_version == 0) and
    // which every key-set change resets.  Value replacement keeps it.
    uint32_t peek_keys_version{0};
    // the name is an INSTANCE attribute callable (bound
    // method stored in __init__, the _Unpickler.read pattern) -- the
    // class side has no such attribute at all (pinned by type_version),
    // so the hit caches the lookup SHAPE, not the per-instance value:
    // slot non-null -> {Py_None, slot}.  value stays null for this form.
    bool inst_attr{false};
    void clearPeekKeys();
#endif

    bool isValidKeysVersion(BorrowedRef<> obj);
  };

  ~LoadMethodCache();

  static LoadMethodResult
  lookupHelper(LoadMethodCache* cache, BorrowedRef<> obj, BorrowedRef<> name);
  LoadMethodResult lookup(BorrowedRef<> obj, BorrowedRef<> name);
  void typeChanged(PyTypeObject* type);

  void initCacheStats(const char* filename, const char* method_name);
  void clearCacheStats();
  const CacheStats* cacheStats();

 private:
  LoadMethodResult lookupSlowPath(BorrowedRef<> obj, BorrowedRef<> name);
  void
  fill(BorrowedRef<PyTypeObject> type, BorrowedRef<> value, BorrowedRef<> name);

  std::array<Entry, 4> entries_;
  std::unique_ptr<CacheStats> cache_stats_;
#if PY_VERSION_HEX < 0x030C0000
  // a class receiver (Cls.method / cls.method inside a
  // classmethod) can never match the per-metatype entries above, so every
  // such load fell through to PyObject_GetAttr and a fresh bound method.
  // Lazily hold the pull-validated type-receiver cache for it instead.
  std::unique_ptr<LoadTypeMethodCache> type_recv_311_;
  void absorbTypeRecvStats311();
  // Misses since the last hit; a site whose class receivers keep changing
  // (many classes through one call site) would only thrash the single
  // entry, so it falls back to the generic path once this trips.
  uint16_t type_recv_misses_311_{0};
  bool type_recv_off_311_{false};
#endif

 public:
  // layout accessors for the 3.11 aarch64 inline stub.
  static constexpr size_t numEntries() {
    return 4;
  }
  static constexpr size_t entriesOffset() {
    return offsetof(LoadMethodCache, entries_);
  }
  static constexpr size_t entrySize() {
    return sizeof(Entry);
  }
  static constexpr size_t entryTypeOffset() {
    return offsetof(Entry, type);
  }
  static constexpr size_t entryValueOffset() {
    return offsetof(Entry, value);
  }
  static constexpr size_t entryKeysVersionOffset() {
    return offsetof(Entry, keys_version);
  }
#if PY_VERSION_HEX < 0x030C0000
  static constexpr size_t entryTypeVersionOffset() {
    return offsetof(Entry, type_version);
  }
  static constexpr size_t entryPeekShadowOffset() {
    return offsetof(Entry, peek_shadow);
  }
  static constexpr size_t entryPeekKeysOffset() {
    return offsetof(Entry, peek_keys);
  }
  static constexpr size_t entryPeekNentriesOffset() {
    return offsetof(Entry, peek_nentries);
  }
  static constexpr size_t entryPeekKeysVersionOffset() {
    return offsetof(Entry, peek_keys_version);
  }
  static constexpr size_t entryPeekHintOffset() {
    return offsetof(Entry, peek_hint);
  }
  static constexpr size_t entryInstAttrOffset() {
    return offsetof(Entry, inst_attr);
  }
#endif
};

// A cache for LoadMethodCached instructions where we expect the receiver to be
// a type object.
//
// The first entry in `entry` is the type receiver. The second entry in `entry`
// is the cached value.
//
// The code for loading a method where the expected receiver is a type is
// specialized into a fast path and a slow path. The first element is loaded
// from the cache and compared against the receiver. If they are equal, the
// `getValueHelper()` is called which returns the cached value. If they are not
// equal, `lookupHelper()` is called, which performs the full lookup and
// potentially fills the cache.
class LoadTypeMethodCache {
 public:
  ~LoadTypeMethodCache();

  static LoadMethodResult
  lookupHelper(LoadTypeMethodCache* cache, PyTypeObject* obj, PyObject* name);

  static LoadMethodResult getValueHelper(
      LoadTypeMethodCache* cache,
      PyObject* obj);
#if PY_VERSION_HEX < 0x030C0000
  // Validated hit when the receiver is the cached type, full lookup (and
  // fill) otherwise.  Exceptions propagate as {NULL, NULL}; the caller is
  // a deopt-checked instruction.
  static LoadMethodResult lookupOrHitHelper311(
      LoadTypeMethodCache* cache,
      PyTypeObject* obj,
      PyObject* name);
#endif

  LoadMethodResult lookup(BorrowedRef<PyTypeObject> obj, BorrowedRef<> name);

  // Get the address of the cached type object.
  PyTypeObject** typeAddr();

  // Get the cached method value.
  BorrowedRef<> value();

  void typeChanged(BorrowedRef<PyTypeObject> type);

  void initCacheStats(const char* filename, const char* method_name);
  void clearCacheStats();
  const CacheStats* cacheStats();

 private:
#if PY_VERSION_HEX < 0x030C0000
  enum class WrapperForm : uint8_t { kPlain, kClassMethod, kStaticMethod };
  void fill(
      BorrowedRef<PyTypeObject> type,
      BorrowedRef<> value,
      bool is_bound_meth,
      WrapperForm form = WrapperForm::kPlain);
#else
  void
  fill(BorrowedRef<PyTypeObject> type, BorrowedRef<> value, bool is_bound_meth);
#endif

  // Borrowed, but uses a raw pointer as typeAddr() will return the address of
  // this field for codegen purposes.
  PyTypeObject* type_;
  BorrowedRef<> value_;
  std::unique_ptr<CacheStats> cache_stats_;
  bool is_unbound_meth_;
#if PY_VERSION_HEX < 0x030C0000
  // Pull-based validity: the receiver type's tag at fill time, plus the
  // attribute name so a stale fast-path hit (getValueHelper receives only
  // the receiver) can re-run the full lookup.  The name is borrowed from
  // co_names of the code object whose site owns this cache; the owning
  // CodeRuntime keeps that code alive.
  //
  // The METATYPE is guarded too: every fill below happened only because
  // the metatype routes through type_getattro and holds no data descriptor
  // of this name, and mutating a metaclass never bumps the owner class's
  // own version tag.  Without this pair, adding a same-named data
  // descriptor to the metaclass would leave the class-dict method cached
  // and winning, while stock hands the metaclass descriptor the call.
  uint32_t type_version_{0};
  BorrowedRef<> name_;
  PyTypeObject* metatype_{nullptr};
  uint32_t metatype_version_{0};
  // What value_ is on 3.11: the method itself, or the classmethod /
  // staticmethod object found in the class dict (pinned there by the
  // receiver version).  The wrapper's callable is read on every hit:
  // classmethod.__init__ / staticmethod.__init__ replace it in place
  // without any version moving, so a cached inner callable would go stale
  // or dangle.
  WrapperForm wrapper_form_{WrapperForm::kPlain};
#endif
};

// A cache for an individual LoadModuleAttrCached instruction.
class LoadModuleAttrCache {
 public:
  static PyObject* lookupHelper(
      LoadModuleAttrCache* cache,
      BorrowedRef<> obj,
      BorrowedRef<> name);
  PyObject* lookup(BorrowedRef<> obj, BorrowedRef<> name);

 private:
  PyObject* lookupSlowPath(BorrowedRef<> obj, BorrowedRef<> name);
  void
  fill(BorrowedRef<> obj, BorrowedRef<> value, ci_dict_version_tag_t version);

  // This corresponds to module __dict__'s version which allows us
  // to correctly invalidate the cache whenever the dictionary changes.
  BorrowedRef<> module_;
#if PY_VERSION_HEX >= 0x030E0000
  PyObject** cache_;
#else
  ci_dict_version_tag_t version_{0};
  BorrowedRef<> value_;
#endif
};

class LoadModuleMethodCache {
 public:
  static LoadMethodResult lookupHelper(
      LoadModuleMethodCache* cache,
      BorrowedRef<> obj,
      BorrowedRef<> name);
  LoadMethodResult lookup(BorrowedRef<> obj, BorrowedRef<> name);
  BorrowedRef<> moduleObj();
#if PY_VERSION_HEX < 0x030E0000
  BorrowedRef<> value();
#else
  PyObject** cache() {
    return cache_;
  }
#endif

 private:
  LoadMethodResult lookupSlowPath(BorrowedRef<> obj, BorrowedRef<> name);

  // This corresponds to module __dict__'s version which allows us
  // to correctly invalidate the cache whenever the dictionary changes.
  BorrowedRef<> module_obj_;
#if PY_VERSION_HEX >= 0x030E0000
  PyObject** cache_;
#else
  ci_dict_version_tag_t module_version_{0};
  BorrowedRef<> value_;
#endif
};

// Invalidate all load/store attr caches for type
void notifyICsTypeChanged(BorrowedRef<PyTypeObject> type);

#if PY_VERSION_HEX < 0x030C0000
// MR-09 observability: per-class cache tallies for the pull-validated 3.11
// arms.  A hit consumed a validated entry; a miss took the slow path with
// no usable entry; a fill wrote an entry; an invalidation is a pull check
// retiring a stale entry.  Monotonic counters, read through
// cinderjit.get_attr_cache_stats().
struct AttrCacheClassStats {
  uint64_t fills{0};
  uint64_t hits{0};
  uint64_t misses{0};
  uint64_t invalidations{0};
};

struct AttrCacheStats311 {
  AttrCacheClassStats load_attr;
  AttrCacheClassStats store_attr;
  AttrCacheClassStats load_method;
  AttrCacheClassStats load_type_attr;
  AttrCacheClassStats load_type_method;
  AttrCacheClassStats load_module_attr;
  AttrCacheClassStats load_module_method;
};

AttrCacheStats311& attrCacheStats311();

// Replace an existing value of a materialized instance dict in place: the
// new value is owned before the old one is released (storing the same object
// back is safe) and the dict's PEP 509 version advances exactly as stock's
// STORE_ATTR_WITH_HINT / insertdict do.  The method cache's negative anchors
// are keyed on the keys table version, which a value replacement leaves
// untouched.
void replaceMaterializedDictValue311(
    PyDictObject* dict,
    PyObject** slot,
    PyObject* value);
#endif

} // namespace jit

struct FunctionEntryCacheValue {
  void** ptr{nullptr};
  Ref<_PyTypedArgsInfo> arg_info;
};

using FunctionEntryCacheMap =
    jit::UnorderedMap<PyFunctionObject*, FunctionEntryCacheValue>;
