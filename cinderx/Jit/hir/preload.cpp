// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/preload.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/dict.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#include "cinderx/Interpreter/cinder_opcode.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/StaticPython/classloader.h"
#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/StaticPython/vtable_builder.h"
#include "cinderx/module_state.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif

#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace jit::hir {

namespace {

OwnedType resolve_type_descr(BorrowedRef<> descr) {
  int optional, exact;
  auto type = Ref<PyTypeObject>::steal(
      _PyClassLoader_ResolveType(descr, &optional, &exact));

  return {
      std::move(type), static_cast<bool>(optional), static_cast<bool>(exact)};
}

FieldInfo resolve_field_descr(BorrowedRef<PyTupleObject> descr) {
  int field_type;
  Py_ssize_t offset = _PyClassLoader_ResolveFieldOffset(descr, &field_type);

  JIT_THROW_IF(offset == -1, "Failed to resolve field {}", repr(descr));

  return {
      offset,
      prim_type_to_type(field_type),
      PyTuple_GET_ITEM(descr, PyTuple_GET_SIZE(descr) - 1)};
}

void _fill_primitive_arg_types_helper(
    BorrowedRef<_PyTypedArgsInfo> prim_args_info,
    ArgTypeMap& map) {
  for (Py_ssize_t i = 0; i < Py_SIZE(prim_args_info.get()); i++) {
    map.emplace(
        prim_args_info->tai_args[i].tai_argnum,
        prim_type_to_type(prim_args_info->tai_args[i].tai_primitive_type));
  }
}

void fill_primitive_arg_types_func(
    BorrowedRef<PyFunctionObject> func,
    ArgTypeMap& map) {
  auto prim_args_info =
      Ref<_PyTypedArgsInfo>::steal(_PyClassLoader_GetTypedArgsInfo(
          reinterpret_cast<PyCodeObject*>(func->func_code), 1));
  JIT_THROW_IF(
      prim_args_info == nullptr,
      "Failed to load primitive argument type information for function {}",
      funcFullname(func));
  _fill_primitive_arg_types_helper(prim_args_info, map);
}

void fill_primitive_arg_types_thunk(
    BorrowedRef<PyObject> thunk,
    ArgTypeMap& map,
    PyObject* container) {
  auto prim_args_info = Ref<_PyTypedArgsInfo>::steal(
      _PyClassLoader_GetTypedArgsInfoFromThunk(thunk, container, 1));
  JIT_THROW_IF(
      prim_args_info == nullptr,
      "Failed to load primitive argument type information for thunk {}",
      repr(thunk));

  _fill_primitive_arg_types_helper(prim_args_info, map);
}

void fill_primitive_arg_types_builtin(BorrowedRef<> callable, ArgTypeMap& map) {
  Ci_PyTypedMethodDef* def = _PyClassLoader_GetTypedMethodDef(callable);
  JIT_THROW_IF(
      def == nullptr,
      "Failed to load typed method def from {} object",
      Py_TYPE(callable)->tp_name);
  for (Py_ssize_t i = 0; def->tmd_sig[i] != nullptr; i++) {
    const Ci_Py_SigElement* elem = def->tmd_sig[i];
    int code = Ci_Py_SIG_TYPE_MASK(elem->se_argtype);
    Type typ = prim_type_to_type(code);
    if (typ <= TPrimitive) {
      map.emplace(i, typ);
    }
  }
}

#ifndef WIN32
std::unique_ptr<NativeTarget> resolve_native_target(
    BorrowedRef<> native_descr,
    BorrowedRef<> signature) {
  auto target = std::make_unique<NativeTarget>();
  void* raw_ptr = _PyClassloader_LookupSymbol(
      PyTuple_GET_ITEM(native_descr.get(), 0),
      PyTuple_GET_ITEM(native_descr.get(), 1));

  JIT_THROW_IF(
      raw_ptr == nullptr,
      "Invalid address {} for native function descr {}",
      raw_ptr,
      repr(native_descr));

  target->callable = raw_ptr;

  Py_ssize_t siglen = PyTuple_GET_SIZE(signature.get());
  auto return_type_code = _PyClassLoader_ResolvePrimitiveType(
      PyTuple_GET_ITEM(signature.get(), siglen - 1));
  target->return_type = prim_type_to_type(return_type_code);
  JIT_THROW_IF(
      !(target->return_type <= TCInt),
      "Native function return type must be a primitive int, got {}",
      target->return_type);

  // Fill in the primitive arg type map in the target (index -> Type)
  ArgTypeMap& primitive_arg_types = target->primitive_arg_types;
  for (Py_ssize_t i = 0; i < siglen - 1; i++) {
    int arg_type_code = _PyClassLoader_ResolvePrimitiveType(
        PyTuple_GET_ITEM(signature.get(), i));
    Type typ = prim_type_to_type(arg_type_code);
    JIT_THROW_IF(
        !(typ <= TCInt),
        "Native function argument {} must be a primitive int, got {}",
        i,
        typ);
    primitive_arg_types.emplace(i, typ);
  }

  return target;
}
#endif

PreloaderManager s_manager;
thread_local PreloaderManager* tls_manager = nullptr;

#if PY_VERSION_HEX >= 0x030C0000
std::optional<bool> has_no_subclasses_via_api(PyTypeObject* type) {
#ifndef _WIN32
  using GetSubclassesFunc = PyObject* (*)(PyTypeObject*);
  static auto get_subclasses = reinterpret_cast<GetSubclassesFunc>(
      dlsym(RTLD_DEFAULT, "_PyType_GetSubclasses"));
  if (get_subclasses == nullptr) {
    return std::nullopt;
  }

  Ref<> subclasses = Ref<>::steal(get_subclasses(type));
  if (subclasses == nullptr) {
    PyErr_Clear();
    return std::nullopt;
  }
  return PyList_Check(subclasses) && PyList_GET_SIZE(subclasses.get()) == 0;
#else
  return std::nullopt;
#endif
}
#endif

bool has_no_subclasses(PyTypeObject* type) {
#if PY_VERSION_HEX >= 0x030C0000
  if (auto result = has_no_subclasses_via_api(type)) {
    return *result;
  }
#endif

  PyObject* subclasses = reinterpret_cast<PyObject*>(type->tp_subclasses);
  return subclasses == nullptr ||
      (PyDict_Check(subclasses) && PyDict_Size(subclasses) == 0);
}

bool is_self_load(const BytecodeInstruction& instr) {
  switch (instr.opcode()) {
    case LOAD_FAST:
    case LOAD_FAST_BORROW:
    case LOAD_FAST_CHECK:
      return instr.oparg() == 0;
    case LOAD_FAST_LOAD_FAST:
    case LOAD_FAST_BORROW_LOAD_FAST_BORROW:
      // These fused opcodes push the high-nibble local first and the
      // low-nibble local second. A following LOAD_ATTR consumes the top value.
      return (instr.oparg() & 0xf) == 0;
    default:
      return false;
  }
}

bool has_self_attr_load(BorrowedRef<PyCodeObject> code) {
  bool previous_was_self = false;
  for (const auto& instr : BytecodeInstructionBlock{code}) {
    if ((instr.opcode() == LOAD_ATTR || instr.opcode() == LOAD_METHOD) &&
        previous_was_self) {
      return true;
    }
    previous_was_self = is_self_load(instr);
  }
  return false;
}

std::optional<OwnedType> infer_method_self_type_candidate(
    BorrowedRef<PyCodeObject> code,
    BorrowedRef<PyDictObject> globals) {
  if (code->co_argcount < 1 || !PyUnicode_CheckExact(code->co_qualname)) {
    return std::nullopt;
  }
  if (!has_self_attr_load(code)) {
    return std::nullopt;
  }

  BorrowedRef<> arg0_name_obj{jit::getVarname(code, 0)};
  if (!PyUnicode_CheckExact(arg0_name_obj)) {
    return std::nullopt;
  }
  const char* arg0_name = PyUnicode_AsUTF8(arg0_name_obj);
  if (arg0_name == nullptr || std::strcmp(arg0_name, "self") != 0) {
    PyErr_Clear();
    return std::nullopt;
  }

  const char* qualname = PyUnicode_AsUTF8(code->co_qualname);
  if (qualname == nullptr) {
    PyErr_Clear();
    return std::nullopt;
  }

  std::string_view qualname_view{qualname};
  std::size_t dot = qualname_view.find('.');
  if (dot == std::string_view::npos || dot == 0 ||
      qualname_view.rfind('.') != dot ||
      qualname_view.find('<') != std::string_view::npos) {
    return std::nullopt;
  }

  std::string owner_name{qualname_view.substr(0, dot)};
  std::string method_name{qualname_view.substr(dot + 1)};
  ThreadedCompileSerialize guard;
  BorrowedRef<> owner_obj{PyDict_GetItemString(globals, owner_name.c_str())};
  if (owner_obj == nullptr || !PyType_Check(owner_obj)) {
    return std::nullopt;
  }

  auto owner_type = reinterpret_cast<PyTypeObject*>(owner_obj.get());
  if (!(owner_type->tp_flags & Py_TPFLAGS_HEAPTYPE) ||
      owner_type->tp_getattro != PyObject_GenericGetAttr ||
      !has_no_subclasses(owner_type)) {
    return std::nullopt;
  }

  BorrowedRef<> method_obj{
      PyDict_GetItemString(owner_type->tp_dict, method_name.c_str())};
  if (method_obj == nullptr || !PyFunction_Check(method_obj)) {
    return std::nullopt;
  }
  auto method_func = reinterpret_cast<PyFunctionObject*>(method_obj.get());
  if (reinterpret_cast<PyCodeObject*>(method_func->func_code) != code.get()) {
    return std::nullopt;
  }

  return OwnedType{Ref<PyTypeObject>::create(owner_type), false, true};
}

} // namespace

std::unique_ptr<Preloader> Preloader::make(
    BorrowedRef<PyFunctionObject> func,
    Ref<> reifier) {
  return Preloader::make(
      func->func_code,
      func->func_builtins,
      func->func_globals,
      AnnotationIndex::from_function(func),
      funcFullname(func),
      std::move(reifier));
}

std::unique_ptr<Preloader> Preloader::make(
    BorrowedRef<PyCodeObject> code,
    BorrowedRef<PyDictObject> builtins,
    BorrowedRef<PyDictObject> globals,
    std::unique_ptr<AnnotationIndex> annotations,
    const std::string& fullname,
    Ref<> reifier) {
  auto preloader = std::unique_ptr<Preloader>(new Preloader(
      code,
      builtins,
      globals,
      std::move(annotations),
      fullname,
      std::move(reifier)));
  bool success = preloader->preload();
  JIT_THROW_IF(
      success == static_cast<bool>(PyErr_Occurred()),
      "Expecting Python exception only when preloading fails, preloading "
      "result: {}",
      success);
  if (!success) {
    return nullptr;
  }
#if defined(CINDER_AARCH64)
  preloader->setOSREntryTargetOffsets(collectBackedgeTargetOffsets(code));
#endif
  return preloader;
}

BorrowedRef<PyFunctionObject> InvokeTarget::func() const {
  JIT_THROW_IF(!isFunction(), "InvokeTarget is not a PyFunctionObject");
  return reinterpret_cast<PyFunctionObject*>(callable.get());
}

bool InvokeTarget::isBuiltin() const {
  return builtin_c_func != nullptr;
}

bool InvokeTarget::isFunction() const {
  return PyFunction_Check(callable);
}

const OwnedType* Preloader::preloadedType(BorrowedRef<> descr) const {
  auto it = types_.find(descr);
  return it != types_.end() ? &it->second : nullptr;
}

const FieldInfo* Preloader::fieldInfo(BorrowedRef<> descr) const {
  auto it = fields_.find(descr);
  return it != fields_.end() ? &it->second : nullptr;
}

const InvokeTarget& Preloader::invokeFunctionTarget(BorrowedRef<> descr) const {
  return *(map_get(func_targets_, descr));
}

const InvokeTarget& Preloader::invokeMethodTarget(BorrowedRef<> descr) const {
  return *(map_get(meth_targets_, descr));
}

const NativeTarget& Preloader::invokeNativeTarget(BorrowedRef<> target) const {
  return *(map_get(native_targets_, target));
}

const DescrMap<std::unique_ptr<InvokeTarget>>&
Preloader::invokeFunctionTargets() const {
  return func_targets_;
}

const GlobalNamesMap& Preloader::globalNames() const {
  return global_names_;
}

Type Preloader::checkArgType(int local_idx) const {
  auto it = check_arg_types_.find(local_idx);
  return it != check_arg_types_.end() ? it->second.toHir() : TObject;
}

std::optional<Type> Preloader::inferredSelfType() const {
  if (!inferred_self_type_) {
    return std::nullopt;
  }
  return inferred_self_type_->toHir();
}

PyObject** Preloader::getGlobalCache(BorrowedRef<> name_obj) const {
  JIT_THROW_IF(
      !canCacheGlobals(),
      "Trying to get a globals cache with unwatchable builtins and/or globals "
      "for {}",
      fullname());
  JIT_THROW_IF(
      !PyUnicode_CheckExact(name_obj),
      "Name must be a str, got {}",
      Py_TYPE(name_obj)->tp_name);
  BorrowedRef<PyUnicodeObject> name{name_obj};
  // The manager is allocated for every version that emits LoadGlobalCached
  // and for no version that does not, so a null here means a caller reached
  // this on a branch with no global-cache story.  Fail the compile with the
  // name rather than dereferencing null.
  jit::IGlobalCacheManager* caches =
      cinderx::getModuleState()->cache_manager.get();
  JIT_THROW_IF(
      caches == nullptr,
      "Trying to get a globals cache on a build without a global cache "
      "manager for {}",
      fullname());
  return caches->getGlobalCache(builtins_, globals_, name);
}

bool Preloader::canCacheGlobals() const {
  return hasOnlyUnicodeKeys(builtins_) && hasOnlyUnicodeKeys(globals_);
}

#if PY_VERSION_HEX < 0x030C0000
// Resolve the type that owns this method from its qualname, for the 3.11
// LOAD_METHOD_WITH_VALUES and LOAD_ATTR_INSTANCE_VALUE receiver guards.
// 3.12+ has no consumer for this information and skips the lookup.
void Preloader::preloadMethodOwnerType() {
  const std::string& name = fullname();
  const std::size_t colon = name.find(':');
  if (colon == std::string::npos || colon + 1 == name.size()) {
    return;
  }

  std::string_view qualname{name};
  qualname.remove_prefix(colon + 1);
  if (qualname.find("<locals>") != std::string_view::npos) {
    return;
  }

  const std::size_t dot = qualname.find('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 == qualname.size() ||
      qualname.find('.', dot + 1) != std::string_view::npos) {
    return;
  }

  std::string_view owner_name = qualname.substr(0, dot);
  std::string_view method_name = qualname.substr(dot + 1);
  auto owner_key = Ref<>::steal(
      PyUnicode_FromStringAndSize(owner_name.data(), owner_name.size()));
  if (owner_key == nullptr) {
    PyErr_Clear();
    return;
  }

  PyObject* owner = PyDict_GetItemWithError(globals_, owner_key);
  if (owner == nullptr) {
    if (PyErr_Occurred()) {
      PyErr_Clear();
    }
    return;
  }
  if (!PyType_Check(owner)) {
    return;
  }

  auto method_key = Ref<>::steal(
      PyUnicode_FromStringAndSize(method_name.data(), method_name.size()));
  if (method_key == nullptr) {
    PyErr_Clear();
    return;
  }

  auto owner_type = reinterpret_cast<PyTypeObject*>(owner);
  if (owner_type->tp_dict == nullptr) {
    return;
  }

  PyObject* descr = PyDict_GetItemWithError(owner_type->tp_dict, method_key);
  if (descr == nullptr) {
    if (PyErr_Occurred()) {
      PyErr_Clear();
    }
    return;
  }
  if (!PyFunction_Check(descr)) {
    return;
  }
  auto func = reinterpret_cast<PyFunctionObject*>(descr);
  if (func->func_code != code_) {
    return;
  }

  method_owner_type_ = {Ref<PyTypeObject>::create(owner_type), false, true};
}
#endif // PY_VERSION_HEX < 0x030C0000

BorrowedRef<> Preloader::global(int name_idx) const {
  BorrowedRef<> name = map_get(global_names_, name_idx, nullptr);
  if (name != nullptr && canCacheGlobals()) {
    return *getGlobalCache(name);
  }
  return nullptr;
}

std::unique_ptr<Function> Preloader::makeFunction() const {
  // We touch refcounts of Python objects here, so must serialize
  ThreadedCompileSerialize guard;
  auto irfunc = std::make_unique<Function>();
  irfunc->fullname = fullname_;
  irfunc->setCode(code_);
  irfunc->builtins.reset(builtins_);
  irfunc->globals.reset(globals_);
  irfunc->prim_args_info.reset(prim_args_info_);
  irfunc->return_type = return_type_;
  irfunc->has_primitive_args = hasPrimitiveArgs();
  for (auto& [local, preloaded_type] : check_arg_types_) {
    irfunc->typed_args.emplace_back(
        local,
        preloaded_type.type,
        preloaded_type.optional,
        preloaded_type.exact,
        preloaded_type.toHir());
  }
  return irfunc;
}

BorrowedRef<PyCodeObject> Preloader::code() const {
  return code_;
}

BorrowedRef<PyDictObject> Preloader::globals() const {
  return globals_;
}

BorrowedRef<PyDictObject> Preloader::builtins() const {
  return builtins_;
}

AnnotationIndex* Preloader::annotations() const {
  return annotations_.get();
}

const std::string& Preloader::fullname() const {
  return fullname_;
}

Type Preloader::returnType() const {
  return return_type_;
}

int Preloader::numArgs() const {
  if (code_ == nullptr) {
    // code_ might be null if we parsed from textual ir
    return 0;
  }
  return code_->co_argcount + code_->co_kwonlyargcount +
      bool(code_->co_flags & CO_VARARGS) +
      bool(code_->co_flags & CO_VARKEYWORDS);
}

bool Preloader::hasPrimitiveArgs() const {
  return prim_args_info_ != nullptr;
}

BorrowedRef<> Preloader::reifier() const {
  return reifier_;
}

Preloader::Preloader(
    BorrowedRef<PyCodeObject> code,
    BorrowedRef<PyDictObject> builtins,
    BorrowedRef<PyDictObject> globals,
    std::unique_ptr<AnnotationIndex> annotations,
    const std::string& fullname,
    Ref<> reifier)
    : code_(Ref<>::create(code)),
      builtins_(Ref<>::create(builtins)),
      globals_(Ref<>::create(globals)),
      annotations_(std::move(annotations)),
      fullname_(fullname),
      reifier_(std::move(reifier)) {
  JIT_CHECK(PyCode_Check(code_), "Expected PyCodeObject");
}

BorrowedRef<> Preloader::constArg(BytecodeInstruction& bc_instr) const {
  return PyTuple_GET_ITEM(code_->co_consts, bc_instr.oparg());
}

bool Preloader::preload() {
  bool is_static = code_->co_flags & CI_CO_STATICALLY_COMPILED;
  if (is_static && !preloadStatic()) {
    return false;
  }

  if (!is_static) {
    inferred_self_type_ = infer_method_self_type_candidate(code_, globals_);
  }
#if PY_VERSION_HEX < 0x030C0000
  preloadMethodOwnerType();
#endif

  jit::BytecodeInstructionBlock bc_instrs{code_};
  for (auto bc_instr : bc_instrs) {
    switch (bc_instr.opcode()) {
      case LOAD_GLOBAL: {
#if PY_VERSION_HEX < 0x030C0000
        // 3.11 has no consumer for a global cache in either mode.  The only
        // LOAD_GLOBAL fast path on this branch reads the interpreter's own
        // quickened cache (tryEmitLoadGlobalModuleValue311), and
        // LoadGlobalCached -- the instruction a GlobalCache serves -- is
        // emitted from 3.12 on.  Preloading one here would register a
        // process-lifetime dict watcher for a cache nothing reads, and the
        // module state deliberately allocates no GlobalCacheManager on this
        // branch (_cinderx-lib.cpp), so the call would dereference null.
        //
        // Written as a shadow-mode check this held only while 3.11 meant
        // shadow.  The executing canary mode fell straight through it into
        // that null, and so did the inliner's preload worklist, which walks
        // globalNames() and calls global() for each entry.
        break;
#endif
        if (!canCacheGlobals()) {
          break;
        }
        PyObject* names = code_->co_names;
        Py_ssize_t names_len = PyTuple_Size(names);
        int name_idx = loadGlobalIndex(bc_instr.oparg());
        JIT_THROW_IF(
            name_idx >= names_len,
            "Preloaded LOAD_GLOBAL with index {} for names tuple of length {}",
            name_idx,
            names_len);

        BorrowedRef<> name = PyTuple_GET_ITEM(names, name_idx);
        JIT_THROW_IF(name == nullptr, "Name cannot be null");
        // Make sure the cached value has been loaded and any side effects of
        // loading it (e.g. lazy imports) have been exercised before we create
        // the GlobalCache; otherwise GlobalCache initialization can
        // self-destroy due to side effects of PyDict_GetItem and cause a
        // use-after-free.
        PyObject* global_value = PyDict_GetItemWithError(globals_, name);
        if (!global_value && !PyErr_Occurred()) {
          // It's extremely unlikely that builtins dict could ever contain a
          // lazy import that needs warming up, but since it is technically
          // possible, we may as well go ahead and warm that up too if the key
          // isn't in globals.
          PyDict_GetItemWithError(builtins_, name);
        }
        if (PyErr_Occurred()) {
          return false;
        }
        // The above dict fetches may have had side effects that mean globals
        // are no longer cacheable, so recheck that.
        if (canCacheGlobals()) {
          // We also initialize the GlobalCache here so we don't have to
          // thread-serialize initializing it later (it calls PyDict_GetItem,
          // which can cause data races in multithreaded compile.)
          getGlobalCache(name);
          global_names_.emplace(name_idx, name);
        }
        break;
      }
      case BUILD_CHECKED_LIST:
      case BUILD_CHECKED_MAP: {
        BorrowedRef<> descr = PyTuple_GetItem(constArg(bc_instr), 0);
        OwnedType collection_type = resolve_type_descr(descr);
        JIT_THROW_IF(
            collection_type.type == nullptr,
            "Unknown collection type descr {} during preloading of {}",
            repr(descr),
            fullname());
        types_.emplace(descr, std::move(collection_type));
        break;
      }
      case CAST:
      case LOAD_CLASS:
      case REFINE_TYPE:
      case TP_ALLOC: {
        BorrowedRef<> descr = constArg(bc_instr);
        OwnedType alloc_type = resolve_type_descr(descr);
        JIT_THROW_IF(
            alloc_type.type == nullptr,
            "Unknown {} type descr {} during preloading of {}",
            bc_instr.opcode(),
            repr(descr),
            fullname());
        types_.emplace(descr, std::move(alloc_type));
        break;
      }
      case LOAD_FIELD:
      case STORE_FIELD: {
        BorrowedRef<PyTupleObject> descr(constArg(bc_instr));
        fields_.emplace(descr, resolve_field_descr(descr));
        break;
      }
      case LOAD_METHOD_STATIC:
      case INVOKE_FUNCTION:
      case INVOKE_METHOD: {
        BorrowedRef<> descr = PyTuple_GetItem(constArg(bc_instr), 0);
        auto& map = bc_instr.opcode() == INVOKE_FUNCTION ? func_targets_
                                                         : meth_targets_;
        std::unique_ptr<InvokeTarget> target =
            resolveTargetDescr(descr, bc_instr.opcode());
        if (target) {
          map.emplace(descr, std::move(target));
          break;
        } else {
          return false;
        }
      }
#ifndef WIN32
      case INVOKE_NATIVE: {
        BorrowedRef<> target_descr = PyTuple_GetItem(constArg(bc_instr), 0);
        BorrowedRef<> signature = PyTuple_GetItem(constArg(bc_instr), 1);
        native_targets_.emplace(
            target_descr, resolve_native_target(target_descr, signature));
        break;
      }
#endif
    }
  }

  return true;
}

bool Preloader::preloadStatic() {
  BorrowedRef<> ret_type_descr = _PyClassLoader_GetCodeReturnTypeDescr(code_);
  if (ret_type_descr == nullptr) {
    // Special case where a module's code object is being preloaded.  It will
    // not have argument or return type descrs (and they cannot be added as they
    // interfere with the "<import-from>" list!).
    if (isModuleCodeObject()) {
      return true;
    }

    JIT_THROW(
        "Statically typed function {} has no return type descr, co_consts "
        "is {}",
        fullname(),
        repr(code_->co_consts));
  }

  OwnedType ret_type = resolve_type_descr(ret_type_descr);
  JIT_THROW_IF(
      ret_type.type == nullptr,
      "Unknown return type descr {} during preloading of {}",
      repr(ret_type_descr),
      fullname());

  return_type_ = ret_type.toHir();

  BorrowedRef<PyTupleObject> checks = reinterpret_cast<PyTupleObject*>(
      _PyClassLoader_GetCodeArgumentTypeDescrs(code_));

  bool has_primitive_args = false;
  constexpr Py_ssize_t kMaxLocals = 16384;
  for (int i = 0; i < PyTuple_GET_SIZE(checks); i += 2) {
    Py_ssize_t local = PyLong_AsSsize_t(PyTuple_GET_ITEM(checks, i));
    JIT_THROW_IF(
        local < 0 || local >= kMaxLocals,
        "In Static Python function {}, hit bad local {} at index {}, "
        "arguments checks tuple is {}",
        fullname(),
        local,
        i,
        repr(checks));
    OwnedType preloaded_type =
        resolve_type_descr(PyTuple_GET_ITEM(checks, i + 1));
    JIT_THROW_IF(
        preloaded_type.type == nullptr,
        "Unknown type descr {} during preloading of {}",
        repr(PyTuple_GET_ITEM(checks, i + 1)),
        fullname());
    JIT_THROW_IF(
        preloaded_type.type == reinterpret_cast<PyTypeObject*>(&PyObject_Type),
        "Shouldn't generate type checks for object type, in {} for local {} at "
        "index {}",
        fullname(),
        local,
        i);
    Type type = preloaded_type.toHir();
    check_arg_types_.emplace(local, std::move(preloaded_type));
    if (type <= TPrimitive) {
      has_primitive_args = true;
    }
  }

  if (has_primitive_args) {
    prim_args_info_ = Ref<_PyTypedArgsInfo>::steal(
        _PyClassLoader_GetTypedArgsInfo(code_, true));
  }

  return true;
}

// Check if a code object is for the top-level code in a module.
bool Preloader::isModuleCodeObject() const {
  return fullname().ends_with("<module>") || fullname() == "__main__:__main__";
}

std::unique_ptr<InvokeTarget> Preloader::resolveTargetDescr(
    BorrowedRef<> descr,
    int opcode) {
  auto target = std::make_unique<InvokeTarget>();
  PyObject* container;
  auto callable =
      Ref<>::steal(_PyClassLoader_ResolveFunction(descr, &container));
  JIT_THROW_IF(
      callable == nullptr,
      "Unknown invoke target {} during preloading {}",
      repr(descr),
      fullname());

  int optional, exact, func_flags;
  auto return_pytype =
      Ref<PyTypeObject>::steal(_PyClassLoader_ResolveReturnType(
          callable, &optional, &exact, &func_flags));

  target->container_is_immutable = _PyClassLoader_IsImmutable(container);
  if (return_pytype != nullptr) {
    if (func_flags & Ci_FUNC_FLAGS_COROUTINE) {
      // TODO properly handle coroutine returns awaitable type
      target->return_type = TObject;
    } else {
      OwnedType preloaded_type{
          std::move(return_pytype),
          static_cast<bool>(optional),
          static_cast<bool>(exact)};
      target->return_type = preloaded_type.toHir();
    }
  }
  target->is_statically_typed = _PyClassLoader_IsStaticCallable(callable);
  PyMethodDef* def;
  Ci_PyTypedMethodDef* tmd;
  bool is_thunk = false;
  if (_PyClassLoader_IsPatchedThunk(callable)) {
    is_thunk = true;
  } else if ((def = _PyClassLoader_GetMethodDef(callable)) != nullptr) {
    target->builtin_c_func = reinterpret_cast<void*>(def->ml_meth);
    if (def->ml_flags == METH_NOARGS) {
      target->builtin_expected_nargs = 1;
    } else if (def->ml_flags == METH_O) {
      target->builtin_expected_nargs = 2;
    } else if ((tmd = _PyClassLoader_GetTypedMethodDef(callable))) {
      target->builtin_returns_error_code = (tmd->tmd_ret == Ci_Py_SIG_ERROR);
      target->builtin_returns_void = (tmd->tmd_ret == Ci_Py_SIG_VOID);
      target->builtin_c_func = tmd->tmd_meth;
    }
  }
  target->callable = std::move(callable);

  if (opcode == LOAD_METHOD_STATIC) {
    target->slot = _PyClassLoader_ResolveMethod(descr);
    JIT_THROW_IF(
        target->slot == -1,
        "Method lookup failed for descr {} in function {}",
        repr(descr),
        fullname());
  } else { // the rest of this only used by INVOKE_FUNCTION currently
    if (!target->container_is_immutable) {
      target->indirect_ptr = _PyClassLoader_ResolveIndirectPtr(descr);
      JIT_THROW_IF(
          target->indirect_ptr == nullptr,
          "Indirect ptr null for {} in {} (stale bytecode?)",
          repr(descr),
          fullname());
    }
  }

  if (target->is_statically_typed) {
    if (target->isFunction()) {
      fill_primitive_arg_types_func(
          target->func(), target->primitive_arg_types);
    } else {
      fill_primitive_arg_types_builtin(
          target->callable, target->primitive_arg_types);
    }
  }

  if (is_thunk) {
    fill_primitive_arg_types_thunk(
        target->callable.get(), target->primitive_arg_types, container);
  }

  return target;
}

void PreloaderManager::add(
    BorrowedRef<PyCodeObject> code,
    std::unique_ptr<Preloader> preloader) {
  auto [_, inserted] = preloaders_.emplace(code, std::move(preloader));
  JIT_CHECK(
      inserted,
      "Trying to create a duplicate preloader for {}",
      PyUnicode_AsUTF8(code->co_qualname));
}

Preloader* PreloaderManager::find(BorrowedRef<PyCodeObject> code) {
  auto it = preloaders_.find(code);
  return it != preloaders_.end() ? it->second.get() : nullptr;
}

Preloader* PreloaderManager::find(BorrowedRef<PyFunctionObject> func) {
  BorrowedRef<PyCodeObject> code = func->func_code;
  return find(code);
}

bool PreloaderManager::empty() const {
  return preloaders_.empty();
}

size_t PreloaderManager::size() const {
  return preloaders_.size();
}

void PreloaderManager::clear() {
  preloaders_.clear();
}

bool PreloaderManager::isGlobalManager() const {
  return tls_manager == nullptr;
}

PreloaderManager& preloaderManager() {
  if (tls_manager != nullptr) {
    return *tls_manager;
  }
  return s_manager;
}

IsolatedPreloaders::IsolatedPreloaders() : prev_manager_(tls_manager) {
  tls_manager = &local_manager_;
}

IsolatedPreloaders::~IsolatedPreloaders() {
  tls_manager = prev_manager_;
}

} // namespace jit::hir
