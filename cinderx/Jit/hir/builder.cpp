// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/builder.h"

#include "ceval.h"

#include "cinderx/python_runtime.h"

#if PY_VERSION_HEX >= 0x030C0000
extern "C" {
#include "internal/pycore_intrinsics.h"
#include "internal/pycore_long.h"
#include "internal/pycore_runtime.h"
}
#endif

#include "cinderx/Common/code.h"
#include "cinderx/Common/dict.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Interpreter/cinder_opcode.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/hir/annotation_index.h"
#include "cinderx/Jit/hir/array_specialize.h"
#include "cinderx/Jit/hir/ssa.h"
#include "cinderx/Jit/hir/type.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/threaded_compile.h"
#include "cinderx/StaticPython/checked_dict.h"
#include "cinderx/StaticPython/checked_list.h"
#include "cinderx/StaticPython/classloader.h"
#include "cinderx/StaticPython/static_array.h"
#include "cinderx/StaticPython/typed_method_def.h"
#include "cinderx/module_state.h"
#include "cinderx/python_runtime.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

// Variant of JIT_THROW() that will log the name of the code object and the
// current offset.
#define BUILDER_THROW(MSG, ...)                         \
  JIT_THROW(                                            \
      MSG " in {} at offset {}",                        \
      __VA_ARGS__ __VA_OPT__(, ) preloader_.fullname(), \
      bc_instr.opcodeOffset())

namespace jit::hir {

namespace {

#if PY_VERSION_HEX >= 0x030E0000
PyObject* readCacheObj(
    PyCodeObject* code,
    const BytecodeInstruction& bc_instr,
    int instruction_offset) {
  static_assert(
      sizeof(PyObject*) == 4 * sizeof(_Py_CODEUNIT),
      "object cache slot must be exactly 4 code units");
  PyObject* obj = nullptr;
  std::memcpy(
      &obj,
      &codeUnit(code)[bc_instr.opcodeIndex().value() + instruction_offset]
           .cache,
      sizeof(obj));
  return obj;
}

PyMemberDef* attrSlotMemberDef(
    PyCodeObject* code,
    const BytecodeInstruction& bc_instr,
    int name_idx) {
  uint32_t type_version = bc_instr.attrCacheTypeVersion();
  if (type_version == 0) {
    return nullptr;
  }

  PyInterpreterState* interp = _PyInterpreterState_GET();
  PyTypeObject* type =
      interp->types.type_version_cache[type_version % TYPE_VERSION_CACHE_SIZE];
  if (type == nullptr || type->tp_version_tag != type_version) {
    return nullptr;
  }

  BorrowedRef<> name = PyTuple_GET_ITEM(code->co_names, name_idx);
  BorrowedRef<> descr = _PyType_Lookup(type, name);
  if (descr == nullptr || Py_TYPE(descr) != &PyMemberDescr_Type) {
    return nullptr;
  }

  PyMemberDef* def =
      reinterpret_cast<PyMemberDescrObject*>(descr.get())->d_member;
  if (def->flags & READ_RESTRICTED) {
    return nullptr;
  }
  if (def->type != T_OBJECT && def->type != T_OBJECT_EX) {
    return nullptr;
  }
  if (def->offset != bc_instr.attrCacheIndex()) {
    return nullptr;
  }
  return def;
}

bool attrSlotTypeDefinesGetattr(const BytecodeInstruction& bc_instr) {
  uint32_t type_version = bc_instr.attrCacheTypeVersion();
  if (type_version == 0) {
    return false;
  }

  PyInterpreterState* interp = _PyInterpreterState_GET();
  PyTypeObject* type =
      interp->types.type_version_cache[type_version % TYPE_VERSION_CACHE_SIZE];
  if (type == nullptr || type->tp_version_tag != type_version) {
    return false;
  }
  return _PyType_Lookup(type, &_Py_ID(__getattr__)) != nullptr;
}
#elif PY_VERSION_HEX < 0x030C0000
// The LOAD_ATTR_SLOT fast path reads interp->types.type_version_cache, which
// does not exist on 3.11.  The path is gated off there (slot_fast_path_enabled
// is forced false), so these stubs report "no fast path" and are never
// reached; they exist only to keep the shared LOAD_ATTR_SLOT case compilable.
// A real 3.11 slot fast path is inline-cache work, out of scope here.
PyMemberDef* attrSlotMemberDef(PyCodeObject*, const BytecodeInstruction&, int) {
  return nullptr;
}

bool attrSlotTypeDefinesGetattr(const BytecodeInstruction&) {
  return false;
}
#endif

void rotateStackTop(OperandStack& stack, int count) {
  if (count < 2) {
    return;
  }

  JIT_CHECK(
      stack.size() >= count,
      "Rotate requires {} values, operand stack only has {}",
      count,
      stack.size());

  std::rotate(stack.end() - count, stack.end() - 1, stack.end());
}

// Check that an opcode is one we know how to translate into HIR.
bool isSupportedOpcode(int opcode) {
  switch (opcode) {
#if PY_VERSION_HEX >= 0x030C0000
    case BEFORE_ASYNC_WITH:
#endif
    case BEFORE_WITH:
    case BINARY_ADD:
    case BINARY_AND:
    case BINARY_FLOOR_DIVIDE:
    case BINARY_LSHIFT:
    case BINARY_MATRIX_MULTIPLY:
    case BINARY_MODULO:
    case BINARY_MULTIPLY:
    case BINARY_OP:
    case BINARY_OR:
    case BINARY_POWER:
    case BINARY_RSHIFT:
    case BINARY_SLICE:
    case BINARY_SUBSCR:
    case BINARY_SUBTRACT:
    case BINARY_TRUE_DIVIDE:
    case BINARY_XOR:
    case BUILD_CHECKED_LIST:
    case BUILD_CHECKED_MAP:
    case BUILD_CONST_KEY_MAP:
    case BUILD_LIST:
    case BUILD_MAP:
    case BUILD_SET:
    case BUILD_SLICE:
    case BUILD_STRING:
    case BUILD_INTERPOLATION:
    case BUILD_TEMPLATE:
    case BUILD_TUPLE:
    case CONVERT_VALUE:
    case CALL:
    case CALL_FUNCTION:
    case CALL_FUNCTION_EX:
    case CALL_FUNCTION_KW:
    case CALL_INTRINSIC_1:
    case CALL_INTRINSIC_2:
    case CALL_KW:
    case CALL_METHOD:
    case CAST:
#if PY_VERSION_HEX >= 0x030C0000
    case CHECK_EG_MATCH:
#endif
    case CHECK_EXC_MATCH:
    case CLEANUP_THROW:
    case COMPARE_OP:
    case CONVERT_PRIMITIVE:
    case CONTAINS_OP:
    case COPY:
    case COPY_DICT_WITHOUT_KEYS:
    case COPY_FREE_VARS:
    case DELETE_ATTR:
    case DELETE_FAST:
    case DELETE_SUBSCR:
    case DICT_MERGE:
    case DICT_UPDATE:
    case DUP_TOP:
    case DUP_TOP_TWO:
    case EAGER_IMPORT_NAME:
#if PY_VERSION_HEX >= 0x030C0000
    case END_ASYNC_FOR:
#endif
    case END_FOR:
    case END_SEND:
    case EXTENDED_ARG:
    case FAST_LEN:
    case FORMAT_SIMPLE:
    case FORMAT_VALUE:
    case FORMAT_WITH_SPEC:
    case FOR_ITER:
    case GEN_START:
#if PY_VERSION_HEX >= 0x030C0000
    case GET_AITER:
    case GET_ANEXT:
    case GET_AWAITABLE:
#endif
    case GET_ITER:
    case GET_LEN:
    case GET_YIELD_FROM_ITER:
#if PY_VERSION_HEX >= 0x030E0000 || ENABLE_LAZY_IMPORTS
    case IMPORT_FROM:
      // LIR generation for IMPORT_FROM depends on access to _PyEval_ImportFrom
      // (added in 3.14) or the_PyImport_ImportFrom function that's only added
      // by Lazy Imports.
#endif
    case IMPORT_NAME:
    case INPLACE_ADD:
    case INPLACE_AND:
    case INPLACE_FLOOR_DIVIDE:
    case INPLACE_LSHIFT:
    case INPLACE_MATRIX_MULTIPLY:
    case INPLACE_MODULO:
    case INPLACE_MULTIPLY:
    case INPLACE_OR:
    case INPLACE_POWER:
    case INPLACE_RSHIFT:
    case INPLACE_SUBTRACT:
    case INPLACE_TRUE_DIVIDE:
    case INPLACE_XOR:
    case INVOKE_FUNCTION:
    case INVOKE_METHOD:
    case INVOKE_NATIVE:
    case IS_OP:
    case JUMP_ABSOLUTE:
    case JUMP_BACKWARD:
    case JUMP_BACKWARD_NO_INTERRUPT:
#if PY_VERSION_HEX >= 0x030E0000
    case JUMP_BACKWARD_JIT:
    case JUMP_BACKWARD_NO_JIT:
#endif
    case JUMP_FORWARD:
    case JUMP_IF_FALSE_OR_POP:
    case JUMP_IF_NONZERO_OR_POP:
    case JUMP_IF_NOT_EXC_MATCH:
    case JUMP_IF_TRUE_OR_POP:
    case JUMP_IF_ZERO_OR_POP:
    case KW_NAMES:
    case LIST_APPEND:
    case LIST_EXTEND:
    case LIST_TO_TUPLE:
    case LOAD_ASSERTION_ERROR:
    case LOAD_ATTR:
    case LOAD_ATTR_SUPER:
    case LOAD_BUILD_CLASS:
    case LOAD_CLOSURE:
    case LOAD_COMMON_CONSTANT:
    case LOAD_CONST:
    case LOAD_DEREF:
    case LOAD_FAST:
    case LOAD_FAST_AND_CLEAR:
    case LOAD_FAST_BORROW:
    case LOAD_FAST_BORROW_LOAD_FAST_BORROW:
    case LOAD_FAST_LOAD_FAST:
    case LOAD_FAST_CHECK:
    case LOAD_FIELD:
    case LOAD_GLOBAL:
    case LOAD_ITERABLE_ARG:
    case LOAD_LOCAL:
    case LOAD_METHOD:
    case LOAD_METHOD_STATIC:
    case LOAD_METHOD_SUPER:
    case LOAD_SMALL_INT:
    case LOAD_SPECIAL:
    case LOAD_SUPER_ATTR:
    case LOAD_TYPE:
    case MAKE_CELL:
    case MAKE_FUNCTION:
    case MAP_ADD:
#if PY_VERSION_HEX >= 0x030C0000
    case MATCH_CLASS:
    case MATCH_KEYS:
    case MATCH_MAPPING:
    case MATCH_SEQUENCE:
#endif
    case NOP:
    case NOT_TAKEN:
    case POP_BLOCK:
    case POP_EXCEPT:
    case POP_ITER:
#if PY_VERSION_HEX < 0x030C0000
    case POP_JUMP_BACKWARD_IF_FALSE:
    case POP_JUMP_BACKWARD_IF_NONE:
    case POP_JUMP_BACKWARD_IF_NOT_NONE:
    case POP_JUMP_BACKWARD_IF_TRUE:
    case POP_JUMP_FORWARD_IF_FALSE:
    case POP_JUMP_FORWARD_IF_NONE:
    case POP_JUMP_FORWARD_IF_NOT_NONE:
    case POP_JUMP_FORWARD_IF_TRUE:
#endif
    case POP_JUMP_IF_FALSE:
    case POP_JUMP_IF_NONE:
    case POP_JUMP_IF_NONZERO:
    case POP_JUMP_IF_NOT_NONE:
    case POP_JUMP_IF_TRUE:
    case POP_JUMP_IF_ZERO:
    case POP_TOP:
#if PY_VERSION_HEX < 0x030C0000
    case PRECALL:
#endif
    case PRIMITIVE_BINARY_OP:
    case PRIMITIVE_BOX:
    case PRIMITIVE_COMPARE_OP:
    case PRIMITIVE_LOAD_CONST:
    case PRIMITIVE_UNARY_OP:
    case PRIMITIVE_UNBOX:
    case PUSH_EXC_INFO:
    case PUSH_NULL:
    case RAISE_VARARGS:
    case REFINE_TYPE:
    case RERAISE:
    case RESUME:
    case RETURN_CONST:
    case RETURN_GENERATOR:
    case RETURN_PRIMITIVE:
    case RETURN_VALUE:
    case ROT_FOUR:
    case ROT_N:
    case ROT_THREE:
    case ROT_TWO:
    case SEND:
    case SEQUENCE_GET:
    case SEQUENCE_SET:
    case SET_ADD:
    case SET_FUNCTION_ATTRIBUTE:
    case SET_UPDATE:
    case SETUP_ASYNC_WITH:
    case SETUP_FINALLY:
    case SETUP_WITH:
    case STORE_ATTR:
    case STORE_DEREF:
    case STORE_FAST:
    case STORE_FAST_LOAD_FAST:
    case STORE_FAST_STORE_FAST:
    case STORE_FIELD:
    case STORE_GLOBAL:
    case STORE_LOCAL:
    case STORE_SLICE:
    case STORE_SUBSCR:
    case SWAP:
    case TO_BOOL:
    case TP_ALLOC:
    case UNARY_INVERT:
    case UNARY_NEGATIVE:
    case UNARY_NOT:
    case UNARY_POSITIVE:
    case UNPACK_EX:
    case UNPACK_SEQUENCE:
    case WITH_EXCEPT_START:
    case YIELD_FROM:
    case YIELD_VALUE:
      return true;
    default:
      break;
  }
  return false;
}

// Check that a symbol/name is one that the JIT has banned.
bool isBannedName(std::string_view name) {
  return name == "eval" || name == "exec" || name == "locals";
}

#if PY_VERSION_HEX < 0x030C0000
// co_names entries are Unicode objects, but CodeType.replace() can legally
// insert a lone surrogate. PyUnicode_AsUTF8 then returns NULL and sets
// UnicodeEncodeError; constructing a string_view from that pointer is UB.
std::string_view
nameAtOrRefuse(PyObject* names, Py_ssize_t i, const char** refuse) {
  if (*refuse != nullptr) {
    return {};
  }
  PyObject* item = PyTuple_GET_ITEM(names, i);
  const char* utf8 = item != nullptr ? PyUnicode_AsUTF8(item) : nullptr;
  if (utf8 == nullptr) {
    PyErr_Clear();
    *refuse = "REFUSE_SHAPE_INVALID_UTF8_NAME";
    return {};
  }
  return std::string_view(utf8);
}
#endif

// True if the code object contains any branch back to an earlier instruction;
// used as a structural proxy for "this function has a loop body" when gating
// exact-int guard emission for specialized numeric opcodes.
bool codeHasBackedge(BorrowedRef<PyCodeObject> code) {
  for (const auto& bc_instr : BytecodeInstructionBlock{code}) {
    if (bc_instr.isBranch() &&
        bc_instr.getJumpTarget() <= bc_instr.baseOffset()) {
      return true;
    }
  }
  return false;
}

bool registerHasTypeEvidence(Register* reg, Type type) {
  if (reg->isA(type)) {
    return true;
  }

  auto* instr = reg->instr();
  if (instr == nullptr || !instr->IsLoadConst()) {
    return false;
  }

  return static_cast<const LoadConst*>(instr)->type() <= type;
}

bool hasArraySubscrStoreFastPathEvidence(
    Register* container,
    Register* sub,
    Register* value,
    Type array_type) {
  return container->isA(array_type) ||
      (registerHasTypeEvidence(sub, TLongExact) &&
       registerHasTypeEvidence(value, TFloatExact));
}
struct LoadSuperAttrPattern311 {
  int global_super_idx;
  int type_global_idx;
  int receiver_local_idx;
  int name_idx;
  bool load_method;
  bool no_args_in_super_call;
  int instrs_to_skip_after_super;
};

std::optional<LoadSuperAttrPattern311> matchLoadSuperAttrPattern311(
    BorrowedRef<PyCodeObject> code,
    BytecodeInstructionBlock::Iterator bc_it,
    const BytecodeInstructionBlock& bc_block) {
#if PY_VERSION_HEX < 0x030C0000
  BytecodeInstruction load_global = *bc_it;
  if (load_global.opcode() != LOAD_GLOBAL || !(load_global.oparg() & 1)) {
    return std::nullopt;
  }

  int global_idx = loadGlobalIndex(load_global.oparg());
  PyObject* global_name = PyTuple_GET_ITEM(code->co_names, global_idx);
  if (PyUnicode_CompareWithASCIIString(global_name, "super") != 0) {
    return std::nullopt;
  }

  ++bc_it;
  if (bc_it == bc_block.end()) {
    return std::nullopt;
  }
  BytecodeInstruction next = *bc_it;

  bool no_args_in_super_call = true;
  int type_global_idx = -1;
  int receiver_local_idx = -1;
  int instrs_to_skip_after_super = 3;

  if (next.opcode() == LOAD_GLOBAL) {
    type_global_idx = loadGlobalIndex(next.oparg());

    ++bc_it;
    if (bc_it == bc_block.end()) {
      return std::nullopt;
    }
    BytecodeInstruction receiver = *bc_it;
    if (receiver.opcode() != LOAD_FAST) {
      return std::nullopt;
    }
    receiver_local_idx = receiver.oparg();

    ++bc_it;
    if (bc_it == bc_block.end()) {
      return std::nullopt;
    }
    next = *bc_it;
    no_args_in_super_call = false;
    instrs_to_skip_after_super = 5;
  }

  if (next.opcode() != PRECALL ||
      next.oparg() != (no_args_in_super_call ? 0 : 2)) {
    return std::nullopt;
  }

  ++bc_it;
  if (bc_it == bc_block.end()) {
    return std::nullopt;
  }
  BytecodeInstruction call = *bc_it;
  if (call.opcode() != CALL ||
      call.oparg() != (no_args_in_super_call ? 0 : 2)) {
    return std::nullopt;
  }

  ++bc_it;
  if (bc_it == bc_block.end()) {
    return std::nullopt;
  }
  BytecodeInstruction load_attr_or_method = *bc_it;
  int opcode = load_attr_or_method.opcode();
  if (opcode != LOAD_ATTR && opcode != LOAD_METHOD) {
    return std::nullopt;
  }

  return LoadSuperAttrPattern311{
      global_idx,
      type_global_idx,
      receiver_local_idx,
      loadAttrIndex(load_attr_or_method.oparg()),
      opcode == LOAD_METHOD,
      no_args_in_super_call,
      instrs_to_skip_after_super};
#else
  return std::nullopt;
#endif
}

#if PY_VERSION_HEX < 0x030C0000
uint32_t readCacheU32(const _Py_CODEUNIT* cache) {
  return static_cast<uint32_t>(cache[0]) |
      (static_cast<uint32_t>(cache[1]) << 16);
}

PyObject* readCacheObj(const _Py_CODEUNIT* cache) {
  uintptr_t result = 0;
  for (std::size_t i = 0; i < sizeof(PyObject*) / sizeof(_Py_CODEUNIT); ++i) {
    result |= static_cast<uintptr_t>(cache[i]) << (16 * i);
  }
  return reinterpret_cast<PyObject*>(result);
}

Py_ssize_t findActiveUnicodeDictEntryIndex(
    BorrowedRef<PyDictObject> dict,
    BorrowedRef<> name,
    BorrowedRef<> expected_value) {
  JIT_DCHECK(dict->ma_values == nullptr, "expected a combined dict");
  JIT_DCHECK(hasOnlyUnicodeKeys(dict), "expected unicode keys");

  PyDictKeysObject* keys = dict->ma_keys;
  PyDictUnicodeEntry* entries = DK_UNICODE_ENTRIES(keys);
  for (Py_ssize_t i = 0; i < keys->dk_nentries; ++i) {
    PyDictUnicodeEntry* entry = &entries[i];
    if (entry->me_key == nullptr || entry->me_value == nullptr) {
      continue;
    }
    if (entry->me_key == name) {
      return entry->me_value == expected_value ? i : -1;
    }
    int equal = PyObject_RichCompareBool(entry->me_key, name, Py_EQ);
    if (equal < 0) {
      PyErr_Clear();
      return -1;
    }
    if (equal) {
      return entry->me_value == expected_value ? i : -1;
    }
  }
  return -1;
}

BorrowedRef<PyTypeObject> resolveMethodOwnerType(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<PyDictObject> globals) {
  BorrowedRef<> qualname{func->func_qualname};
  if (!PyUnicode_Check(qualname)) {
    return nullptr;
  }

  Py_ssize_t qualname_size = 0;
  const char* qualname_data = PyUnicode_AsUTF8AndSize(qualname, &qualname_size);
  if (qualname_data == nullptr) {
    PyErr_Clear();
    return nullptr;
  }

  std::string_view qualname_view{
      qualname_data, static_cast<std::size_t>(qualname_size)};
  if (qualname_view.find("<locals>") != std::string_view::npos) {
    return nullptr;
  }

  const std::size_t dot = qualname_view.find('.');
  if (dot == std::string_view::npos || dot == 0 ||
      dot + 1 == qualname_view.size() ||
      qualname_view.find('.', dot + 1) != std::string_view::npos) {
    return nullptr;
  }

  std::string_view owner_name = qualname_view.substr(0, dot);
  std::string_view method_name = qualname_view.substr(dot + 1);
  auto owner_key = Ref<>::steal(
      PyUnicode_FromStringAndSize(owner_name.data(), owner_name.size()));
  auto method_key = Ref<>::steal(
      PyUnicode_FromStringAndSize(method_name.data(), method_name.size()));
  if (owner_key == nullptr || method_key == nullptr) {
    PyErr_Clear();
    return nullptr;
  }

  ThreadedCompileSerialize guard;
  PyObject* owner = PyDict_GetItemWithError(globals, owner_key);
  if (owner == nullptr) {
    if (PyErr_Occurred()) {
      PyErr_Clear();
    }
    return nullptr;
  }
  if (!PyType_Check(owner)) {
    return nullptr;
  }

  auto owner_type = reinterpret_cast<PyTypeObject*>(owner);
  if (owner_type->tp_dict == nullptr) {
    return nullptr;
  }

  PyObject* descr = PyDict_GetItemWithError(owner_type->tp_dict, method_key);
  if (descr == nullptr) {
    if (PyErr_Occurred()) {
      PyErr_Clear();
    }
    return nullptr;
  }
  if (descr != func) {
    return nullptr;
  }

  return owner_type;
}
#endif

} // namespace

// Get the array.array type object for fast path type guards.
// Pre-cached with runtime layout validation. The cached value is readable
// by any thread once initialized; only the initialization path requires
// the GIL (and is skipped on worker threads).
PyTypeObject* getStdlibArrayType() {
  static PyTypeObject* cached_type = nullptr;
  if (cached_type != nullptr) {
    return cached_type;
  }

  // Initialization needs the GIL (PyImport_ImportModule etc.).
  // Worker threads skip initialization and return nullptr; the fast path
  // is only available for functions compiled on the main thread.
  RETURN_MULTITHREADED_COMPILE(nullptr);

  auto module = Ref<>::steal(PyImport_ImportModule("array"));
  if (module == nullptr) {
    PyErr_Clear();
    return nullptr;
  }

  auto array_type = Ref<>::steal(PyObject_GetAttrString(module, "array"));
  if (array_type == nullptr) {
    PyErr_Clear();
    return nullptr;
  }

  if (!PyType_Check(array_type)) {
    return nullptr;
  }

  auto* type = reinterpret_cast<PyTypeObject*>(array_type.get());

  // Runtime layout validation: create a probe instance and verify offsets.
  auto probe_list = Ref<>::steal(PyList_New(1));
  if (probe_list == nullptr) {
    PyErr_Clear();
    return nullptr;
  }
  auto float_val = Ref<>::steal(PyFloat_FromDouble(1.5));
  if (float_val == nullptr) {
    PyErr_Clear();
    return nullptr;
  }
  PyList_SET_ITEM(probe_list, 0, float_val.release());

  auto d_str = Ref<>::steal(PyUnicode_InternFromString("d"));
  if (d_str == nullptr) {
    PyErr_Clear();
    return nullptr;
  }
  auto args = Ref<>::steal(PyTuple_Pack(2, d_str.get(), probe_list.get()));
  if (args == nullptr) {
    PyErr_Clear();
    return nullptr;
  }

  auto probe = Ref<>::steal(PyObject_CallObject(array_type, args));
  if (probe == nullptr) {
    PyErr_Clear();
    return nullptr;
  }

  auto* arr = reinterpret_cast<StdlibArrayObject*>(probe.get());

  // Verify ob_item points to valid double data
  if (arr->ob_item == nullptr) {
    return nullptr;
  }

  // Verify ob_descr->typecode == 'd'
  if (arr->ob_descr == nullptr || arr->ob_descr->typecode != 'd') {
    return nullptr;
  }

  // Verify we can actually read the stored value
  double val = *reinterpret_cast<double*>(arr->ob_item);
  if (val != 1.5) {
    return nullptr;
  }

  // Transfer ownership: array_type's Ref<> will decref on scope exit,
  // so we must incref to keep the type alive for the static cache.
  Py_INCREF(type);
  cached_type = type;
  return cached_type;
}

// Allocate a temp register that may be used for the stack. It should not be a
// register that will be treated specially in the FrameState (e.g. tracked as
// containing a local or cell.)
Register* TempAllocator::AllocateStack() {
  Register* reg = env_->AllocateRegister();
  cache_.emplace_back(reg);
  return reg;
}

// Get the i-th stack temporary or allocate one.
Register* TempAllocator::GetOrAllocateStack(std::size_t idx) {
  if (idx < cache_.size()) {
    Register* reg = cache_[idx];
    return reg;
  }
  return AllocateStack();
}

// Allocate a temp register that will not be used for a stack value.
Register* TempAllocator::AllocateNonStack() {
  return env_->AllocateRegister();
}

void HIRBuilder::allocateLocalsplus(Environment* env, FrameState& state) {
  int nlocalsplus = numLocalsplus(code_);
  state.localsplus.clear();
  state.localsplus.reserve(nlocalsplus);
  for (int i = 0; i < nlocalsplus; ++i) {
    state.localsplus.emplace_back(env->AllocateRegister());
  }

  state.nlocals = numLocals(code_);
}

// Holds the current state of translation for a given basic block
struct HIRBuilder::TranslationContext {
  TranslationContext(BasicBlock* b, const FrameState& fs)
      : block(b), frame(fs) {}

  template <typename T, typename... Args>
  T* emit(Args&&... args) {
    auto instr = block->appendWithOff<T>(
        frame.instrOffset(), std::forward<Args>(args)...);
    return instr;
  }

  template <typename T, typename... Args>
  T* emitChecked(Args&&... args) {
    auto instr = emit<T>(std::forward<Args>(args)...);
    auto out = instr->output();
    emit<CheckExc>(out, out, frame);
    return instr;
  }

  template <typename T, typename... Args>
  T* emitVariadic(
      TempAllocator& temps,
      std::size_t num_operands,
      Args&&... args) {
    Register* out = temps.AllocateStack();
    auto call = emit<T>(num_operands, out, std::forward<Args>(args)...);
    for (auto i = num_operands; i > 0; i--) {
      Register* operand = frame.stack.pop();
      call->SetOperand(i - 1, operand);
    }
    call->setFrameState(frame);
    frame.stack.push(out);
    return call;
  }

  void emitSnapshot() {
    emit<Snapshot>(frame);
  }

  BasicBlock* block{nullptr};
  FrameState frame;
};

void HIRBuilder::addInitialYield(TranslationContext& tc) {
  auto out = temps_.AllocateNonStack();
  tc.emit<InitialYield>(out, tc.frame);
}

// Add LoadArg instructions for each function argument. This ensures that the
// corresponding variables are always assigned and allows for a uniform
// treatment of registers that correspond to arguments (vs locals) during
// definite assignment analysis.
void HIRBuilder::addLoadArgs(TranslationContext& tc, int num_args) {
  PyCodeObject* code = tc.frame.code;
  int starargs_idx = (code->co_flags & CO_VARARGS)
      ? code->co_argcount + code->co_kwonlyargcount
      : -1;
  for (int i = 0; i < num_args; i++) {
    // Arguments in CPython are the first N locals.
    Register* dst = tc.frame.localsplus[i];
    JIT_CHECK(dst != nullptr, "No register for argument {}", i);
    if (i == starargs_idx) {
      tc.emit<LoadArg>(dst, i, TTupleExact);
    } else {
      Type type = preloader_.checkArgType(i);
      tc.emit<LoadArg>(dst, i, type);
    }
  }
}

static bool should_snapshot(
    const BytecodeInstruction& bci,
    bool is_in_async_for_header_block) {
  // Taking a snapshot after a terminator doesn't make sense, as control either
  // transfers to another basic block or the function ends.
  if (bci.isTerminator()) {
    return false;
  }

#if PY_VERSION_HEX < 0x030C0000
  // The executing mode snapshots every non-terminator boundary.  The skip
  // list below is a metadata-size optimization: replay-safe instructions
  // do not need a resume point of their own.  The instrumentation polls
  // change the calculus -- they live on boundaries, and a boundary without
  // a snapshot is a boundary the poll pass cannot guard.  The gap is not
  // theoretical: a value released at such a boundary (POP_TOP's operand,
  // a STORE_FAST's previous value) runs its __del__ AFTER the last polled
  // boundary, and a profile function registered there would never hear
  // from the running frame again.  Metadata size is the wrong thing to
  // save in a milestone whose subject is correctness.
  if (getConfig().state == State::kRunning) {
    return true;
  }
#endif

  switch (bci.opcode()) {
    // These instructions only modify frame state and are always safe to
    // replay. We don't snapshot these in order to limit the amount of
    // unnecessary metadata in the lowered IR.
    case CONVERT_PRIMITIVE:
    case COPY:
    case DUP_TOP_TWO:
    case DUP_TOP:
    case END_FOR:
    case EXTENDED_ARG:
    case IS_OP:
    case KW_NAMES:
    case LOAD_ASSERTION_ERROR:
    case LOAD_CLOSURE:
    case LOAD_CONST:
    case LOAD_FAST_AND_CLEAR:
    case LOAD_FAST_BORROW_LOAD_FAST_BORROW:
    case LOAD_FAST_BORROW:
    case LOAD_FAST_CHECK:
    case LOAD_FAST_LOAD_FAST:
    case LOAD_FAST:
    case LOAD_LOCAL:
    case NOP:
    case POP_ITER:
    case POP_TOP:
    case PRIMITIVE_BOX:
    case PRIMITIVE_LOAD_CONST:
    case PRIMITIVE_UNARY_OP:
    case PRIMITIVE_UNBOX:
    case PUSH_NULL:
    case REFINE_TYPE:
    case ROT_FOUR:
    case ROT_N:
    case ROT_THREE:
    case ROT_TWO:
    case STORE_FAST_LOAD_FAST:
    case STORE_FAST_STORE_FAST:
    case STORE_FAST:
    case STORE_LOCAL:
    case SWAP: {
      return false;
    }
    // In an async-for header block YIELD_FROM controls whether we end the loop
    case YIELD_FROM: {
      return !is_in_async_for_header_block;
    }
    case JUMP_IF_NOT_EXC_MATCH:
    case RERAISE:
    case WITH_EXCEPT_START: {
      // Exception-handler opcodes are translated when they appear on a
      // reachable CFG edge.  Snapshotting after them is unnecessary: RERAISE
      // terminates, and WITH_EXCEPT_START / JUMP_IF_NOT_EXC_MATCH either
      // branch or are followed by a deopt-shaped recovery block.
      return false;
    }
    // Take a snapshot after translating all other bytecode instructions. This
    // may generate unnecessary deoptimization metadata but will always be
    // correct.
    default: {
      return true;
    }
  }
}

static bool isEntrySetupInstr(
    const BytecodeInstruction& bci,
    bool initial_yield_value_on_stack) {
  switch (bci.opcode()) {
    case COPY_FREE_VARS:
    case GEN_START:
    case MAKE_CELL:
    case NOP:
    case NOT_TAKEN:
    case RESUME:
    case RETURN_GENERATOR:
      return true;
    case POP_TOP:
      return initial_yield_value_on_stack;
    default:
      return false;
  }
}

// Compute basic block boundaries and allocate corresponding HIR blocks
HIRBuilder::BlockMap HIRBuilder::createBlocks(
    Function& irfunc,
    const BytecodeInstructionBlock& bc_block) {
  BlockMap block_map;

  // Mark the beginning of each basic block in the bytecode
  std::set<BCIndex> block_starts = {BCIndex{0}};
  auto maybe_add_next_instr = [&](const BytecodeInstruction& bc_instr) {
    BCIndex next_instr_idx = bc_instr.nextInstrOffset();
    if (next_instr_idx < bc_block.size()) {
      block_starts.insert(next_instr_idx);
    }
  };
  for (auto bc_instr : bc_block) {
    if (bc_instr.isBranch()) {
      maybe_add_next_instr(bc_instr);
      BCIndex target = bc_instr.getJumpTarget();
      block_starts.insert(target);
    } else {
      auto opcode = bc_instr.opcode();
      if (
          // We always split after YIELD_FROM to handle the case where it's the
          // top of an async-for loop and so generate a HIR conditional jump.
          bc_instr.isTerminator() || (opcode == YIELD_FROM)) {
        maybe_add_next_instr(bc_instr);
      } else {
        JIT_CHECK(!bc_instr.isTerminator(), "Terminator should split block");
      }
    }
  }

  // Allocate blocks
  auto it = block_starts.begin();
  while (it != block_starts.end()) {
    BCIndex start_idx = *it;
    ++it;
    BCIndex end_idx;
    if (it != block_starts.end()) {
      end_idx = *it;
    } else {
      end_idx = BCIndex{bc_block.size()};
    }
    auto block = irfunc.cfg.AllocateBlock();
    block_map.blocks[start_idx] = block;
    block_map.bc_blocks.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(block),
        std::forward_as_tuple(bc_block.code(), start_idx, end_idx));
  }

  return block_map;
}

BasicBlock* HIRBuilder::getBlockAtOff(BCOffset off) {
  auto it = block_map_.blocks.find(off);
  JIT_DCHECK(it != block_map_.blocks.end(), "No block for offset {}", off);
  return it->second;
}

bool HIRBuilder::isSimpleLeafFunction(BorrowedRef<PyCodeObject> code) {
  if (code->co_flags & kCoFlagsAnyGenerator) {
    return false;
  }
  for (auto& instr : BytecodeInstructionBlock{code}) {
    switch (instr.opcode()) {
      case COPY:
      case LOAD_CONST:
      case LOAD_FAST:
      case LOAD_FAST_AND_CLEAR:
      case LOAD_FAST_BORROW:
      case LOAD_FAST_BORROW_LOAD_FAST_BORROW:
      case LOAD_FAST_CHECK:
      case LOAD_FAST_LOAD_FAST:
      case NOP:
      case NOT_TAKEN:
      case POP_TOP:
      case PUSH_NULL:
      case RESUME:
      case RETURN_CONST:
      case RETURN_VALUE:
      case STORE_FAST:
      case STORE_FAST_LOAD_FAST:
      case STORE_FAST_STORE_FAST:
      case SWAP:
        break;
      default:
        return false;
    }
    if (instr.isBackwardBranch()) {
      return false;
    }
  }
  return true;
}

bool HIRBuilder::isSimpleNumericLeafFunction(BorrowedRef<PyCodeObject> code) {
  if (code->co_flags & kCoFlagsAnyGenerator) {
    return false;
  }

  int numeric_binary_op_count = 0;
  for (auto& instr : BytecodeInstructionBlock{code}) {
    if (instr.isBackwardBranch()) {
      return false;
    }

    switch (instr.opcode()) {
      case BINARY_OP:
        switch (instr.specializedOpcode()) {
          case BINARY_OP_ADD_INT:
          case BINARY_OP_MULTIPLY_INT:
          case BINARY_OP_SUBTRACT_INT:
          case BINARY_OP_ADD_FLOAT:
          case BINARY_OP_MULTIPLY_FLOAT:
          case BINARY_OP_SUBTRACT_FLOAT:
            numeric_binary_op_count++;
            break;
          default:
            break;
        }
        break;
      case COMPARE_OP:
      case COPY:
      case LOAD_CONST:
      case LOAD_FAST:
      case LOAD_FAST_AND_CLEAR:
      case LOAD_FAST_BORROW:
      case LOAD_FAST_BORROW_LOAD_FAST_BORROW:
      case LOAD_FAST_CHECK:
      case LOAD_FAST_LOAD_FAST:
      case LOAD_SMALL_INT:
      case NOP:
      case NOT_TAKEN:
      case POP_TOP:
      case PUSH_NULL:
      case RESUME:
      case RETURN_CONST:
      case RETURN_VALUE:
      case STORE_FAST:
      case STORE_FAST_LOAD_FAST:
      case STORE_FAST_STORE_FAST:
      case SWAP:
        break;
      default:
        return false;
    }
  }
  return numeric_binary_op_count > 1;
}

#if PY_VERSION_HEX < 0x030C0000
static std::optional<InPlaceOpKind> getInPlaceOpKindFromOparg(int oparg);

#if PY_VERSION_HEX >= 0x030B0000
static bool hasIntConstLocalBinaryAccumulator311(
    BorrowedRef<PyCodeObject> code) {
  constexpr int kMaxCompactAccumulatorLocalsPlus = 8;
  BytecodeInstructionBlock bc_instrs{code};
  for (auto it = bc_instrs.begin(); it != bc_instrs.end(); ++it) {
    BytecodeInstruction load = *it;
    if (load.opcode() != LOAD_FAST) {
      continue;
    }

    auto const_it = it;
    ++const_it;
    if (const_it == bc_instrs.end()) {
      return false;
    }
    BytecodeInstruction load_const = *const_it;
    if (load_const.opcode() != LOAD_CONST ||
        load_const.oparg() >= PyTuple_GET_SIZE(code->co_consts)) {
      continue;
    }
    BorrowedRef<> const_value =
        PyTuple_GET_ITEM(code->co_consts, load_const.oparg());
    if (!PyLong_CheckExact(const_value)) {
      continue;
    }

    auto binary_it = const_it;
    ++binary_it;
    if (binary_it == bc_instrs.end()) {
      return false;
    }
    BytecodeInstruction binary = *binary_it;
    if (binary.opcode() != BINARY_OP) {
      continue;
    }
    auto inplace_op = getInPlaceOpKindFromOparg(binary.oparg());
    if (!inplace_op.has_value()) {
      continue;
    }

    auto store_it = binary_it;
    ++store_it;
    if (store_it == bc_instrs.end()) {
      return false;
    }
    BytecodeInstruction store = *store_it;
    if (store.opcode() == STORE_FAST && store.oparg() == load.oparg()) {
      if ((*inplace_op == InPlaceOpKind::kAdd ||
           *inplace_op == InPlaceOpKind::kSubtract) &&
          numLocalsplus(code) <= kMaxCompactAccumulatorLocalsPlus) {
        continue;
      }
      return true;
    }
  }
  return false;
}
#endif

#endif

const char* unsupportedShapeReason311(BorrowedRef<PyCodeObject> code) {
#if PY_VERSION_HEX >= 0x030B0000 && PY_VERSION_HEX < 0x030C0000
  // AArch64 cond branches are ±1MiB. test_compile.test_extended_arg builds a
  // 150KiB co_code whose shadow machine code cannot relocate
  // (RelocOffsetOutOfRange). 64KiB bytecode is already outside any stdlib
  // function observed on the 440-module surface.
  constexpr Py_ssize_t kMaxShadowBytecodeBytes311 = 65536;
  if (_PyCode_NBYTES(code) > kMaxShadowBytecodeBytes311) {
    return "REFUSE_SHAPE_CODEGEN_SPAN";
  }
  if (hasIntConstLocalBinaryAccumulator311(code)) {
    return "REFUSE_SHAPE_INT_ACCUMULATOR_POLICY";
  }
  PyObject* names = code->co_names;
  if (names == nullptr || !PyTuple_Check(names)) {
    return "REFUSE_SHAPE_INVALID_UTF8_NAME";
  }
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(names); i++) {
    PyObject* item = PyTuple_GET_ITEM(names, i);
    if (item == nullptr || PyUnicode_AsUTF8(item) == nullptr) {
      PyErr_Clear();
      return "REFUSE_SHAPE_INVALID_UTF8_NAME";
    }
  }
#else
  (void)code;
#endif
  return nullptr;
}

const char* unsupportedOpcodeReason311(BorrowedRef<PyCodeObject> code) {
#if PY_VERSION_HEX < 0x030C0000
  PyObject* names = code->co_names;
  const char* name_refuse = nullptr;
  auto name_at = [&](Py_ssize_t i) {
    return nameAtOrRefuse(names, i, &name_refuse);
  };
  BytecodeInstructionBlock bc_instrs{code};
  for (auto bc_it = bc_instrs.begin(); bc_it != bc_instrs.end(); ++bc_it) {
    switch (bc_it->opcode()) {
      case CHECK_EG_MATCH:
      case PREP_RERAISE_STAR:
        return "REFUSE_EXCEPT_STAR_UNAUDITED";
      case MATCH_CLASS:
      case MATCH_KEYS:
      case MATCH_MAPPING:
      case MATCH_SEQUENCE:
        return "REFUSE_PATTERN_MATCHING_UNAUDITED";
      case IMPORT_FROM:
        return "REFUSE_HELPER_UNAVAILABLE_PRE314";
      case DELETE_DEREF:
      case DELETE_GLOBAL:
        return "REFUSE_UNPORTED";
      case ASYNC_GEN_WRAP:
      case BEFORE_ASYNC_WITH:
      case END_ASYNC_FOR:
      case GET_AITER:
      case GET_ANEXT:
      case GET_AWAITABLE:
        // These opcodes are interpreter-only because they implement async
        // execution. They still appear in *synchronous* factories that
        // construct async genexpressions, so eligibility must return the
        // registered async reason instead of SUPPORTED_OPCODE_FAILURE.
        return "INTERP_ONLY_ASYNC_CODE";
      case DELETE_NAME:
      case IMPORT_STAR:
      case LOAD_CLASSDEREF:
      case LOAD_NAME:
      case PRINT_EXPR:
      case SETUP_ANNOTATIONS:
      case STORE_NAME:
        return "INTERP_ONLY_NON_FUNCTION_SCOPE";
      case CACHE:
      case DO_TRACING:
        return "INTERP_ONLY_PSEUDO_SLOT";
      default:
        if (!isSupportedOpcode(bc_it->opcode())) {
          // Reachable only for an opcode with no support-list row, or a
          // refuse/interpreter-only row missing from this switch.
          return "SUPPORTED_OPCODE_FAILURE";
        }
        if (bc_it->opcode() == LOAD_GLOBAL) {
          int oparg = bc_it->oparg();
          auto loaded = name_at(oparg >> 1);
          if (name_refuse != nullptr) {
            return name_refuse;
          }
          if ((oparg & 0x01) && loaded == "super" &&
              !matchLoadSuperAttrPattern311(code, bc_it, bc_instrs)
                   .has_value()) {
            return "REFUSE_SHAPE_FRONTEND_POLICY";
          }
          if (isBannedName(loaded)) {
            return "REFUSE_SHAPE_FRONTEND_POLICY";
          }
        }
        break;
    }
  }
#else
  (void)code;
#endif
  return nullptr;
}

const char* unsupportedExecuteReason311(BorrowedRef<PyCodeObject> code) {
#if PY_VERSION_HEX < 0x030C0000
  // The MR-04 execute surface: machine code may only run for functions
  // whose every instruction sits inside this audited whitelist --
  // positional/local data flow, basic arithmetic and comparisons, control
  // flow, and iteration over already-materialized iterables.  Everything
  // the plan defers stays out by construction: no CALL family, no attr or
  // subscript ICs, no LOAD_GLOBAL (3.11 has no global-cache story yet),
  // no cells/closures, no exception table, no generators.  The decoder
  // yields unspecialized opcodes, so quickened forms cannot slip past.
  BytecodeInstructionBlock bc_instrs{code};
  for (auto bc_it = bc_instrs.begin(); bc_it != bc_instrs.end(); ++bc_it) {
    switch (bc_it->opcode()) {
      case BINARY_OP:
      case COMPARE_OP:
      case CONTAINS_OP:
      case COPY:
      case DELETE_FAST:
      case EXTENDED_ARG:
      case FOR_ITER:
      case GET_ITER:
      case IS_OP:
      case JUMP_BACKWARD:
      case JUMP_BACKWARD_NO_INTERRUPT:
      case JUMP_FORWARD:
      case LOAD_CONST:
      case LOAD_FAST:
      case NOP:
      case POP_JUMP_BACKWARD_IF_FALSE:
      case POP_JUMP_BACKWARD_IF_NONE:
      case POP_JUMP_BACKWARD_IF_NOT_NONE:
      case POP_JUMP_BACKWARD_IF_TRUE:
      case POP_JUMP_FORWARD_IF_FALSE:
      case POP_JUMP_FORWARD_IF_NONE:
      case POP_JUMP_FORWARD_IF_NOT_NONE:
      case POP_JUMP_FORWARD_IF_TRUE:
      case POP_TOP:
      case RESUME:
      case RETURN_VALUE:
      case STORE_FAST:
      case SWAP:
      case UNARY_INVERT:
      case UNARY_NEGATIVE:
      case UNARY_NOT:
      case UNARY_POSITIVE:
        break;
      default:
        return "REFUSE_SHAPE_EXECUTE_SURFACE";
    }
  }
#else
  (void)code;
#endif
  return nullptr;
}

std::unique_ptr<Function> buildHIR(const Preloader& preloader) {
  return HIRBuilder{preloader}.buildHIR();
}

// This performs an abstract interpretation over the bytecode for func in order
// to translate it from a stack to register machine. The translation proceeds
// in two passes over the bytecode. First, basic block boundaries are
// enumerated and a mapping from block start offset to basic block is
// created. Next, basic blocks are filled in by simulating the effect that each
// instruction has on the stack.
//
// The correctness of the translation depends on the invariant that the depth
// the operand stack is be constant at each program point.  All of the CPython
// bytecode that we currently support maintain this invariant. However, there
// are a few bytecodes that do not (e.g. SETUP_FINALLY). We will need to deal
// with that if we ever want to support compiling them.
std::unique_ptr<Function> HIRBuilder::buildHIR() {
  checkTranslate();
  code_has_backedge_ = codeHasBackedge(code_);
  code_is_simple_numeric_leaf_ = isSimpleNumericLeafFunction(code_);

#if PY_VERSION_HEX < 0x030C0000
  if (code_->co_flags & kCoFlagsAnyGenerator) {
    JIT_THROW(
        "generators are unsupported on CPython 3.11 in {}",
        preloader_.fullname());
  }
#if PY_VERSION_HEX >= 0x030B0000
  const char* shape_reason = unsupportedShapeReason311(code_);
  if (shape_reason != nullptr) {
    JIT_THROW(
        "code shape {} is unsupported on CPython 3.11 in {}",
        shape_reason,
        preloader_.fullname());
  }
#endif
#endif

  is_simple_leaf_function_ = isSimpleLeafFunction(code_);

  std::unique_ptr<Function> irfunc = preloader_.makeFunction();
  buildHIRImpl(irfunc.get(), /*frame_state=*/nullptr);
  // Use removeTrampolineBlocks and removeUnreachableBlocks directly instead of
  // Run because the rest of CleanCFG requires SSA.
  removeTrampolineBlocks(&irfunc->cfg);
  removeUnreachableBlocks(*irfunc);
  return irfunc;
}

// Loop through each of the arguments on the current translation context and
// check and see if there is any annotation to guard against.
bool HIRBuilder::emitTypeAnnotationGuards(TranslationContext& tc) {
  AnnotationIndex* index = preloader_.annotations();
  bool first = true;

  auto emit_arg_guard = [&](int arg_idx,
                            Type type,
                            bool needs_frame_state = false) {
    // If we have a guard to emit, we need a snapshot for deopt. Callers should
    // only emit entry guards after any bytecode setup required by the frame has
    // been translated, so the current instruction offset is a valid resume
    // point.
    if (first) {
      first = false;
      tc.emitSnapshot();
    }

    auto arg = tc.frame.localsplus.at(arg_idx);
    JIT_CHECK(arg != nullptr, "No register for argument {}", arg_idx);
    if (needs_frame_state) {
      tc.emit<GuardType>(arg, type, arg, tc.frame);
    } else {
      tc.emit<GuardType>(arg, type, arg);
    }
  };

  auto emit_inferred_self_guard = [&]() {
    auto inferred_self_type = preloader_.inferredSelfType();
    if (!inferred_self_type) {
      return;
    }

    auto arg = tc.frame.localsplus.at(0);
    JIT_CHECK(arg != nullptr, "No register for argument 0");
    if (arg->type() != TTop && arg->type() != TObject) {
      return;
    }

    emit_arg_guard(0, *inferred_self_type, /*needs_frame_state=*/true);
  };

  emit_inferred_self_guard();

  // Bail out if there are no annotations.
  if (!index) {
    return !first;
  }

  PyCodeObject* const code = tc.frame.code;

  for (int arg_idx = 0; arg_idx < preloader_.numArgs(); arg_idx++) {
    PyObject* annotation = index->find(getVarname(code, arg_idx));

    // If there is no annotation or if the annotation is an unexpected type,
    // then skip over this argument.
    //
    // Note that this also skips over more complex types like unions. It could
    // be beneficial in the future to support runtime checks for these kinds of
    // annotations.
    if (!annotation || !PyType_Check(annotation)) {
      continue;
    }

    Type type =
        Type::fromTypeExact(reinterpret_cast<PyTypeObject*>(annotation));
    emit_arg_guard(arg_idx, type);
  }

  return !first;
}

BasicBlock* HIRBuilder::buildHIRImpl(
    Function* irfunc,
    FrameState* frame_state) {
  temps_ = TempAllocator(&irfunc->env);
  env_ = &irfunc->env;

  BytecodeInstructionBlock bc_instrs{code_};
  block_map_ = createBlocks(*irfunc, bc_instrs);

  // Ensure that the entry block isn't a loop header
  BasicBlock* entry_block = getBlockAtOff(BCOffset{0});
  for (const auto& bci : bc_instrs) {
    if (bci.isBranch() && bci.getJumpTarget() == 0) {
      entry_block = irfunc->cfg.AllocateBlock();
      break;
    }
  }
  if (frame_state == nullptr) {
    // Function is not being inlined (irfunc matches code) so set the whole
    // CFG's entry block.
    irfunc->cfg.entry_block = entry_block;
  }

  // Insert LoadArg, LoadClosureCell, and MakeCell/MakeNullCell instructions
  // for the entry block
  TranslationContext entry_tc{
      entry_block,
      FrameState{
          code_,
          preloader_.globals(),
          preloader_.builtins(),
          /*parent=*/frame_state}};
  allocateLocalsplus(&irfunc->env, entry_tc.frame);

  addLoadArgs(entry_tc, preloader_.numArgs());

  if (frame_state == nullptr) {
    func_ = temps_.AllocateNonStack();
    entry_tc.emit<LoadCurrentFunc>(func_);
  }

  if (frame_state == nullptr) {
    entry_tc.emit<LoadFrame>();
  }

#if PY_VERSION_HEX < 0x030C0000
  // CPython 3.11 localsplus start as NULL.  Arguments are defined by
  // LoadArg above; remaining locals need an explicit reaching definition so
  // LOAD_FAST unbound checks (and LIR Moves) do not see a missing operand
  // after dead-branch elimination.  Must come after the Load* prologue:
  // SSAify's verifier rejects LoadFrame after any non-LoadArg/LoadCurrentFunc/
  // LoadFrame instruction.
  for (int i = preloader_.numArgs(); i < numLocals(code_); ++i) {
    entry_tc.emit<LoadConst>(entry_tc.frame.localsplus[i], TNullptr);
  }
#endif

  // "Initial Yield" has an explicit bytecode instruction in
  // "RETURN_GENERATOR" and so is emitted at the appropriate time.

  BasicBlock* first_block = getBlockAtOff(BCOffset{0});
  if (entry_block != first_block) {
    entry_block->appendWithOff<Branch>(BCOffset{0}, first_block);
  }

  entry_tc.block = first_block;
  translate(*irfunc, bc_instrs, entry_tc);

  return entry_block;
}

InlineResult HIRBuilder::inlineHIR(
    Function* caller,
    FrameState* caller_frame_state) {
  checkTranslate();
  code_has_backedge_ = codeHasBackedge(code_);
  code_is_simple_numeric_leaf_ = isSimpleNumericLeafFunction(code_);

  BasicBlock* entry_block = buildHIRImpl(caller, caller_frame_state);
  // Make one block with a Return that merges the return branches from the
  // callee. After SSA, it will turn into a massive Phi. The caller can find
  // the Return and use it as the output of the call instruction.
  Register* return_val = caller->env.AllocateRegister();
  BasicBlock* exit_block = caller->cfg.AllocateBlock();
  if (preloader_.returnType() <= TPrimitive) {
    exit_block->append<Return>(return_val, preloader_.returnType());
  } else {
    exit_block->append<Return>(return_val);
  }
  for (auto block : caller->cfg.GetRPOTraversal(entry_block)) {
    auto instr = block->GetTerminator();
    if (instr->IsReturn()) {
      auto assign = Assign::create(return_val, instr->GetOperand(0));
      auto branch = Branch::create(exit_block);
      instr->ExpandInto({assign, branch});
      delete instr;
    }
  }

  // Map of FrameState to parent pointers. We must completely disconnect the
  // inlined function's CFG from its caller for SSAify to run properly: it will
  // find uses (in FrameState) before defs and insert LoadConst<Nullptr>.
  UnorderedMap<FrameState*, FrameState*> framestate_parent;
  for (BasicBlock* block : caller->cfg.GetRPOTraversal(entry_block)) {
    for (Instr& instr : *block) {
      JIT_CHECK(
          !instr.IsBeginInlinedFunction(),
          "there should be no BeginInlinedFunction in inlined functions");
      JIT_CHECK(
          !instr.IsEndInlinedFunction(),
          "there should be no EndInlinedFunction in inlined functions");
      FrameState* fs = nullptr;
      if (auto db = instr.asDeoptBase()) {
        fs = db->frameState();
      } else if (instr.opcode() == Opcode::kSnapshot) {
        auto snap = dynamic_cast<Snapshot*>(&instr);
        fs = snap->frameState();
      }
      if (fs == nullptr || fs->parent == nullptr) {
        continue;
      }
      bool inserted = framestate_parent.emplace(fs, fs->parent).second;
      JIT_CHECK(inserted, "there should not be duplicate FrameState pointers");
      fs->parent = nullptr;
    }
  }

  // The caller function has already been converted to SSA form and all HIR
  // passes require input to be in SSA form. SSAify the inlined function.
  SSAify{}.Run(*caller, entry_block);

  // Re-link the CFG.
  for (auto& [fs, parent] : framestate_parent) {
    fs->parent = parent;
  }

  return {entry_block, exit_block};
}

void HIRBuilder::translate(
    Function& irfunc,
    const jit::BytecodeInstructionBlock& bc_instrs,
    const TranslationContext& initial_tc) {
  std::deque<TranslationContext> queue = {initial_tc};
  std::unordered_set<BasicBlock*> processed;
  std::unordered_set<BasicBlock*> loop_headers;
#if PY_VERSION_HEX < 0x030C0000
  // One record per backward jump: the edge and the frame state AT the
  // jump, after its stack effect.  The polls these become are inserted
  // per edge (see insertRunPeriodicActivitesForBackedge), matching stock
  // 3.11's eval-breaker placement and its traceback position.
  struct BackedgePoll {
    BasicBlock* src;
    BasicBlock* target;
    FrameState frame;
  };
  std::vector<BackedgePoll> backedge_polls;
#endif
  bool entry_guards_emitted = false;
  bool in_entry_setup = true;
  bool saw_initial_yield = false;

  while (!queue.empty()) {
    auto tc = std::move(queue.front());
    queue.pop_front();
    if (processed.contains(tc.block)) {
      continue;
    }
    processed.emplace(tc.block);

    // Translate remaining instructions into HIR
    auto& bc_block = map_get(block_map_.bc_blocks, tc.block);
    bool is_entry_bc_block = bc_block.startOffset() == BCOffset{0};

    auto is_in_async_for_header_block = [&tc, &bc_instrs]() {
      if (tc.frame.block_stack.isEmpty()) {
        return false;
      }
      const ExecutionBlock& block_top = tc.frame.block_stack.top();
      return block_top.isAsyncForHeaderBlock(bc_instrs);
    };

    BytecodeInstruction prev_bc_instr{code_, BCOffset{-2}};
    for (auto bc_it = bc_block.begin(); bc_it != bc_block.end(); ++bc_it) {
      BytecodeInstruction bc_instr = *bc_it;

      tc.frame.cur_instr_offs = bc_instr.baseOffset();
      Instr* prev_hir_instr = tc.block->GetTerminator();
      bool emitted_entry_guards = false;
      bool initial_yield_value_on_stack =
          saw_initial_yield && !tc.frame.stack.isEmpty();
      bool is_entry_setup_instr = is_entry_bc_block && in_entry_setup &&
          isEntrySetupInstr(bc_instr, initial_yield_value_on_stack);
      if (is_entry_bc_block && !entry_guards_emitted &&
          !is_entry_setup_instr) {
        JIT_DCHECK(
            tc.frame.stack.isEmpty(),
            "entry guards inserted with non-empty operand stack");
        if (tc.frame.stack.isEmpty()) {
          emitted_entry_guards = emitTypeAnnotationGuards(tc);
        }
        entry_guards_emitted = true;
        in_entry_setup = false;
      }
      // Outputting too many snapshots is safe but noisy so try to cull.
      // Note in some cases we'll have a non-empty block without yet having
      // translated any bytecodes. For example, if this is the first block and
      // there were prologue HIR instructions.
      if (!emitted_entry_guards &&
          // A completely empty block always gets a snapshot.
          (prev_hir_instr == nullptr ||
           // If we already have HIR instructions but haven't processed a
           // bytecode yet then conservatively emit a Snapshot.
           prev_bc_instr.baseOffset() < 0 ||
           // Only emit a Snapshot after bytecode instructions which might
           // change the frame state.
           should_snapshot(prev_bc_instr, is_in_async_for_header_block()))) {
        if (prev_hir_instr && prev_hir_instr->IsSnapshot()) {
          auto snapshot = static_cast<Snapshot*>(prev_hir_instr);
          snapshot->setFrameState(tc.frame);
        } else {
          tc.emit<Snapshot>(tc.frame);
        }
      }
      prev_bc_instr = bc_instr;

      // Translate instruction
      auto opcode = bc_instr.opcode();
      switch (opcode) {
        case NOP:
        case NOT_TAKEN: {
          break;
        }
        case PUSH_NULL: {
          emitPushNull(tc);
          break;
        }
        case BINARY_ADD:
        case BINARY_AND:
        case BINARY_FLOOR_DIVIDE:
        case BINARY_LSHIFT:
        case BINARY_MATRIX_MULTIPLY:
        case BINARY_MODULO:
        case BINARY_MULTIPLY:
        case BINARY_OP:
        case BINARY_OR:
        case BINARY_POWER:
        case BINARY_RSHIFT:
        case BINARY_SUBSCR:
        case BINARY_SUBTRACT:
        case BINARY_TRUE_DIVIDE:
        case BINARY_XOR: {
          // The array.array('d') subscript fast path is emitted later in the
          // Simplify pass (simplifyBinaryOp), where the container's type is
          // known, so it does not perturb subscripts on statically-typed
          // containers (list/tuple/dict/str).
          emitBinaryOp(tc, bc_instr);
          break;
        }
        case INPLACE_ADD:
        case INPLACE_AND:
        case INPLACE_FLOOR_DIVIDE:
        case INPLACE_LSHIFT:
        case INPLACE_MATRIX_MULTIPLY:
        case INPLACE_MODULO:
        case INPLACE_MULTIPLY:
        case INPLACE_OR:
        case INPLACE_POWER:
        case INPLACE_RSHIFT:
        case INPLACE_SUBTRACT:
        case INPLACE_TRUE_DIVIDE:
        case INPLACE_XOR: {
          emitInPlaceOp(tc, bc_instr);
          break;
        }
        case UNARY_NOT:
#if PY_VERSION_HEX >= 0x030E0000
          emitUnaryNot(tc);
          break;
#endif
        case UNARY_NEGATIVE:
        case UNARY_POSITIVE:
        case UNARY_INVERT: {
          emitUnaryOp(tc, bc_instr);
          break;
        }
        case BUILD_LIST:
        case BUILD_TUPLE:
          emitMakeListTuple(tc, bc_instr);
          break;
        case BUILD_CHECKED_LIST: {
          emitBuildCheckedList(tc, bc_instr);
          break;
        }
        case BUILD_CHECKED_MAP: {
          emitBuildCheckedMap(tc, bc_instr);
          break;
        }
        case BUILD_MAP: {
          emitBuildMap(tc, bc_instr);
          break;
        }
        case BUILD_SET: {
          emitBuildSet(tc, bc_instr);
          break;
        }
        case BUILD_CONST_KEY_MAP: {
          emitBuildConstKeyMap(tc, bc_instr);
          break;
        }
#if PY_VERSION_HEX < 0x030C0000
        case PRECALL: {
          break;
        }
#endif
        case CALL:
        case CALL_FUNCTION:
        case CALL_FUNCTION_EX:
        case CALL_FUNCTION_KW:
        case CALL_KW:
        case CALL_METHOD:
        case INVOKE_FUNCTION:
        case INVOKE_METHOD:
        case INVOKE_NATIVE: {
          emitAnyCall(irfunc.cfg, tc, bc_it, bc_instrs);
          break;
        }
        case CALL_INTRINSIC_1:
        case CALL_INTRINSIC_2: {
          emitCallInstrinsic(tc, bc_instr);
          break;
        }
        case RESUME: {
          emitResume(irfunc.cfg, tc, bc_instr);
          break;
        }
        case KW_NAMES: {
          emitKwNames(tc, bc_instr);
          break;
        }
        case MAKE_CELL: {
          emitMakeCell(tc, bc_instr.oparg());
          break;
        }
        case COPY: {
          emitCopy(tc, bc_instr.oparg());
          break;
        }
        case COPY_FREE_VARS: {
          emitCopyFreeVars(tc, bc_instr.oparg());
          break;
        }
        case SWAP: {
          emitSwap(tc, bc_instr.oparg());
          break;
        }
        case IS_OP: {
          emitIsOp(tc, bc_instr.oparg());
          break;
        }
        case CONTAINS_OP: {
          emitContainsOp(tc, bc_instr.oparg());
          break;
        }
        case COMPARE_OP: {
          emitCompareOp(tc, bc_instr);
          break;
        }
        case TO_BOOL: {
          emitToBool(tc, &bc_instr);
          break;
        }
        case COPY_DICT_WITHOUT_KEYS: {
          emitCopyDictWithoutKeys(tc);
          break;
        }
        case GET_LEN: {
          emitGetLen(tc);
          break;
        }
        case DELETE_ATTR: {
          emitDeleteAttr(tc, bc_instr);
          break;
        }
        case LOAD_ATTR: {
          emitLoadAttr(irfunc.cfg, tc, bc_instr);
          break;
        }
        case LOAD_METHOD: {
          emitLoadMethod(tc, bc_instr);
          break;
        }
        case LOAD_METHOD_STATIC: {
          emitLoadMethodStatic(tc, bc_instr);
          break;
        }
        case LOAD_METHOD_SUPER: {
          emitLoadMethodOrAttrSuper(irfunc.cfg, tc, bc_instr, true);
          break;
        }
        case LOAD_ASSERTION_ERROR: {
          emitLoadAssertionError(tc, irfunc.env);
          break;
        }
        case LOAD_ATTR_SUPER:
        case LOAD_SUPER_ATTR: {
          emitLoadMethodOrAttrSuper(irfunc.cfg, tc, bc_instr, false);
          break;
        }
        case LOAD_CLOSURE: {
          int idx = bc_instr.oparg();
          tc.frame.stack.push(tc.frame.localsplus[idx]);
          break;
        }
        case LOAD_DEREF: {
          emitLoadDeref(tc, bc_instr);
          break;
        }
        case STORE_DEREF: {
          emitStoreDeref(tc, bc_instr);
          break;
        }
        case LOAD_CLASS: {
          emitLoadClass(tc, bc_instr);
          break;
        }
        case LOAD_CONST: {
          emitLoadConst(tc, bc_instr);
          break;
        }
        case LOAD_FAST:
        case LOAD_FAST_AND_CLEAR:
        case LOAD_FAST_CHECK:
        case LOAD_FAST_BORROW: {
          emitLoadFast(tc, bc_instr);
          break;
        }
        case LOAD_FAST_LOAD_FAST:
        case LOAD_FAST_BORROW_LOAD_FAST_BORROW: {
          if (tryEmitListPrefixReverseAssign(tc, bc_it, bc_block.end())) {
            prev_bc_instr = *bc_it;
            break;
          }
          emitLoadFastLoadFast(tc, bc_instr);
          break;
        }
        case LOAD_LOCAL: {
          emitLoadLocal(tc, bc_instr);
          break;
        }
        case LOAD_SMALL_INT: {
          emitLoadSmallInt(tc, bc_instr);
          break;
        }
        case LOAD_SPECIAL: {
          emitLoadSpecial(tc, bc_instr);
          break;
        }
        case LOAD_TYPE: {
          emitLoadType(tc, bc_instr);
          break;
        }
        case CONVERT_PRIMITIVE: {
          emitConvertPrimitive(tc, bc_instr);
          break;
        }
        case PRIMITIVE_LOAD_CONST: {
          emitPrimitiveLoadConst(tc, bc_instr);
          break;
        }
        case PRIMITIVE_BOX: {
          emitPrimitiveBox(tc, bc_instr);
          break;
        }
        case PRIMITIVE_UNBOX: {
          emitPrimitiveUnbox(tc, bc_instr);
          break;
        }
        case PRIMITIVE_BINARY_OP: {
          emitPrimitiveBinaryOp(tc, bc_instr);
          break;
        }
        case PRIMITIVE_COMPARE_OP: {
          emitPrimitiveCompare(tc, bc_instr);
          break;
        }
        case PRIMITIVE_UNARY_OP: {
          emitPrimitiveUnaryOp(tc, bc_instr);
          break;
        }
        case FAST_LEN: {
          emitFastLen(irfunc.cfg, tc, bc_instr);
          break;
        }
        case REFINE_TYPE: {
          emitRefineType(tc, bc_instr);
          break;
        }
        case SEQUENCE_GET: {
          emitSequenceGet(tc, bc_instr);
          break;
        }
        case SEQUENCE_SET: {
          emitSequenceSet(tc, bc_instr);
          break;
        }
        case LOAD_GLOBAL: {
          if (tryEmitLoadMethodOrAttrSuper311(
                  irfunc.cfg, tc, bc_it, bc_block)) {
            break;
          }
          emitLoadGlobal(tc, bc_instr);
          break;
        }
        case JUMP_ABSOLUTE:
        case JUMP_BACKWARD:
#if PY_VERSION_HEX >= 0x030E0000
        case JUMP_BACKWARD_JIT:
        case JUMP_BACKWARD_NO_JIT:
#endif
        {
          BCOffset target_off = bc_instr.getJumpTarget();
          BasicBlock* target = getBlockAtOff(target_off);
          if (target_off <= bc_instr.baseOffset() || opcode != JUMP_ABSOLUTE) {
            loop_headers.emplace(target);
#if PY_VERSION_HEX < 0x030C0000
            backedge_polls.push_back({tc.block, target, tc.frame});
#endif
          }
          tc.emit<Branch>(target);
          break;
        }
        case JUMP_BACKWARD_NO_INTERRUPT:
        case JUMP_FORWARD: {
          BCOffset target_off = bc_instr.getJumpTarget();
          BasicBlock* target = getBlockAtOff(target_off);
          tc.emit<Branch>(target);
          break;
        }
        case JUMP_IF_FALSE_OR_POP:
        case JUMP_IF_NONZERO_OR_POP:
        case JUMP_IF_TRUE_OR_POP:
        case JUMP_IF_ZERO_OR_POP: {
          emitJumpIf(tc, bc_instr);
          break;
        }
        case POP_BLOCK: {
          popBlock(irfunc.cfg, tc);
          break;
        }
        case POP_JUMP_IF_FALSE:
        case POP_JUMP_IF_TRUE:
#if PY_VERSION_HEX < 0x030C0000
        case POP_JUMP_BACKWARD_IF_FALSE:
        case POP_JUMP_BACKWARD_IF_TRUE:
        case POP_JUMP_FORWARD_IF_FALSE:
        case POP_JUMP_FORWARD_IF_TRUE:
#endif
        {
          BasicBlock* target = getBlockAtOff(bc_instr.getJumpTarget());
#if PY_VERSION_HEX < 0x030C0000
          BasicBlock* jump_block = tc.block;
#endif
          if (bc_instr.isBackwardBranch()) {
            loop_headers.emplace(target);
          }
          emitPopJumpIf(tc, bc_instr);
#if PY_VERSION_HEX < 0x030C0000
          if (bc_instr.isBackwardBranch()) {
            // Recorded after the emit: the poll's frame state is the
            // post-pop state at the jump.
            backedge_polls.push_back({jump_block, target, tc.frame});
          }
#endif
          break;
        }
        case POP_JUMP_IF_NONE:
        case POP_JUMP_IF_NOT_NONE:
#if PY_VERSION_HEX < 0x030C0000
        case POP_JUMP_BACKWARD_IF_NONE:
        case POP_JUMP_BACKWARD_IF_NOT_NONE:
        case POP_JUMP_FORWARD_IF_NONE:
        case POP_JUMP_FORWARD_IF_NOT_NONE:
#endif
        {
          BasicBlock* target = getBlockAtOff(bc_instr.getJumpTarget());
#if PY_VERSION_HEX < 0x030C0000
          BasicBlock* jump_block = tc.block;
#endif
          if (bc_instr.isBackwardBranch()) {
            loop_headers.emplace(target);
          }
          emitPopJumpIfNone(tc, bc_instr);
#if PY_VERSION_HEX < 0x030C0000
          if (bc_instr.isBackwardBranch()) {
            // Recorded after the emit: the poll's frame state is the
            // post-pop state at the jump.
            backedge_polls.push_back({jump_block, target, tc.frame});
          }
#endif
          break;
        }
        case POP_ITER:
          if constexpr (PY_VERSION_HEX >= 0x030F0000) {
            tc.frame.stack.pop();
          }
          tc.frame.stack.pop();
          break;
        case POP_TOP: {
          tc.frame.stack.pop();
          break;
        }
        case RETURN_CONST: {
          Register* reg = temps_.AllocateStack();
          JIT_CHECK(
              bc_instr.oparg() < PyTuple_Size(code_->co_consts),
              "RETURN_CONST index out of bounds");
          Type type = Type::fromObject(
              PyTuple_GET_ITEM(code_->co_consts, bc_instr.oparg()));
          tc.emit<LoadConst>(reg, type);
          if (getConfig().refine_static_python && type < TObject) {
            tc.emit<RefineType>(reg, type, reg);
          }
          tc.emit<Return>(reg, type);
          break;
        }
        case RETURN_PRIMITIVE: {
          Type type = prim_type_to_type(bc_instr.oparg());
          JIT_CHECK(
              type <= preloader_.returnType(),
              "bad return type {}, expected {}",
              type,
              preloader_.returnType());
          Register* reg = tc.frame.stack.pop();
          tc.emit<Return>(reg, type);
          break;
        }
        case RETURN_VALUE: {
          JIT_CHECK(
              tc.frame.block_stack.isEmpty(),
              "Returning with non-empty block stack");
          Register* reg = tc.frame.stack.pop();
          Type ret_type = preloader_.returnType();
          if (getConfig().refine_static_python && ret_type < TObject) {
            tc.emit<RefineType>(reg, ret_type, reg);
          }
          tc.emit<Return>(reg, ret_type);
          break;
        }
        case ROT_N: {
          rotateStackTop(tc.frame.stack, bc_instr.oparg());
          break;
        }
#if PY_VERSION_HEX >= 0x030C0000
        case END_ASYNC_FOR: {
          emitEndAsyncFor(tc);
          break;
        }
#endif
        case END_FOR: {
          // This instruction is only for use when FOR_ITER is specialized for a
          // generator. As we use unspecialized bytecode only, we modify
          // BytecodeInstruction::getJumpTarget() to always skip the END_FOR so
          // that block should never be processed.
          BUILDER_THROW("We should never cross an END_FOR in the HIR builder");
        }
        case SETUP_FINALLY: {
          emitSetupFinally(tc, bc_instr);
          break;
        }
        case STORE_ATTR: {
          emitStoreAttr(tc, bc_instr);
          break;
        }
        case STORE_FAST: {
          emitStoreFast(tc, bc_instr);
          break;
        }
        case STORE_FAST_STORE_FAST: {
          emitStoreFastStoreFast(tc, bc_instr);
          break;
        }
        case STORE_FAST_LOAD_FAST: {
          emitStoreFastLoadFast(tc, bc_instr);
          break;
        }
        case STORE_LOCAL: {
          emitStoreLocal(tc, bc_instr);
          break;
        }
        case BINARY_SLICE: {
          emitBinarySlice(tc);
          break;
        }
        case STORE_SLICE: {
          emitStoreSlice(tc);
          break;
        }
        case STORE_SUBSCR: {
          emitStoreSubscr(irfunc.cfg, tc, bc_instr);
          break;
        }
        case BUILD_SLICE: {
          emitBuildSlice(tc, bc_instr);
          break;
        }
#if PY_VERSION_HEX >= 0x030C0000
        case GET_AITER: {
          emitGetAIter(tc);
          break;
        }
        case GET_ANEXT: {
          emitGetANext(tc);
          break;
        }
#endif
        case GET_ITER: {
          if constexpr (PY_VERSION_HEX >= 0x030F0000) {
            if (bc_instr.oparg() > 0) {
              emitGetYieldFromIter(irfunc.cfg, tc);
              emitPushNull(tc);
            } else {
              emitGetIter(tc);
            }
          } else {
            emitGetIter(tc);
          }
          break;
        }
        case GET_YIELD_FROM_ITER: {
          emitGetYieldFromIter(irfunc.cfg, tc);
          break;
        }
        case MAKE_FUNCTION: {
          emitMakeFunction(tc, bc_instr);
          break;
        }
        case LIST_APPEND: {
          emitListAppend(tc, bc_instr);
          break;
        }
        case LIST_EXTEND: {
          emitListExtend(tc, bc_instr);
          break;
        }
        case LIST_TO_TUPLE: {
          emitListToTuple(tc);
          break;
        }
        case LOAD_ITERABLE_ARG: {
          emitLoadIterableArg(irfunc.cfg, tc, bc_instr);
          break;
        }
        case DUP_TOP: {
          auto& stack = tc.frame.stack;
          stack.push(stack.top());
          break;
        }
        case DUP_TOP_TWO: {
          auto& stack = tc.frame.stack;
          Register* top = stack.top();
          Register* snd = stack.top(1);
          stack.push(snd);
          stack.push(top);
          break;
        }
        case ROT_TWO: {
          rotateStackTop(tc.frame.stack, 2);
          break;
        }
        case ROT_THREE: {
          rotateStackTop(tc.frame.stack, 3);
          break;
        }
        case ROT_FOUR: {
          rotateStackTop(tc.frame.stack, 4);
          break;
        }
        case FOR_ITER: {
          emitForIter(tc, bc_instr);
          break;
        }
        case LOAD_FIELD: {
          emitLoadField(tc, bc_instr);
          break;
        }
        case CAST: {
          emitCast(tc, bc_instr);
          break;
        }
        case TP_ALLOC: {
          emitTpAlloc(tc, bc_instr);
          break;
        }
        case STORE_FIELD: {
          emitStoreField(tc, bc_instr);
          break;
        }
        case POP_JUMP_IF_ZERO:
        case POP_JUMP_IF_NONZERO: {
          emitPopJumpIf(tc, bc_instr);
          break;
        }
#if PY_VERSION_HEX >= 0x030E0000 || ENABLE_LAZY_IMPORTS
        case IMPORT_FROM: {
          emitImportFrom(tc, bc_instr);
          break;
        }
#endif
        case EAGER_IMPORT_NAME:
        case IMPORT_NAME: {
          emitImportName(tc, bc_instr);
          break;
        }
        case RAISE_VARARGS: {
          emitRaiseVarargs(tc);
          break;
        }
        case YIELD_VALUE: {
          emitYieldValue(tc, bc_instr);
          break;
        }
        case YIELD_FROM: {
          if (is_in_async_for_header_block()) {
            emitAsyncForHeaderYieldFrom(irfunc.cfg, tc, bc_instr);
          } else {
            emitYieldFrom(irfunc.cfg, tc, temps_.AllocateStack());
          }
          break;
        }
#if PY_VERSION_HEX >= 0x030C0000
        case GET_AWAITABLE: {
          emitGetAwaitable(irfunc.cfg, tc, bc_instrs, bc_instr);
          break;
        }
#endif
        case BUILD_STRING: {
          emitBuildString(tc, bc_instr);
          break;
        }
        case FORMAT_VALUE: {
          emitFormatValue(tc, bc_instr);
          break;
        }
        case FORMAT_WITH_SPEC: {
          emitFormatWithSpec(tc);
          break;
        }
        case MAP_ADD: {
          emitMapAdd(tc, bc_instr);
          break;
        }
        case SET_ADD: {
          emitSetAdd(tc, bc_instr);
          break;
        }
        case SET_UPDATE: {
          emitSetUpdate(tc, bc_instr);
          break;
        }
        case UNPACK_EX: {
          emitUnpackEx(tc, bc_instr);
          break;
        }
        case UNPACK_SEQUENCE: {
          emitUnpackSequence(irfunc.cfg, tc, bc_instr);
          break;
        }
        case DELETE_SUBSCR: {
          Register* sub = tc.frame.stack.pop();
          Register* container = tc.frame.stack.pop();
          tc.emit<DeleteSubscr>(container, sub, tc.frame);
          break;
        }
        case DELETE_FAST: {
          int var_idx = bc_instr.oparg();
          Register* var = tc.frame.localsplus[var_idx];
          moveOverwrittenStackRegisters(tc, var);
          tc.emit<LoadConst>(var, TNullptr);
          break;
        }
#if PY_VERSION_HEX >= 0x030C0000
        case BEFORE_ASYNC_WITH:
#endif
        case BEFORE_WITH: {
          emitBeforeWith(tc, bc_instr);
          break;
        }
        case SETUP_ASYNC_WITH: {
          emitSetupAsyncWith(tc, bc_instr);
          break;
        }
        case SETUP_WITH: {
          emitSetupWith(tc, bc_instr);
          break;
        }
#if PY_VERSION_HEX >= 0x030C0000
        case MATCH_CLASS: {
          emitMatchClass(irfunc.cfg, tc, bc_instr);
          break;
        }
        case MATCH_KEYS: {
          emitMatchKeys(irfunc.cfg, tc);
          break;
        }
        case MATCH_MAPPING: {
          emitMatchMappingSequence(irfunc.cfg, tc, Py_TPFLAGS_MAPPING);
          break;
        }
        case MATCH_SEQUENCE: {
          emitMatchMappingSequence(irfunc.cfg, tc, Py_TPFLAGS_SEQUENCE);
          break;
        }
#endif
        case GEN_START: {
          // In the interpreter this instruction behaves like POP_TOP because it
          // assumes a generator will always be sent a superfluous None value to
          // start execution via the stack. We skip doing this for JIT
          // functions. This should be fine as long as we can't de-opt after the
          // function is started but before GEN_START. This check ensures this.
          JIT_DCHECK(
              bc_instr.baseIndex() == 0, "GEN_START must be first instruction");
          break;
        }
        case DICT_UPDATE: {
          emitDictUpdate(tc, bc_instr);
          break;
        }
        case DICT_MERGE: {
          emitDictMerge(tc, bc_instr);
          break;
        }
        case RETURN_GENERATOR: {
          auto out = temps_.AllocateStack();
          tc.emit<InitialYield>(out, tc.frame);
          tc.frame.stack.push(out);
          break;
        }
        case SEND: {
          emitSend(tc, bc_instr);
          break;
        }
        case END_SEND: {
          // Pop the value and iterator off the stack and then push back the
          // value.
          Register* value = tc.frame.stack.pop();
          if constexpr (PY_VERSION_HEX >= 0x030F0000) {
            tc.frame.stack.pop();
          }
          tc.frame.stack.pop();
          tc.frame.stack.push(value);
          break;
        }
        case BUILD_INTERPOLATION: {
          emitBuildInterpolation(tc, bc_instr);
          break;
        }
        case BUILD_TEMPLATE: {
          emitBuildTemplate(tc);
          break;
        }
        case CONVERT_VALUE: {
          emitConvertValue(tc, bc_instr);
          break;
        }
        case FORMAT_SIMPLE: {
          emitFormatSimple(irfunc.cfg, tc);
          break;
        }
        case LOAD_COMMON_CONSTANT: {
          emitLoadCommonConstant(tc, bc_instr);
          break;
        }
        case SET_FUNCTION_ATTRIBUTE: {
          emitSetFunctionAttribute(tc, bc_instr);
          break;
        }
        case LOAD_BUILD_CLASS: {
          emitLoadBuildClass(tc);
          break;
        }
        case STORE_GLOBAL: {
          emitStoreGlobal(tc, bc_instr);
          break;
        }
#if PY_VERSION_HEX >= 0x030C0000
        case CHECK_EG_MATCH:
#endif
        case CHECK_EXC_MATCH:
        case CLEANUP_THROW:
        case POP_EXCEPT:
        case PUSH_EXC_INFO:
        case RERAISE:
        case WITH_EXCEPT_START:
          BUILDER_THROW(
              "{} appearing outside of exception handler", opcodeName(opcode));
        default: {
          BUILDER_THROW("Unhandled opcode {} ({})", opcodeName(opcode), opcode);
        }
      }

      if (is_entry_setup_instr) {
        if (opcode == RETURN_GENERATOR) {
          saw_initial_yield = true;
        } else if (opcode == POP_TOP && tc.frame.stack.isEmpty()) {
          saw_initial_yield = false;
        }
      }
    }
    // Insert jumps for blocks that fall through.
    auto last_instr = tc.block->GetTerminator();
    if ((last_instr == nullptr) || !last_instr->IsTerminator()) {
      auto off = bc_block.endOffset();
      last_instr = tc.emit<Branch>(getBlockAtOff(off));
    }

    // Make sure any values left on the stack are in the registers that we
    // expect
    BlockCanonicalizer bc;
    bc.Run(tc.block, temps_, tc.frame.stack);

    // Add successors to be processed
    //
    // These bytecodes alter the operand stack along one branch and leave it
    // untouched along the other. Thus, they must be special cased.
    switch (prev_bc_instr.opcode()) {
      case FOR_ITER: {
        auto condbr = static_cast<CondBranchIterNotDone*>(last_instr);
        auto new_frame = tc.frame;
        if constexpr (PY_VERSION_HEX >= 0x030E0000) {
          // Just pop the sentinel value. The target POP_ITER will pop the
          // iterator.
          new_frame.stack.discard(1);
        } else {
          // Pop both the sentinel value signaling iteration is complete
          // and the iterator itself.
          new_frame.stack.discard(2);
        }
        queue.emplace_back(condbr->true_bb(), tc.frame);
        queue.emplace_back(condbr->false_bb(), new_frame);
        break;
      }
      case JUMP_IF_FALSE_OR_POP:
      case JUMP_IF_ZERO_OR_POP: {
        auto condbr = static_cast<CondBranch*>(last_instr);
        auto new_frame = tc.frame;
        new_frame.stack.pop();
        queue.emplace_back(condbr->true_bb(), new_frame);
        queue.emplace_back(condbr->false_bb(), tc.frame);
        break;
      }
      case JUMP_IF_NONZERO_OR_POP:
      case JUMP_IF_TRUE_OR_POP: {
        auto condbr = static_cast<CondBranch*>(last_instr);
        auto new_frame = tc.frame;
        new_frame.stack.pop();
        queue.emplace_back(condbr->true_bb(), tc.frame);
        queue.emplace_back(condbr->false_bb(), new_frame);
        break;
      }
      default: {
        if (prev_bc_instr.opcode() == YIELD_FROM &&
            is_in_async_for_header_block()) {
          JIT_CHECK(
              last_instr->IsCondBranchIterNotDone(),
              "Async-for header should end with CondBranchIterNotDone");
          auto condbr = static_cast<CondBranchIterNotDone*>(last_instr);
          FrameState new_frame = tc.frame;
          // Pop sentinel value signaling that iteration is complete
          new_frame.stack.pop();
          queue.emplace_back(condbr->true_bb(), tc.frame);
          queue.emplace_back(condbr->false_bb(), std::move(new_frame));
          break;
        }
        for (std::size_t i = 0; i < last_instr->numEdges(); i++) {
          auto succ = last_instr->successor(i);
          queue.emplace_back(succ, tc.frame);
        }
        break;
      }
    }
    JIT_DCHECK(
        tc.block->GetTerminator() != nullptr &&
            !tc.block->GetTerminator()->IsSnapshot(),
        "opcodes should not end with a snapshot");
  }

  JIT_CHECK(
      kwnames_ == nullptr,
      "Stashed a KW_NAMES value for function {} but never consumed it",
      irfunc.fullname);

#if PY_VERSION_HEX < 0x030C0000
  // Per-edge polls, not a header-shared check block: stock 3.11 checks the
  // eval breaker inside the backward-jump handlers, on the taken branch
  // only, and its traceback for an asynchronous exception names the jump.
  // A shared check block in front of the header polls the loop-entry
  // fallthrough and JUMP_BACKWARD_NO_INTERRUPT too -- positions stock
  // never polls -- and can only carry the header's frame state, a
  // different bytecode position than stock reports.
  (void)loop_headers;
  for (auto& poll : backedge_polls) {
    insertRunPeriodicActivitesForBackedge(
        irfunc.cfg, poll.src, poll.target, poll.frame);
  }
#else
  for (auto block : loop_headers) {
    insertRunPeriodicActivitesForLoop(irfunc.cfg, block);
  }
#endif
}

void BlockCanonicalizer::InsertCopies(
    Register* reg,
    TempAllocator& temps,
    Instr& terminator,
    std::vector<Register*>& alloced) {
  if (done_.contains(reg)) {
    return;
  } else if (processing_.contains(reg)) {
    // We've detected a cycle. Move the register to a new home
    // in order to break the cycle.
    auto tmp = temps.AllocateStack();
    auto mov = Assign::create(tmp, reg);
    mov->copyBytecodeOffset(terminator);
    mov->InsertBefore(terminator);
    moved_[reg] = tmp;
    alloced.emplace_back(tmp);
    return;
  }

  auto orig_reg = reg;
  for (auto dst : copies_[reg]) {
    auto it = copies_.find(dst);
    if (it != copies_.end()) {
      // The destination also needs to be moved. So deal with it first.
      processing_.insert(reg);
      InsertCopies(dst, temps, terminator, alloced);
      processing_.erase(reg);
      // It's possible that the register we were processing was moved
      // because it participated in a cycle
      auto it2 = moved_.find(reg);
      if (it2 != moved_.end()) {
        reg = it2->second;
      }
    }
    auto mov = Assign::create(dst, reg);
    mov->copyBytecodeOffset(terminator);
    mov->InsertBefore(terminator);
  }

  done_.insert(orig_reg);
}

void BlockCanonicalizer::Run(
    BasicBlock* block,
    TempAllocator& temps,
    OperandStack& stack) {
  if (stack.isEmpty()) {
    return;
  }

  processing_.clear();
  copies_.clear();
  moved_.clear();

  // Compute the desired stack layout
  std::vector<Register*> dsts;
  dsts.reserve(stack.size());
  for (std::size_t i = 0; i < stack.size(); i++) {
    auto reg = temps.GetOrAllocateStack(i);
    dsts.emplace_back(reg);
  }

  // Compute the minimum number of copies that need to happen
  std::vector<Register*> need_copy;
  auto term = block->GetTerminator();
  std::vector<Register*> alloced;
  for (std::size_t i = 0; i < stack.size(); i++) {
    auto src = stack.at(i);
    auto dst = dsts[i];
    if (src != dst) {
      need_copy.emplace_back(src);
      copies_[src].emplace_back(dst);

      if (term->Uses(src)) {
        term->ReplaceUsesOf(src, dst);
      } else if (term->Uses(dst)) {
        auto tmp = temps.AllocateStack();
        alloced.emplace_back(tmp);
        auto mov = Assign::create(tmp, dst);
        mov->InsertBefore(*term);
        term->ReplaceUsesOf(dst, tmp);
      }
    }
  }
  if (need_copy.empty()) {
    return;
  }

  for (auto reg : need_copy) {
    InsertCopies(reg, temps, *term, alloced);
  }

  // Put the stack in canonical form
  for (std::size_t i = 0; i < stack.size(); i++) {
    stack.atPut(i, dsts[i]);
  }
}

static std::optional<BinaryOpKind> getBinaryOpKindFromOpcode(int opcode) {
  switch (opcode) {
    case BINARY_ADD:
      return BinaryOpKind::kAdd;
    case BINARY_AND:
      return BinaryOpKind::kAnd;
    case BINARY_FLOOR_DIVIDE:
      return BinaryOpKind::kFloorDivide;
    case BINARY_LSHIFT:
      return BinaryOpKind::kLShift;
    case BINARY_MATRIX_MULTIPLY:
      return BinaryOpKind::kMatrixMultiply;
    case BINARY_MODULO:
      return BinaryOpKind::kModulo;
    case BINARY_MULTIPLY:
      return BinaryOpKind::kMultiply;
    case BINARY_OR:
      return BinaryOpKind::kOr;
    case BINARY_POWER:
      return BinaryOpKind::kPower;
    case BINARY_RSHIFT:
      return BinaryOpKind::kRShift;
    case BINARY_SUBSCR:
      return BinaryOpKind::kSubscript;
    case BINARY_SUBTRACT:
      return BinaryOpKind::kSubtract;
    case BINARY_TRUE_DIVIDE:
      return BinaryOpKind::kTrueDivide;
    case BINARY_XOR:
      return BinaryOpKind::kXor;
    default:
      return std::nullopt;
  }
}

static std::optional<BinaryOpKind> getBinaryOpKindFromOparg(int oparg) {
  switch (oparg) {
    case NB_ADD:
      return BinaryOpKind::kAdd;
    case NB_AND:
      return BinaryOpKind::kAnd;
    case NB_FLOOR_DIVIDE:
      return BinaryOpKind::kFloorDivide;
    case NB_LSHIFT:
      return BinaryOpKind::kLShift;
    case NB_MATRIX_MULTIPLY:
      return BinaryOpKind::kMatrixMultiply;
    case NB_MULTIPLY:
      return BinaryOpKind::kMultiply;
    case NB_REMAINDER:
      return BinaryOpKind::kModulo;
    case NB_OR:
      return BinaryOpKind::kOr;
    case NB_POWER:
      return BinaryOpKind::kPower;
    case NB_RSHIFT:
      return BinaryOpKind::kRShift;
    case NB_SUBTRACT:
      return BinaryOpKind::kSubtract;
    case NB_TRUE_DIVIDE:
      return BinaryOpKind::kTrueDivide;
    case NB_XOR:
      return BinaryOpKind::kXor;
#if PY_VERSION_HEX >= 0x030E0000
    case NB_SUBSCR:
      return BinaryOpKind::kSubscript;
#endif
    default:
      return std::nullopt;
  }
}

static std::optional<InPlaceOpKind> getInPlaceOpKindFromOpcode(int opcode) {
  switch (opcode) {
    case INPLACE_ADD:
      return InPlaceOpKind::kAdd;
    case INPLACE_AND:
      return InPlaceOpKind::kAnd;
    case INPLACE_FLOOR_DIVIDE:
      return InPlaceOpKind::kFloorDivide;
    case INPLACE_LSHIFT:
      return InPlaceOpKind::kLShift;
    case INPLACE_MATRIX_MULTIPLY:
      return InPlaceOpKind::kMatrixMultiply;
    case INPLACE_MODULO:
      return InPlaceOpKind::kModulo;
    case INPLACE_MULTIPLY:
      return InPlaceOpKind::kMultiply;
    case INPLACE_OR:
      return InPlaceOpKind::kOr;
    case INPLACE_POWER:
      return InPlaceOpKind::kPower;
    case INPLACE_RSHIFT:
      return InPlaceOpKind::kRShift;
    case INPLACE_SUBTRACT:
      return InPlaceOpKind::kSubtract;
    case INPLACE_TRUE_DIVIDE:
      return InPlaceOpKind::kTrueDivide;
    case INPLACE_XOR:
      return InPlaceOpKind::kXor;
    default:
      return std::nullopt;
  }
}

static std::optional<InPlaceOpKind> getInPlaceOpKindFromOparg(int oparg) {
  switch (oparg) {
    case NB_INPLACE_ADD:
      return InPlaceOpKind::kAdd;
    case NB_INPLACE_AND:
      return InPlaceOpKind::kAnd;
    case NB_INPLACE_FLOOR_DIVIDE:
      return InPlaceOpKind::kFloorDivide;
    case NB_INPLACE_LSHIFT:
      return InPlaceOpKind::kLShift;
    case NB_INPLACE_MATRIX_MULTIPLY:
      return InPlaceOpKind::kMatrixMultiply;
    case NB_INPLACE_MULTIPLY:
      return InPlaceOpKind::kMultiply;
    case NB_INPLACE_REMAINDER:
      return InPlaceOpKind::kModulo;
    case NB_INPLACE_OR:
      return InPlaceOpKind::kOr;
    case NB_INPLACE_POWER:
      return InPlaceOpKind::kPower;
    case NB_INPLACE_RSHIFT:
      return InPlaceOpKind::kRShift;
    case NB_INPLACE_SUBTRACT:
      return InPlaceOpKind::kSubtract;
    case NB_INPLACE_TRUE_DIVIDE:
      return InPlaceOpKind::kTrueDivide;
    case NB_INPLACE_XOR:
      return InPlaceOpKind::kXor;
    default:
      return std::nullopt;
  }
}

void HIRBuilder::emitPushNull(TranslationContext& tc) {
  auto& stack = tc.frame.stack;
  Register* tmp = temps_.AllocateStack();
  tc.emit<LoadConst>(tmp, TNullptr);
  stack.push(tmp);
}

void HIRBuilder::emitAnyCall(
    CFG& cfg,
    TranslationContext& tc,
    jit::BytecodeInstructionBlock::Iterator& bc_it,
    const jit::BytecodeInstructionBlock& bc_instrs) {
  BytecodeInstruction bc_instr = *bc_it;
  auto flags = CallFlags::None;

  auto opcode = bc_instr.opcode();
  switch (opcode) {
    case CALL_FUNCTION:
    case CALL_FUNCTION_KW: {
      // Operands include the function arguments plus the function itself.
      auto num_operands = static_cast<std::size_t>(bc_instr.oparg()) + 1;
      // Add one more operand for the kwnames tuple at the end.
      if (opcode == CALL_FUNCTION_KW) {
        num_operands++;
        flags |= CallFlags::KwArgs;
      }
      tc.emitVariadic<VectorCall>(temps_, num_operands, flags);
      break;
    }
    case CALL_FUNCTION_EX: {
      emitCallEx(tc, bc_instr);
      break;
    }
    case CALL:
    case CALL_KW:
    case CALL_METHOD: {
      auto num_operands = static_cast<std::size_t>(bc_instr.oparg()) + 2;
      auto num_stack_inputs = num_operands;
      bool is_call_kw = opcode == CALL_KW;
      if (kwnames_ != nullptr || is_call_kw) {
        if (is_call_kw) {
          num_stack_inputs++;
        }
        num_operands++;
        flags |= CallFlags::KwArgs;
      }

      // Manually set up the instruction instead of using emitVariadic.
      // kwnames_ isn't on the stack, but it has to be part of the operand
      // count.
      Register* out = temps_.AllocateStack();
      auto call = tc.emit<CallMethod>(num_operands, out, flags);
      for (auto i = num_stack_inputs; i > 0; i--) {
        Register* arg = tc.frame.stack.pop();
        call->SetOperand(i - 1, arg);
      }
      if (kwnames_ != nullptr) {
        JIT_CHECK(
            call->GetOperand(num_operands - 1) == nullptr,
            "Somehow already set the kwnames argument");
        call->SetOperand(num_operands - 1, kwnames_);
        kwnames_ = nullptr;
      }
      call->setFrameState(tc.frame);

      tc.frame.stack.push(out);
      break;
    }
    case INVOKE_FUNCTION: {
      emitInvokeFunction(tc, bc_instr);
      break;
    }
    case INVOKE_NATIVE: {
      emitInvokeNative(tc, bc_instr);
      break;
    }
    case INVOKE_METHOD: {
      emitInvokeMethod(tc, bc_instr);
      break;
    }
    default:
      BUILDER_THROW(
          "Unhandled call opcode {} ({})", opcodeName(opcode), opcode);
  }
}

void HIRBuilder::emitCallInstrinsic(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto oparg = bc_instr.oparg();
  auto num_operands = 1;

  Register* value = tc.frame.stack.pop();
  Register* res = temps_.AllocateStack();
  std::vector<Register*> args;
  if (bc_instr.opcode() == CALL_INTRINSIC_2) {
    JIT_CHECK(
        oparg <= MAX_INTRINSIC_2,
        "Invalid oparg for binary intrinsic function: {}",
        oparg);
    Register* value2 = tc.frame.stack.pop();
    args.push_back(value2);
    num_operands = 2;
  } else {
    JIT_CHECK(
        oparg <= MAX_INTRINSIC_1,
        "Invalid oparg for unary intrinsic function: {}",
        oparg);
  }
  args.push_back(value);
  tc.emit<CallIntrinsic>(num_operands, res, oparg, args);
  tc.frame.stack.push(res);
}

void HIRBuilder::emitResume(
    CFG& cfg,
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  if (bc_instr.oparg() >= 2) {
    return;
  }
  if (is_simple_leaf_function_) {
    return;
  }
  TranslationContext succ(cfg.AllocateBlock(), tc.frame);
  succ.emitSnapshot();
  insertRunPeriodicActivites(cfg, tc.block, succ.block, tc.frame);
  tc.block = succ.block;
}

void HIRBuilder::emitKwNames(
    TranslationContext& tc,
    const BytecodeInstruction& bc_instr) {
  auto index = bc_instr.oparg();
  auto consts_len = PyTuple_Size(code_->co_consts);
  JIT_CHECK(
      index < consts_len,
      "KW_NAMES index {} is greater than co_consts length {}",
      index,
      consts_len);
  JIT_CHECK(
      kwnames_ == nullptr,
      "Trying to save KW_NAMES({}) but previous kwnames_ value wasn't consumed "
      "by a CALL* opcode yet",
      index);

  kwnames_ = temps_.AllocateNonStack();
  tc.emit<LoadConst>(
      kwnames_, Type::fromObject(PyTuple_GET_ITEM(code_->co_consts, index)));
}

void HIRBuilder::emitBinaryOp(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto& stack = tc.frame.stack;
  Register* right = stack.pop();
  Register* left = stack.pop();
  Register* result = temps_.AllocateStack();

  int opcode = bc_instr.opcode();
  int oparg = bc_instr.oparg();
  // Exact int guards on specialized numeric opcodes work well for loop-hot
  // functions, but can be actively harmful for tiny mixed-numeric leaf helpers
  // like raytrace's Vector.dot(). Keep the int guards only for code objects
  // that actually contain a backedge, or simple numeric leaf helpers that are
  // likely to be inlined into such loops. Preserve float exact guards so
  // float-only leaf helpers can still lower to the existing unboxed fast paths.
  bool specialize_int_guards = !getConfig().backedge_gated_int_guards ||
      code_has_backedge_ || code_is_simple_numeric_leaf_;

#if PY_VERSION_HEX < 0x030C0000
  // CPython 3.11 reuses the same quickened BINARY_OP_* opcode for normal and
  // in-place opargs. The typed fast paths below are only valid for normal
  // binary operations; in-place operations must keep the generic semantics.
  bool use_binary_op_type_guards =
      opcode != BINARY_OP || getBinaryOpKindFromOparg(oparg).has_value();
#else
  constexpr bool use_binary_op_type_guards = true;
#endif

  if (getConfig().specialized_opcodes) {
    switch (bc_instr.specializedOpcode()) {
      case BINARY_OP_ADD_INT:
      case BINARY_OP_MULTIPLY_INT:
      case BINARY_OP_SUBTRACT_INT:
#if PY_VERSION_HEX < 0x030C0000
        if (use_binary_op_type_guards) {
#else
        if (specialize_int_guards) {
#endif
          tc.emit<GuardType>(left, TLongExact, left, tc.frame);
          tc.emit<GuardType>(right, TLongExact, right, tc.frame);
        }
        break;
      case BINARY_OP_ADD_FLOAT:
      case BINARY_OP_MULTIPLY_FLOAT:
      case BINARY_OP_SUBTRACT_FLOAT:
        if (use_binary_op_type_guards) {
          tc.emit<GuardType>(left, TFloatExact, left, tc.frame);
          tc.emit<GuardType>(right, TFloatExact, right, tc.frame);
        }
        break;
      case BINARY_OP_ADD_UNICODE:
        if (use_binary_op_type_guards) {
          tc.emit<GuardType>(left, TUnicodeExact, left, tc.frame);
          tc.emit<GuardType>(right, TUnicodeExact, right, tc.frame);
        }
        break;
      case BINARY_SUBSCR_DICT:
      case BINARY_OP_SUBSCR_DICT:
        tc.emit<GuardType>(left, TDictExact, left, tc.frame);
        break;
      case BINARY_SUBSCR_LIST_INT:
      case BINARY_OP_SUBSCR_LIST_INT:
        tc.emit<GuardType>(left, TListExact, left, tc.frame);
        tc.emit<GuardType>(right, TLongExact, right, tc.frame);
        break;
      case BINARY_SUBSCR_TUPLE_INT:
      case BINARY_OP_SUBSCR_TUPLE_INT:
        tc.emit<GuardType>(left, TTupleExact, left, tc.frame);
        tc.emit<GuardType>(right, TLongExact, right, tc.frame);
        break;
      default:
        break;
    }
  }

  BinaryOpKind op_kind;
  if (opcode == BINARY_OP) {
    auto opt_op_kind = getBinaryOpKindFromOparg(oparg);
    if (opt_op_kind) {
      op_kind = *opt_op_kind;
    } else {
      // BINARY_OP can also contain inplace opargs.
      auto inplace_opt_op_kind = getInPlaceOpKindFromOparg(oparg);
      JIT_CHECK(
          inplace_opt_op_kind.has_value(),
          "Unrecognized oparg for BINARY_OP: {}",
          oparg);
      InPlaceOpKind inplace_op_kind = *inplace_opt_op_kind;
      tc.emit<InPlaceOp>(result, inplace_op_kind, left, right, tc.frame);
      stack.push(result);
      return;
    }
  } else {
    auto opt_op_kind = getBinaryOpKindFromOpcode(opcode);
    JIT_CHECK(
        opt_op_kind.has_value(),
        "Unrecognized opcode {} ({}) for binary operation",
        opcode,
        opcodeName(opcode));
    op_kind = *opt_op_kind;
  }

  tc.emit<BinaryOp>(result, op_kind, left, right, tc.frame);
  stack.push(result);
}

void HIRBuilder::emitInPlaceOp(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto& stack = tc.frame.stack;
  Register* right = stack.pop();
  Register* left = stack.pop();
  Register* result = temps_.AllocateStack();
  int opcode = bc_instr.opcode();
  auto opt_op_kind = getInPlaceOpKindFromOpcode(opcode);
  JIT_CHECK(
      opt_op_kind.has_value(),
      "Unrecognized opcode {} ({}) for inplace operation",
      opcode,
      opcodeName(opcode));
  InPlaceOpKind op_kind = *opt_op_kind;
  tc.emit<InPlaceOp>(result, op_kind, left, right, tc.frame);
  stack.push(result);
}

static inline UnaryOpKind get_unary_op_kind(
    const jit::BytecodeInstruction& bc_instr) {
  auto opcode = bc_instr.opcode();
  switch (opcode) {
    case UNARY_NOT:
      return UnaryOpKind::kNot;

    case UNARY_NEGATIVE:
      return UnaryOpKind::kNegate;

    case UNARY_POSITIVE:
      return UnaryOpKind::kPositive;

    case UNARY_INVERT:
      return UnaryOpKind::kInvert;

    default:
      break;
  }
  JIT_THROW("Unhandled unary op {} ({})", opcodeName(opcode), opcode);
}

void HIRBuilder::emitUnaryNot(TranslationContext& tc) {
  Register* operand = tc.frame.stack.pop();
  Register* is_false = temps_.AllocateNonStack();
  Register* const_false = temps_.AllocateNonStack();
  Register* result = temps_.AllocateStack();
  tc.emit<LoadConst>(const_false, Type::fromObject(Py_False));
  tc.emit<PrimitiveCompare>(
      is_false, PrimitiveCompareOp::kEqual, const_false, operand);
  tc.emit<PrimitiveBoxBool>(result, is_false);
  tc.frame.stack.push(result);
}

void HIRBuilder::emitUnaryOp(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* operand = tc.frame.stack.pop();
  Register* result = temps_.AllocateStack();
  UnaryOpKind op_kind = get_unary_op_kind(bc_instr);
  tc.emit<UnaryOp>(result, op_kind, operand, tc.frame);
  tc.frame.stack.push(result);
}

void HIRBuilder::emitCallEx(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* dst = temps_.AllocateStack();
  OperandStack& stack = tc.frame.stack;
  // In 3.14+ we always have kwargs on the stack but it may be null.
  bool has_kwargs = (PY_VERSION_HEX >= 0x030E0000) || bc_instr.oparg() & 0x1;
  Register* kwargs = nullptr;
  auto flags = CallFlags::None;
  if (has_kwargs) {
    kwargs = stack.pop();
    flags |= CallFlags::KwArgs;
  } else {
    Register* nullp = temps_.AllocateNonStack();
    tc.emit<LoadConst>(nullp, TNullptr);
    kwargs = nullp;
  }
  Register* pargs = stack.pop();
  Register* func;
  // CALL_FUNCTION_EX has an unused value on the stack, starting with 3.12.
  // In 3.14 this swapped location.
  if constexpr (PY_VERSION_HEX >= 0x030E0000) {
    stack.pop();
    func = stack.pop();
  } else {
    func = stack.pop();
    stack.pop();
  }
  tc.emit<CallEx>(dst, func, pargs, kwargs, flags, tc.frame);
  stack.push(dst);
}

void HIRBuilder::emitBuildSlice(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  std::size_t num_operands = static_cast<std::size_t>(bc_instr.oparg());
  tc.emitVariadic<BuildSlice>(temps_, num_operands);
}

void HIRBuilder::emitListAppend(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto item = tc.frame.stack.pop();
  auto list = tc.frame.stack.peek(bc_instr.oparg());
  auto dst = temps_.AllocateStack();
  tc.emit<ListAppend>(dst, list, item, tc.frame);
}

void HIRBuilder::emitLoadIterableArg(
    CFG& cfg,
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto iterable = tc.frame.stack.top();
  if (iterable->type() != TTupleExact) {
    TranslationContext tuple_path{cfg.AllocateBlock(), tc.frame};
    tuple_path.emitSnapshot();
    TranslationContext non_tuple_path{cfg.AllocateBlock(), tc.frame};
    non_tuple_path.emitSnapshot();
    tc.emit<CondBranchCheckType>(
        iterable, TTuple, tuple_path.block, non_tuple_path.block);
    tc.block = cfg.AllocateBlock();
    Register* tuple = temps_.AllocateStack();
    tc.frame.stack.topPut(0, tuple);
    tc.emitSnapshot();

    tuple_path.emit<Assign>(tuple, iterable);
    tuple_path.emit<Branch>(tc.block);

    non_tuple_path.emit<GetTuple>(tuple, iterable, non_tuple_path.frame);
    non_tuple_path.emit<Branch>(tc.block);
  }

  auto tuple = tc.frame.stack.pop();
  auto tmp = temps_.AllocateStack();
  auto tup_idx = temps_.AllocateStack();
  auto element = temps_.AllocateStack();
  tc.emit<LoadConst>(tmp, Type::fromCInt(bc_instr.oparg(), TCInt64));
  tc.emit<PrimitiveBox>(tup_idx, tmp, TCInt64, tc.frame);
  tc.emit<BinaryOp>(
      element, BinaryOpKind::kSubscript, tuple, tup_idx, tc.frame);
  tc.frame.stack.push(element);
  tc.frame.stack.push(tuple);
}

bool HIRBuilder::tryEmitDirectMethodCall(
    const InvokeTarget& target,
    TranslationContext& tc,
    long nargs) {
  if (target.is_statically_typed || nargs == target.builtin_expected_nargs) {
    Instr* staticCall;
    Register* out = nullptr;
    if (target.builtin_returns_void) {
      staticCall = tc.emit<CallStaticRetVoid>(nargs, target.builtin_c_func);
    } else {
      out = temps_.AllocateStack();
      Type ret_type =
          target.builtin_returns_error_code ? TCInt32 : target.return_type;
      staticCall =
          tc.emit<CallStatic>(nargs, out, target.builtin_c_func, ret_type);
    }

    auto& stack = tc.frame.stack;
    for (auto i = nargs - 1; i >= 0; i--) {
      Register* operand = stack.pop();
      staticCall->SetOperand(i, operand);
    }

    if (target.builtin_returns_error_code) {
      tc.emit<CheckNeg>(out, out, tc.frame);
    } else if (out != nullptr && !(target.return_type.couldBe(TPrimitive))) {
      tc.emit<CheckExc>(out, out, tc.frame);
    }
    if (target.builtin_returns_void || target.builtin_returns_error_code) {
      // We could update the compiler so that void returning functions either
      // are only used in void contexts, or explicitly emit a LOAD_CONST None
      // when not used in a void context. For now we just produce None here (and
      // in _PyClassLoader_ConvertRet).
      Register* tmp = temps_.AllocateStack();
      tc.emit<LoadConst>(tmp, TNoneType);
      stack.push(tmp);
    } else {
      stack.push(out);
    }
    return true;
  }

  return false;
}

std::vector<Register*> HIRBuilder::setupStaticArgs(
    TranslationContext& tc,
    const InvokeTarget& target,
    long nargs,
    bool statically_invoked) {
  auto arg_regs = std::vector<Register*>(nargs, nullptr);

  for (auto i = nargs - 1; i >= 0; i--) {
    arg_regs[i] = tc.frame.stack.pop();
  }

  // If we have patched a function that accepts/returns primitives,
  // but we couldn't emit a direct x64 call, we have to box any primitive args
  if (!target.primitive_arg_types.empty() && !statically_invoked) {
    for (auto [argnum, type] : target.primitive_arg_types) {
      Register* reg = arg_regs.at(argnum);
      auto boxed_primitive_tmp = temps_.AllocateStack();
      boxPrimitive(tc, boxed_primitive_tmp, reg, type);
      arg_regs[argnum] = boxed_primitive_tmp;
    }
  }

  return arg_regs;
}

void HIRBuilder::fixStaticReturn(
    TranslationContext& tc,
    Register* ret_val,
    Type ret_type) {
  Type boxed_ret = ret_type;
  if (boxed_ret <= TPrimitive) {
    boxed_ret = boxed_ret.asBoxed();
  }
  if (getConfig().refine_static_python && boxed_ret < TObject) {
    tc.emit<RefineType>(ret_val, boxed_ret, ret_val);
  }

  // Since we are not doing an x64 call, we will get a boxed value; if the
  // function is supposed to return a primitive, we need to unbox it because
  // later code in the function will expect the primitive.
  if (ret_type <= TPrimitive) {
    unboxPrimitive(tc, ret_val, ret_val, ret_type);
  }
}

bool HIRBuilder::isStaticRand(const InvokeTarget& target) {
  return target.builtin_c_func == (void*)Ci_static_rand;
}

bool HIRBuilder::tryEmitStaticRandCall(
    const InvokeTarget& target,
    TranslationContext& tc,
    long nargs) {
  // Special case for static function call
  //     rand() -> int32
  //
  // This is a hack to support __static__.rand for now, since it's the most
  // common case. Eventually we'll get the typed method def support into
  // upstream CPython or CinderX and then we'll be able to have generic strongly
  // typed methods.

  if (nargs != 0) {
    return false;
  }

  Register* out = temps_.AllocateStack();
  Type ret_type = TCInt32;
  // Ci_static_rand() boxes the return value; call rand() directly instead.
  tc.emit<CallStatic>(nargs, out, (void*)rand, ret_type);
  tc.frame.stack.push(out);
  return true;
}

void HIRBuilder::emitInvokeFunction(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  BorrowedRef<> arg = constArg(bc_instr);
  BorrowedRef<> descr = PyTuple_GET_ITEM(arg.get(), 0);
  long nargs = PyLong_AsLong(PyTuple_GET_ITEM(arg.get(), 1));

  const InvokeTarget& target = preloader_.invokeFunctionTarget(descr);

  // Hack to support a static type signature for __static__.rand(). Since we
  // don't have typed method defs in 3.12 we special case it here, by ignoring
  // all the metadata generated by the compiler pipeline and simply checking
  // that we are calling the Ci_static_rand function.
  if (isStaticRand(target) && tryEmitStaticRandCall(target, tc, nargs)) {
    return;
  }

  Register* funcreg = temps_.AllocateStack();

  if (target.container_is_immutable) {
    // try to emit a direct x64 call (InvokeStaticFunction/CallStatic) if we can

    if (target.isFunction() && target.is_statically_typed) {
      // Direct invoke is safe whether we succeeded in JIT-compiling or not,
      // it'll just have an extra indirection if not JIT compiled.
      Register* out = temps_.AllocateStack();
      Type typ = target.return_type;
      tc.emit<LoadConst>(funcreg, Type::fromObject(target.callable));

      auto call =
          tc.emit<InvokeStaticFunction>(nargs + 1, out, target.func(), typ);

      call->SetOperand(0, funcreg);

      for (auto i = nargs - 1; i >= 0; i--) {
        Register* operand = tc.frame.stack.pop();
        call->SetOperand(i + 1, operand);
      }
      call->setFrameState(tc.frame);

      tc.frame.stack.push(out);

      return;
    } else if (
        target.isBuiltin() && tryEmitDirectMethodCall(target, tc, nargs)) {
      return;
    }
    // we couldn't emit an x64 call, but we know what object we'll vectorcall,
    // so load it directly
    tc.emit<LoadConst>(funcreg, Type::fromObject(target.callable));
  } else {
    // The target is patchable so we have to load it indirectly
    tc.emit<LoadFunctionIndirect>(
        target.indirect_ptr, descr, funcreg, tc.frame);
  }

  std::vector<Register*> arg_regs =
      setupStaticArgs(tc, target, nargs, false /*statically_invoked*/);

  Register* out = temps_.AllocateStack();
  auto flags = CallFlags::None;
  if (target.container_is_immutable) {
    flags |= CallFlags::Static;
  }

  // Add one for the function argument.
  auto call = tc.emit<VectorCall>(nargs + 1, out, flags);
  for (auto i = 0; i < nargs; i++) {
    call->SetOperand(i + 1, arg_regs.at(i));
  }
  call->SetOperand(0, funcreg);
  call->setFrameState(tc.frame);

  fixStaticReturn(tc, out, target.return_type);
  tc.frame.stack.push(out);
}

void HIRBuilder::emitInvokeNative(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  BorrowedRef<> arg = constArg(bc_instr);
  BorrowedRef<> native_target_descr = PyTuple_GET_ITEM(arg.get(), 0);
  const NativeTarget& target =
      preloader_.invokeNativeTarget(native_target_descr);

  BorrowedRef<> signature = PyTuple_GET_ITEM(arg.get(), 1);

  // The last entry in the signature is the return type, so subtract 1
  Py_ssize_t nargs = PyTuple_GET_SIZE(signature.get()) - 1;

  Register* out = temps_.AllocateStack();
  Type typ = target.return_type;
  auto call = tc.emit<CallStatic>(nargs, out, target.callable, typ);
  for (auto i = nargs - 1; i >= 0; i--) {
    Register* operand = tc.frame.stack.pop();
    call->SetOperand(i, operand);
  }

  tc.frame.stack.push(out);
}

void HIRBuilder::emitInvokeMethodVectorCall(
    TranslationContext& tc,
    std::vector<Register*>& arg_regs,
    const InvokeTarget& target) {
  Register* out = temps_.AllocateStack();

  auto vectorCall = tc.emit<VectorCall>(arg_regs.size(), out, CallFlags::None);
  for (auto i = 0; i < arg_regs.size(); i++) {
    vectorCall->SetOperand(i, arg_regs.at(i));
  }
  vectorCall->setFrameState(tc.frame);

  fixStaticReturn(tc, out, target.return_type);
  tc.frame.stack.push(out);
}

void HIRBuilder::emitLoadMethodStatic(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  BorrowedRef<> arg = constArg(bc_instr);
  BorrowedRef<> descr = PyTuple_GET_ITEM(arg.get(), 0);
  bool is_classmethod = _PyClassLoader_IsClassMethodDescr(arg.get());

  const InvokeTarget& target = preloader_.invokeMethodTarget(descr);

  Register* self = tc.frame.stack.pop();
  auto type = temps_.AllocateStack();
  if (!is_classmethod) {
    tc.emit<LoadField>(
        type, self, "ob_type", offsetof(PyObject, ob_type), TType);
  } else {
    type = self;
  }

  Register* vtable = temps_.AllocateNonStack();
  Register* func_obj = temps_.AllocateNonStack();

  tc.emit<LoadField>(
      vtable, type, "tp_cache", offsetof(PyTypeObject, tp_cache), TObject);
  size_t entry_offset = offsetof(_PyType_VTable, vt_entries) +
      target.slot * sizeof(_PyType_VTableEntry);

  tc.emit<LoadField>(
      func_obj,
      vtable,
      "vte_state",
      entry_offset + offsetof(_PyType_VTableEntry, vte_state),
      TObject);

  // If this is natively callable then we'll want to get load_func for
  // the dispatch later. Otherwise we'll just vectorcall to the function.
  Register* entry_func = temps_.AllocateNonStack();
  Register* vtable_load = temps_.AllocateNonStack();

  tc.emit<LoadField>(
      vtable_load,
      vtable,
      "vte_load",
      entry_offset + offsetof(_PyType_VTableEntry, vte_load),
      TCPtr);

  auto call = tc.emit<CallInd>(
      3, func_obj, "vte_load", TOptObject, vtable_load, func_obj, self);
  call->setFrameState(tc.frame);

  if (target.is_statically_typed) {
    // the entry func isn't used by the interpreter and can't be de-opted but
    // we can have a LOAD_METHOD_STATIC that has another LOAD_METHOD_STATIC
    // before we get to the invokes.
    tc.emit<GetSecondOutput>(entry_func, TCPtr, func_obj);

    static_method_stack_.push(entry_func);
  }

  tc.frame.stack.push(func_obj);
  tc.frame.stack.push(self);
}

void HIRBuilder::emitInvokeMethod(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  BorrowedRef<> arg = constArg(bc_instr);
  BorrowedRef<> descr = PyTuple_GET_ITEM(arg.get(), 0);
  long nargs = PyLong_AsLong(PyTuple_GET_ITEM(arg.get(), 1)) + 2; // thunk, self

  const InvokeTarget& target = preloader_.invokeMethodTarget(descr);

  if (target.isBuiltin() && tryEmitDirectMethodCall(target, tc, nargs - 1)) {
    auto res = tc.frame.stack.pop();
    tc.frame.stack.pop(); // pop the thunk
    tc.frame.stack.push(res);
    return;
  }

  std::vector<Register*> arg_regs =
      setupStaticArgs(tc, target, nargs, target.is_statically_typed);

  if (target.is_statically_typed) {
    Register* out = temps_.AllocateNonStack();
    auto entry = static_method_stack_.pop();
    auto invoke =
        tc.emit<CallInd>(nargs + 1, out, "vtable invoke", target.return_type);
    invoke->SetOperand(0, entry);
    for (size_t i = 0; i < arg_regs.size(); i++) {
      invoke->SetOperand(i + 1, arg_regs[i]);
    }

    invoke->setFrameState(tc.frame);
    tc.frame.stack.push(out);
  } else {
    emitInvokeMethodVectorCall(tc, arg_regs, target);
  }
}

void HIRBuilder::emitIsOp(TranslationContext& tc, int oparg) {
  auto& stack = tc.frame.stack;
  Register* right = stack.pop();
  Register* left = stack.pop();
  Register* unboxed_result = temps_.AllocateStack();
  Register* result = temps_.AllocateStack();
  auto op =
      oparg == 0 ? PrimitiveCompareOp::kEqual : PrimitiveCompareOp::kNotEqual;
  tc.emit<PrimitiveCompare>(unboxed_result, op, left, right);
  tc.emit<PrimitiveBoxBool>(result, unboxed_result);
  stack.push(result);
}

void HIRBuilder::emitContainsOp(TranslationContext& tc, int oparg) {
  auto& stack = tc.frame.stack;
  Register* right = stack.pop();
  Register* left = stack.pop();
  Register* result = temps_.AllocateStack();
  CompareOp op = oparg == 0 ? CompareOp::kIn : CompareOp::kNotIn;
  tc.emit<Compare>(result, op, left, right, tc.frame);
  stack.push(result);
}

void HIRBuilder::emitCompareOp(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  int compare_op = bc_instr.oparg();

  if constexpr (PY_VERSION_HEX < 0x030C0000) {
    // CPython 3.11 uses the raw Py_LT..Py_GE values in COMPARE_OP. CPython
    // 3.12+ packs the compare op with additional flag bits.
  } else if constexpr (PY_VERSION_HEX >= 0x030E0000) {
    compare_op >>= 5;
  } else {
    compare_op >>= 4;
  }

  JIT_CHECK(compare_op >= Py_LT, "Invalid op {}", compare_op);
  JIT_CHECK(compare_op <= Py_GE, "Invalid op {}", compare_op);
  auto& stack = tc.frame.stack;
  Register* right = stack.pop();
  Register* left = stack.pop();
  Register* result = temps_.AllocateStack();
  CompareOp op = static_cast<CompareOp>(compare_op);
  bool specialize_int_guards = !getConfig().backedge_gated_int_guards ||
      code_has_backedge_ || code_is_simple_numeric_leaf_;
  if (getConfig().specialized_opcodes) {
    switch (bc_instr.specializedOpcode()) {
      case COMPARE_OP_FLOAT:
        tc.emit<GuardType>(left, TFloatExact, left, tc.frame);
        tc.emit<GuardType>(right, TFloatExact, right, tc.frame);
        break;
      case COMPARE_OP_INT:
        if (specialize_int_guards) {
          tc.emit<GuardType>(left, TLongExact, left, tc.frame);
          tc.emit<GuardType>(right, TLongExact, right, tc.frame);
        }
        break;
      case COMPARE_OP_STR:
        tc.emit<GuardType>(left, TUnicodeExact, left, tc.frame);
        tc.emit<GuardType>(right, TUnicodeExact, right, tc.frame);
        break;
      default:
        break;
    }
  }

  tc.emit<Compare>(result, op, left, right, tc.frame);
  stack.push(result);
  if (PY_VERSION_HEX >= 0x030E0000 && bc_instr.oparg() & 0x10) {
    emitToBool(tc);
  }
}

void HIRBuilder::emitToBool(
    TranslationContext& tc,
    const jit::BytecodeInstruction* bc_instr) {
  FrameState deopt_frame = tc.frame;
  Register* operand = tc.frame.stack.pop();

  if (bc_instr != nullptr && getConfig().specialized_opcodes) {
    switch (bc_instr->specializedOpcode()) {
#if PY_VERSION_HEX >= 0x030E0000
      case TO_BOOL_BOOL:
        tc.emit<GuardType>(operand, TBool, operand, deopt_frame);
        tc.emit<UseType>(operand, TBool);
        tc.frame.stack.push(operand);
        return;
      case TO_BOOL_INT:
        tc.emit<GuardType>(operand, TLongExact, operand, deopt_frame);
        tc.emit<UseType>(operand, TLongExact);
        break;
      case TO_BOOL_LIST:
        tc.emit<GuardType>(operand, TListExact, operand, deopt_frame);
        tc.emit<UseType>(operand, TListExact);
        break;
      case TO_BOOL_STR:
        tc.emit<GuardType>(operand, TUnicodeExact, operand, deopt_frame);
        tc.emit<UseType>(operand, TUnicodeExact);
        break;
#endif
      default:
        break;
    }
  }

  Register* truthy_result = temps_.AllocateStack();
  tc.emit<IsTruthy>(truthy_result, operand, tc.frame);

  Register* coerced_result = temps_.AllocateStack();
  tc.emit<PrimitiveBoxBool>(coerced_result, truthy_result);
  tc.frame.stack.push(coerced_result);
}

void HIRBuilder::emitCopyDictWithoutKeys(TranslationContext& tc) {
  auto& stack = tc.frame.stack;
  Register* keys = stack.top();
  Register* subject = stack.top(1);
  Register* rest = temps_.AllocateStack();
  tc.emit<CopyDictWithoutKeys>(rest, subject, keys, tc.frame);
  stack.topPut(0, rest);
}

void HIRBuilder::emitGetLen(TranslationContext& tc) {
  FrameState state = tc.frame;
  auto& stack = tc.frame.stack;
  Register* obj = stack.top();
  Register* result = temps_.AllocateStack();
  tc.emit<GetLength>(result, obj, state);
  stack.push(result);
}

void HIRBuilder::emitJumpIf(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* var = tc.frame.stack.top();

  BCOffset true_offset, false_offset;
  bool check_truthy = true;
  auto opcode = bc_instr.opcode();
  switch (opcode) {
    case JUMP_IF_NONZERO_OR_POP:
      check_truthy = false;
      [[fallthrough]];
    case JUMP_IF_TRUE_OR_POP: {
      true_offset = bc_instr.getJumpTarget();
      false_offset = bc_instr.nextInstrOffset();
      break;
    }
    case JUMP_IF_ZERO_OR_POP:
      check_truthy = false;
      [[fallthrough]];
    case JUMP_IF_FALSE_OR_POP: {
      false_offset = bc_instr.getJumpTarget();
      true_offset = bc_instr.nextInstrOffset();
      break;
    }
    default: {
      BUILDER_THROW(
          "Trying to translate non-jump-if bytecode {} ({})",
          opcodeName(opcode),
          opcode);
    }
  }

  BasicBlock* true_block = getBlockAtOff(true_offset);
  BasicBlock* false_block = getBlockAtOff(false_offset);

  if (check_truthy) {
    Register* tval = temps_.AllocateNonStack();
    // Registers that hold the result of `IsTruthy` are guaranteed to never be
    // the home of a value left on the stack at the end of a basic block, so we
    // don't need to worry about potentially storing a PyObject in them.
    tc.emit<IsTruthy>(tval, var, tc.frame);
    tc.emit<CondBranch>(tval, true_block, false_block);
  } else {
    tc.emit<CondBranch>(var, true_block, false_block);
  }
}

void HIRBuilder::emitDeleteAttr(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* receiver = tc.frame.stack.pop();
  tc.emit<DeleteAttr>(receiver, bc_instr.oparg(), tc.frame);
}

Register* HIRBuilder::emitSlotTypeVersionMatches(
    TranslationContext& tc,
    Register* receiver,
    uint32_t type_version) {
  Register* obj_type = temps_.AllocateStack();
  tc.emit<LoadField>(
      obj_type, receiver, "ob_type", offsetof(PyObject, ob_type), TType);

  Register* version = temps_.AllocateStack();
  tc.emit<LoadField>(
      version,
      obj_type,
      "tp_version_tag",
      offsetof(PyTypeObject, tp_version_tag),
      TCUInt32);

  Register* expected = temps_.AllocateStack();
  tc.emit<LoadConst>(expected, Type::fromCUInt(type_version, TCUInt32));

  Register* matches = temps_.AllocateStack();
  tc.emit<PrimitiveCompare>(
      matches, PrimitiveCompareOp::kEqual, version, expected);
  return matches;
}

void HIRBuilder::emitSlotTypeVersionGuard(
    TranslationContext& tc,
    Register* receiver,
    uint32_t type_version,
    const char* descr) {
  Register* matches = emitSlotTypeVersionMatches(tc, receiver, type_version);
  auto* guard = tc.emit<Guard>(matches, tc.frame);
  guard->setGuiltyReg(receiver);
  guard->setDescr(descr);
}

void HIRBuilder::emitLoadAttr(
    CFG& cfg,
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  int oparg = bc_instr.oparg();
  int name_idx = loadAttrIndex(oparg);

  // Starting in CPython 3.12, LOAD_METHOD is merged into LOAD_ATTR and the low
  // bit of the oparg selects method mode. CPython 3.11 still has a separate
  // LOAD_METHOD opcode, so LOAD_ATTR's oparg is a raw name index there.
#if PY_VERSION_HEX >= 0x030C0000
  bool is_method = (oparg & 1) != 0;
#else
  // 3.11 keeps a separate LOAD_METHOD opcode, so LOAD_ATTR's oparg is a raw
  // name index and this path is never method-mode.
  bool is_method = false;
#endif
  int specialized_opcode = getConfig().specialized_opcodes
      ? bc_instr.specializedOpcode()
      : LOAD_ATTR;
  bool slot_fast_path_enabled = specialized_opcode == LOAD_ATTR_SLOT &&
      bc_instr.attrCacheIndex() >= sizeof(PyObject);
#if PY_VERSION_HEX >= 0x030E0000
  bool method_with_values_fast_path_enabled =
      specialized_opcode == LOAD_ATTR_METHOD_WITH_VALUES;
#else
  bool method_with_values_fast_path_enabled = false;
#endif
#if PY_VERSION_HEX < 0x030C0000
  // The slot fast path needs the 3.14 type-version cache; it is out of scope
  // on 3.11 and stays off (see the attrSlot* stubs above).
  slot_fast_path_enabled = false;
#endif
#ifdef Py_GIL_DISABLED
  slot_fast_path_enabled = false;
  method_with_values_fast_path_enabled = false;
#endif
  if (is_method && !slot_fast_path_enabled &&
      !method_with_values_fast_path_enabled) {
    emitLoadMethod(tc, name_idx);
    return;
  }

  Register* receiver = tc.frame.stack.pop();

#if PY_VERSION_HEX < 0x030C0000
  if (tryEmitLoadAttrInstanceValue311(tc, bc_instr, receiver, name_idx)) {
    return;
  }
#endif

  if (getConfig().specialized_opcodes) {
    switch (specialized_opcode) {
      case LOAD_ATTR_MODULE: {
        Type type = Type::fromTypeExact(&PyModule_Type);
        auto guard = tc.emit<GuardType>(receiver, type, receiver, tc.frame);
        receiver = guard->output();
        break;
      }
#ifndef Py_GIL_DISABLED
      case LOAD_ATTR_SLOT: {
        if (!slot_fast_path_enabled) {
          break;
        }
        PyMemberDef* member_def =
            attrSlotMemberDef(code_, bc_instr, name_idx);
        if (member_def == nullptr) {
          if (is_method) {
            tc.frame.stack.push(receiver);
            emitLoadMethod(tc, name_idx);
            return;
          }
          break;
        }
        BorrowedRef<PyUnicodeObject> name =
            PyTuple_GET_ITEM(code_->co_names, name_idx);
        const char* field_name = PyUnicode_AsUTF8(name);
        if (field_name == nullptr) {
          PyErr_Clear();
          field_name = "<unknown>";
        }
        bool may_need_getattr_fallback = attrSlotTypeDefinesGetattr(bc_instr);
        Register* matches = emitSlotTypeVersionMatches(
            tc, receiver, bc_instr.attrCacheTypeVersion());
        BasicBlock* fast_path = cfg.AllocateBlock();
        BasicBlock* slow_path = cfg.AllocateBlock();
        BasicBlock* done = cfg.AllocateBlock();
        Register* result = temps_.AllocateStack();
        tc.emit<CondBranch>(matches, fast_path, slow_path);

        tc.block = fast_path;
        Register* field = temps_.AllocateStack();
        tc.emit<LoadField>(
            field,
            receiver,
            field_name,
            member_def->offset,
            TOptObject);
        if (member_def->type == T_OBJECT_EX) {
          if (may_need_getattr_fallback) {
            BasicBlock* set_block = cfg.AllocateBlock();
            BasicBlock* fallback_block = cfg.AllocateBlock();

            tc.emit<CondBranch>(field, set_block, fallback_block);

            tc.block = set_block;
            Register* checked_result = temps_.AllocateNonStack();
            tc.emit<RefineType>(checked_result, TObject, field);
            tc.emit<Assign>(result, checked_result);
            tc.emit<Branch>(done);

            tc.block = fallback_block;
            Register* fallback = temps_.AllocateNonStack();
            tc.emit<LoadAttr>(fallback, receiver, name_idx, tc.frame);
            tc.emit<Assign>(result, fallback);
            tc.emit<Branch>(done);
          } else {
            auto* check = tc.emit<CheckField>(field, field, name, tc.frame);
            check->setGuiltyReg(receiver);
            tc.emit<Assign>(result, check->output());
            tc.emit<Branch>(done);
          }
        } else {
          BasicBlock* set_block = cfg.AllocateBlock();
          BasicBlock* none_block = cfg.AllocateBlock();

          tc.emit<CondBranch>(field, set_block, none_block);

          tc.block = set_block;
          Register* checked_result = temps_.AllocateNonStack();
          tc.emit<RefineType>(checked_result, TObject, field);
          tc.emit<Assign>(result, checked_result);
          tc.emit<Branch>(done);

          tc.block = none_block;
          Register* none = temps_.AllocateNonStack();
          tc.emit<LoadConst>(none, Type::fromObject(Py_None));
          tc.emit<Assign>(result, none);
          tc.emit<Branch>(done);
        }

        tc.block = slow_path;
        tc.emit<LoadAttr>(
            result,
            receiver,
            name_idx,
            tc.frame,
            /* already_optimized= */ true);
        tc.emit<Branch>(done);

        tc.block = done;
        tc.frame.stack.push(result);
        if (is_method) {
          emitPushNull(tc);
        }
        return;
      }
#endif
#if PY_VERSION_HEX >= 0x030E0000
      case LOAD_ATTR_METHOD_WITH_VALUES: {
        JIT_DCHECK(
            is_method,
            "LOAD_ATTR_METHOD_WITH_VALUES must be a method-style LOAD_ATTR");
        Register* type_version = temps_.AllocateStack();
        Register* keys_version = temps_.AllocateStack();
        Register* descr = temps_.AllocateStack();
        Register* name = temps_.AllocateStack();
        Register* raw_result = temps_.AllocateStack();
        Register* result = temps_.AllocateStack();
        Register* method_instance = temps_.AllocateStack();
        tc.emit<LoadConst>(
            type_version,
            Type::fromCInt(bc_instr.cacheU32(2), TCInt64));
        tc.emit<LoadConst>(
            keys_version,
            Type::fromCInt(bc_instr.cacheU32(4), TCInt64));
        tc.emit<LoadConst>(
            descr, Type::fromCPtr(readCacheObj(code_, bc_instr, 6)));
        tc.emit<LoadConst>(
            name, Type::fromObject(PyTuple_GET_ITEM(code_->co_names, name_idx)));
        tc.emit<CallStatic>(
            5,
            raw_result,
            reinterpret_cast<void*>(JITRT_LoadAttrMethodWithValues),
            TOptObject,
            receiver,
            type_version,
            keys_version,
            descr,
            name);
        tc.emit<CheckExc>(result, raw_result, tc.frame);
        tc.emit<GetSecondOutput>(method_instance, TOptObject, raw_result);
        tc.frame.stack.push(result);
        tc.frame.stack.push(method_instance);
        return;
      }
#endif
#if PY_VERSION_HEX < 0x030C0000
      case LOAD_ATTR_INSTANCE_VALUE: {
        BorrowedRef<PyTypeObject> owner_type = preloader_.methodOwnerType();
        if (owner_type != nullptr) {
          auto guard = tc.emit<GuardType>(
              receiver, Type::fromTypeExact(owner_type), receiver, tc.frame);
          receiver = guard->output();
        }
        break;
      }
#endif
      default:
        break;
    }
  }

  Register* result = temps_.AllocateStack();
  tc.emit<LoadAttr>(result, receiver, name_idx, tc.frame);
  tc.frame.stack.push(result);
}

bool HIRBuilder::tryEmitLoadAttrInstanceValue311(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr,
    Register* receiver,
    int name_idx) {
#if PY_VERSION_HEX < 0x030C0000
  if (!getConfig().specialized_opcodes ||
      bc_instr.specializedOpcode() != LOAD_ATTR_INSTANCE_VALUE) {
    return false;
  }

  const _Py_CODEUNIT* instr = codeUnit(code_) + bc_instr.opcodeIndex().value();
  auto cache = reinterpret_cast<const _PyAttrCache*>(instr + 1);
  uint32_t type_version = readCacheU32(cache->version);
  if (type_version == 0) {
    return false;
  }

  BorrowedRef<> name = PyTuple_GET_ITEM(code_->co_names, name_idx);
  Register* name_reg = temps_.AllocateNonStack();
  tc.emit<LoadConst>(name_reg, Type::fromObject(env_->addReference(name)));

  Register* type_version_reg = temps_.AllocateNonStack();
  tc.emit<LoadConst>(type_version_reg, Type::fromCUInt(type_version, TCUInt32));

  Register* index_reg = temps_.AllocateNonStack();
  tc.emit<LoadConst>(
      index_reg,
      Type::fromCUInt(static_cast<uint64_t>(cache->index), TCUInt64));

  Register* result = temps_.AllocateStack();
  auto call = tc.emit<CallStatic>(
      4,
      result,
      reinterpret_cast<void*>(JITRT_LoadAttrInstanceValueOrGeneric),
      TOptObject);
  call->SetOperand(0, receiver);
  call->SetOperand(1, name_reg);
  call->SetOperand(2, type_version_reg);
  call->SetOperand(3, index_reg);
  tc.emit<CheckExc>(result, result, tc.frame);
  tc.frame.stack.push(result);
  return true;
#else
  return false;
#endif
}

void HIRBuilder::emitLoadMethod(TranslationContext& tc, int name_idx) {
  Register* receiver = tc.frame.stack.pop();
  Register* result = temps_.AllocateStack();
  Register* method_instance = temps_.AllocateStack();
  tc.emit<LoadMethod>(result, receiver, name_idx, tc.frame);
  tc.emit<GetSecondOutput>(method_instance, TOptObject, result);
  tc.frame.stack.push(result);
  tc.frame.stack.push(method_instance);
}

void HIRBuilder::emitLoadMethod(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  if (tryEmitLoadMethodWithValues311(tc, bc_instr)) {
    return;
  }
  emitLoadMethod(tc, bc_instr.oparg());
}

bool HIRBuilder::tryEmitLoadMethodWithValues311(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
#if PY_VERSION_HEX < 0x030C0000
  if (!getConfig().specialized_opcodes ||
      bc_instr.specializedOpcode() != LOAD_METHOD_WITH_VALUES) {
    return false;
  }

  const _Py_CODEUNIT* instr = codeUnit(code_) + bc_instr.opcodeIndex().value();
  auto cache = reinterpret_cast<const _PyLoadMethodCache*>(instr + 1);
  uint32_t type_version = readCacheU32(cache->type_version);
  uint32_t keys_version = readCacheU32(cache->keys_version);
  if (type_version == 0 || keys_version == 0) {
    return false;
  }

  PyObject* descr = readCacheObj(cache->descr);
  if (descr == nullptr || !PyFunction_Check(descr)) {
    return false;
  }

  auto func = reinterpret_cast<PyFunctionObject*>(descr);
  BorrowedRef<PyTypeObject> owner_type =
      resolveMethodOwnerType(func, preloader_.globals());
  if (owner_type == nullptr || owner_type->tp_version_tag != type_version ||
      !PyType_HasFeature(owner_type, Py_TPFLAGS_MANAGED_DICT)) {
    return false;
  }

  BorrowedRef<PyHeapTypeObject> heap_type{owner_type};
  PyDictKeysObject* keys = heap_type->ht_cached_keys;
  if (keys == nullptr || keys->dk_version != keys_version) {
    return false;
  }

  Register* receiver = tc.frame.stack.pop();
  Type expected_receiver_type = Type::fromTypeExact(owner_type);
  if (!receiver->isA(expected_receiver_type)) {
    auto guard = tc.emit<GuardType>(
        receiver, expected_receiver_type, receiver, tc.frame);
    receiver = guard->output();
  }

  auto emit_guard =
      [&](Register* condition, Register* guilty, const char* descr) {
        auto guard = tc.emit<Guard>(condition);
        guard->setFrameState(tc.frame);
        guard->setGuiltyReg(guilty);
        guard->setDescr(descr);
      };

  Register* owner = temps_.AllocateNonStack();
  tc.emit<LoadConst>(
      owner, Type::fromObject(env_->addReference(BorrowedRef<>{owner_type})));

  Register* current_type_version = temps_.AllocateNonStack();
  tc.emit<LoadField>(
      current_type_version,
      owner,
      "tp_version_tag",
      offsetof(PyTypeObject, tp_version_tag),
      TCUInt32);
  Register* expected_type_version = temps_.AllocateNonStack();
  tc.emit<LoadConst>(
      expected_type_version, Type::fromCUInt(type_version, TCUInt32));
  Register* type_version_matches = temps_.AllocateNonStack();
  tc.emit<PrimitiveCompare>(
      type_version_matches,
      PrimitiveCompareOp::kEqual,
      current_type_version,
      expected_type_version);
  emit_guard(type_version_matches, receiver, "LOAD_METHOD type version");

  Register* dict_ptr = temps_.AllocateNonStack();
  tc.emit<LoadField>(
      dict_ptr, receiver, "__dict__", -3 * sizeof(PyObject*), TCUInt64);
  Register* zero = temps_.AllocateNonStack();
  tc.emit<LoadConst>(zero, Type::fromCUInt(0, TCUInt64));
  Register* no_dict = temps_.AllocateNonStack();
  tc.emit<PrimitiveCompare>(
      no_dict, PrimitiveCompareOp::kEqual, dict_ptr, zero);
  emit_guard(no_dict, receiver, "LOAD_METHOD managed dict check");

  Register* keys_ptr = temps_.AllocateNonStack();
  tc.emit<LoadField>(
      keys_ptr,
      owner,
      "ht_cached_keys",
      offsetof(PyHeapTypeObject, ht_cached_keys),
      TCUInt64);
  Register* keys_obj = temps_.AllocateNonStack();
  tc.emit<BitCast>(keys_obj, keys_ptr, TOptObject);
  Register* current_keys_version = temps_.AllocateNonStack();
  tc.emit<LoadField>(
      current_keys_version,
      keys_obj,
      "dk_version",
      offsetof(PyDictKeysObject, dk_version),
      TCUInt32);
  Register* expected_keys_version = temps_.AllocateNonStack();
  tc.emit<LoadConst>(
      expected_keys_version, Type::fromCUInt(keys_version, TCUInt32));
  Register* keys_version_matches = temps_.AllocateNonStack();
  tc.emit<PrimitiveCompare>(
      keys_version_matches,
      PrimitiveCompareOp::kEqual,
      current_keys_version,
      expected_keys_version);
  emit_guard(keys_version_matches, receiver, "LOAD_METHOD keys version");

  Register* func_reg = temps_.AllocateStack();
  tc.emit<LoadConst>(
      func_reg,
      Type::fromObject(
          env_->addReference(BorrowedRef<PyFunctionObject>{func})));
  tc.frame.stack.push(func_reg);
  tc.frame.stack.push(receiver);
  return true;
#else
  return false;
#endif
}

void HIRBuilder::emitLoadMethodOrAttrSuper(
    CFG& cfg,
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr,
    bool load_method) {
  // Deopting from this instruction re-executes LOAD_SUPER_ATTR, which pops
  // its three inputs, so the deopt frame is captured before the pops.
  FrameState deopt_state{tc.frame};
  Register* receiver = tc.frame.stack.pop();
  Register* type = tc.frame.stack.pop();
  Register* global_super = tc.frame.stack.pop();

  int oparg = bc_instr.oparg();
  int name_idx = oparg >> 2;
  load_method = oparg & 1;
  bool no_args_in_super_call = !(oparg & 2);

  emitLoadMethodOrAttrSuper(
      cfg,
      tc,
      name_idx,
      global_super,
      type,
      receiver,
      load_method,
      no_args_in_super_call,
      bc_instr.baseOffset(),
      deopt_state);
}

void HIRBuilder::emitLoadMethodOrAttrSuper(
    CFG& cfg,
    TranslationContext& tc,
    int name_idx,
    Register* global_super,
    Register* type,
    Register* receiver,
    bool load_method,
    bool no_args_in_super_call,
    BCOffset deopt_off,
    const FrameState& deopt_state) {
  TranslationContext deopt_path{cfg.AllocateBlock(), deopt_state};
  Register* result = temps_.AllocateStack();

  // This is assumed to be a type object by the rest of the JIT.  Ideally it
  // would be typed by whatever pushes it onto the stack.
  deopt_path.frame.cur_instr_offs = deopt_off;
  deopt_path.emitSnapshot();
  deopt_path.emit<Deopt>();
  BasicBlock* fast_path = cfg.AllocateBlock();
  tc.emit<CondBranchCheckType>(type, TType, fast_path, deopt_path.block);
  tc.block = fast_path;
  tc.emit<RefineType>(type, TType, type);

  if (!load_method) {
    tc.emit<LoadAttrSuper>(
        result,
        global_super,
        type,
        receiver,
        name_idx,
        no_args_in_super_call,
        tc.frame);
    tc.frame.stack.push(result);
    return;
  }

  Register* method_instance = temps_.AllocateStack();
  tc.emit<LoadMethodSuper>(
      result,
      global_super,
      type,
      receiver,
      name_idx,
      no_args_in_super_call,
      tc.frame);
  tc.emit<GetSecondOutput>(method_instance, TOptObject, result);
  tc.frame.stack.push(result);
  tc.frame.stack.push(method_instance);
}

bool HIRBuilder::tryEmitLoadMethodOrAttrSuper311(
    CFG& cfg,
    TranslationContext& tc,
    jit::BytecodeInstructionBlock::Iterator& bc_it,
    const jit::BytecodeInstructionBlock& bc_block) {
  auto pattern = matchLoadSuperAttrPattern311(code_, bc_it, bc_block);
  if (!pattern.has_value()) {
    return false;
  }

  BytecodeInstruction load_global = *bc_it;
  Register* global_super = temps_.AllocateStack();
  tc.emit<LoadGlobal>(global_super, pattern->global_super_idx, tc.frame);

  Register* type = temps_.AllocateStack();
  Register* receiver = nullptr;

  if (pattern->no_args_in_super_call) {
    int class_idx = -1;
    for (int i = 0; i < numLocalsplus(code_); ++i) {
      PyObject* name = getVarname(code_, i);
      if (PyUnicode_CompareWithASCIIString(name, "__class__") == 0) {
        class_idx = i;
        break;
      }
    }
    if (class_idx < 0 || numLocals(code_) == 0) {
      return false;
    }

    Register* type_cell = tc.frame.localsplus[class_idx];
    tc.emit<LoadCellItem>(type, type_cell);
    BorrowedRef<> class_name = getVarname(code_, class_idx);
    if (class_idx < PyCode_GetFirstFree(code_)) {
      tc.emit<CheckVar>(type, type, class_name, tc.frame);
    } else {
      tc.emit<CheckFreevar>(type, type, class_name, tc.frame);
    }

    receiver = tc.frame.localsplus[0];
    BorrowedRef<> receiver_name = getVarname(code_, 0);
#if PY_VERSION_HEX < 0x030C0000
    _PyLocals_Kind receiver_kind =
        _PyLocals_GetKind(code_->co_localspluskinds, 0);
    if (receiver_kind & CO_FAST_CELL) {
      Register* receiver_cell = receiver;
      receiver = temps_.AllocateStack();
      tc.emit<LoadCellItem>(receiver, receiver_cell);
    }
#endif
    tc.emit<CheckVar>(receiver, receiver, receiver_name, tc.frame);
  } else {
    tc.emit<LoadGlobal>(type, pattern->type_global_idx, tc.frame);
    receiver = tc.frame.localsplus[pattern->receiver_local_idx];
    tc.emit<CheckVar>(
        receiver,
        receiver,
        getVarname(code_, pattern->receiver_local_idx),
        tc.frame);
  }

  // Deopting resumes at the LOAD_GLOBAL that begins the matched sequence;
  // none of the synthesized registers were pushed onto the operand stack, so
  // the current frame is the correct resume state.
  emitLoadMethodOrAttrSuper(
      cfg,
      tc,
      pattern->name_idx,
      global_super,
      type,
      receiver,
      pattern->load_method,
      pattern->no_args_in_super_call,
      load_global.baseOffset(),
      tc.frame);

  for (int i = 0; i < pattern->instrs_to_skip_after_super; ++i) {
    ++bc_it;
  }
  return true;
}

void HIRBuilder::emitMakeCell(TranslationContext& tc, int local_idx) {
  Register* local = tc.frame.localsplus[local_idx];
  Register* cell = temps_.AllocateNonStack();
  tc.emit<MakeCell>(cell, local, tc.frame);
  moveOverwrittenStackRegisters(tc, local);
  tc.emit<Assign>(local, cell);
}

void HIRBuilder::emitCopy(TranslationContext& tc, int item_idx) {
  JIT_CHECK(item_idx > 0, "The index ({}) must be positive!", item_idx);
  Register* item = tc.frame.stack.peek(item_idx);
  tc.frame.stack.push(item);
}

void HIRBuilder::emitCopyFreeVars(TranslationContext& tc, int nfreevars) {
  JIT_CHECK(nfreevars > 0, "Can't initialize {} freevars", nfreevars);
  JIT_CHECK(
      nfreevars == numFreevars(code_),
      "COPY_FREE_VARS oparg doesn't match the function's freevars tuple");
  JIT_CHECK(func_ != nullptr, "No func_ in function with freevars");

  Register* func_closure = temps_.AllocateNonStack();
  tc.emit<LoadField>(
      func_closure,
      func_,
      "func_closure",
      offsetof(PyFunctionObject, func_closure),
      TTuple);
  int offset = numLocalsplus(code_) - nfreevars;
  for (int i = 0; i < nfreevars; ++i) {
    Register* dst = tc.frame.localsplus[offset + i];
    JIT_CHECK(dst != nullptr, "No register for free var {}", i);
    tc.emit<LoadTupleItem>(dst, func_closure, i);
  }
  tc.emit<InitFrameCellVars>(func_, nfreevars);
}

void HIRBuilder::emitSwap(TranslationContext& tc, int item_idx) {
  JIT_CHECK(
      item_idx >= 2, "The index ({}) must be greater or equal to 2.", item_idx);
  Register* item = tc.frame.stack.peek(item_idx);
  Register* top = tc.frame.stack.top();
  tc.frame.stack.topPut(0, item);
  tc.frame.stack.topPut(item_idx - 1, top);
}

void HIRBuilder::emitLoadDeref(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  int idx = bc_instr.oparg();

  Register* src = tc.frame.localsplus[idx];
  Register* dst = temps_.AllocateStack();

  tc.emit<LoadCellItem>(dst, src);

  BorrowedRef<> name = getVarname(code_, idx);
  if (idx < PyCode_GetFirstFree(code_)) {
    tc.emit<CheckVar>(dst, dst, name, tc.frame);
  } else {
    tc.emit<CheckFreevar>(dst, dst, name, tc.frame);
  }

  tc.frame.stack.push(dst);
}

void HIRBuilder::emitStoreDeref(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  int idx = bc_instr.oparg();

  Register* old = temps_.AllocateStack();
  Register* dst = tc.frame.localsplus[idx];
  Register* src = tc.frame.stack.pop();
  if constexpr (kFreeThreadedBuild) {
    // Use atomic swap for thread-safe cell access in FT-Python.
    tc.emit<SwapCellItem>(old, dst, src);
  } else {
    tc.emit<StealCellItem>(old, dst);
    tc.emit<SetCellItem>(dst, src, old);
  }
}

void HIRBuilder::emitLoadAssertionError(
    TranslationContext& tc,
    Environment& env) {
  Register* result = temps_.AllocateStack();
  tc.emit<LoadConst>(
      result, Type::fromObject(env.addReference(PyExc_AssertionError)));
  tc.frame.stack.push(result);
}

void HIRBuilder::emitLoadClass(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  BorrowedRef<> descr = constArg(bc_instr);
  const OwnedType* type = preloader_.preloadedType(descr);
  if (type == nullptr) {
    BUILDER_THROW(
        "LOAD_CLASS: Cannot find type for type descr {}", repr(descr));
  }
  if (type->optional) {
    BUILDER_THROW("Cannot load optional class type {}", type->type->tp_name);
  }

  Register* tmp = temps_.AllocateStack();
  tc.emit<LoadConst>(tmp, Type::fromObject(type->type));
  tc.frame.stack.push(tmp);
}

void HIRBuilder::emitLoadConst(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* tmp = temps_.AllocateStack();
  JIT_CHECK(
      bc_instr.oparg() < PyTuple_Size(code_->co_consts),
      "LOAD_CONST index out of bounds");
  tc.emit<LoadConst>(
      tmp,
      Type::fromObject(PyTuple_GET_ITEM(code_->co_consts, bc_instr.oparg())));
  tc.frame.stack.push(tmp);
}

void HIRBuilder::emitLoadFast(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  int var_idx = bc_instr.oparg();
  Register* var = tc.frame.localsplus[var_idx];
#if PY_VERSION_HEX < 0x030C0000
  bool needs_unbound_check = bc_instr.opcode() == LOAD_FAST;
#else
  bool needs_unbound_check = bc_instr.opcode() == LOAD_FAST_CHECK;
#endif
  if (needs_unbound_check) {
    tc.emit<CheckVar>(var, var, getVarname(code_, var_idx), tc.frame);
  }
  tc.frame.stack.push(var);
  if (bc_instr.opcode() == LOAD_FAST_AND_CLEAR) {
    moveOverwrittenStackRegisters(tc, var);
    tc.emit<LoadConst>(var, TNullptr);
  }
}

void HIRBuilder::emitLoadFastLoadFast(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  int var_idx1 = bc_instr.oparg() >> 4;
  int var_idx2 = bc_instr.oparg() & 0xf;
  size_t localsplus_size = tc.frame.localsplus.size();
  JIT_CHECK(
      var_idx1 < localsplus_size && var_idx2 < localsplus_size,
      "LOAD_FAST_LOAD_FAST ({}, {}) out of bounds for localsplus array size {}",
      var_idx1,
      var_idx2,
      tc.frame.localsplus.size());
  Register* var1 = tc.frame.localsplus[var_idx1];
  tc.frame.stack.push(var1);

  Register* var2 = tc.frame.localsplus[var_idx2];
  tc.frame.stack.push(var2);
}

void HIRBuilder::emitLoadLocal(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  PyObject* index_and_descr =
      PyTuple_GET_ITEM(code_->co_consts, bc_instr.oparg());
  int index = PyLong_AsLong(PyTuple_GET_ITEM(index_and_descr, 0));

  auto var = tc.frame.localsplus[index];
  tc.frame.stack.push(var);
}

void HIRBuilder::emitLoadSmallInt(
    [[maybe_unused]] TranslationContext& tc,
    [[maybe_unused]] const jit::BytecodeInstruction& bc_instr) {
#if PY_VERSION_HEX >= 0x030E0000
  Register* tmp = temps_.AllocateStack();
  JIT_CHECK(
      bc_instr.oparg() < _PY_NSMALLPOSINTS, "LOAD_SMALL_INT out of range");
  tc.emit<LoadConst>(
      tmp,
      Type::fromObject(
          reinterpret_cast<PyObject*>(
              &_PyLong_SMALL_INTS[_PY_NSMALLNEGINTS + bc_instr.oparg()])));
  tc.frame.stack.push(tmp);
#else
  BUILDER_THROW("LOAD_SMALL_INT not supported on this Python version");
#endif
}

void HIRBuilder::emitStoreLocal(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* src = tc.frame.stack.pop();
  PyObject* index_and_descr =
      PyTuple_GET_ITEM(code_->co_consts, bc_instr.oparg());
  int index = PyLong_AsLong(PyTuple_GET_ITEM(index_and_descr, 0));
  auto dst = tc.frame.localsplus[index];
  moveOverwrittenStackRegisters(tc, dst);
  tc.emit<Assign>(dst, src);
}

void HIRBuilder::emitLoadType(
    TranslationContext& tc,
    const jit::BytecodeInstruction&) {
  Register* instance = tc.frame.stack.pop();
  auto type = temps_.AllocateStack();
  tc.emit<LoadField>(
      type, instance, "ob_type", offsetof(PyObject, ob_type), TType);
  tc.frame.stack.push(type);
}

void HIRBuilder::emitConvertPrimitive(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* val = tc.frame.stack.pop();
  Register* out = temps_.AllocateStack();
  Type to_type = prim_type_to_type(bc_instr.oparg() >> 4);
  tc.emit<PrimitiveConvert>(out, val, to_type);
  tc.frame.stack.push(out);
}

void HIRBuilder::emitPrimitiveLoadConst(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* tmp = temps_.AllocateStack();
  int index = bc_instr.oparg();
  JIT_CHECK(
      index < PyTuple_Size(code_->co_consts),
      "PRIMITIVE_LOAD_CONST index out of bounds");
  PyObject* num_and_type = PyTuple_GET_ITEM(code_->co_consts, index);
  JIT_CHECK(
      PyTuple_Size(num_and_type) == 2,
      "wrong size for PRIMITIVE_LOAD_CONST arg tuple")
  PyObject* num = PyTuple_GET_ITEM(num_and_type, 0);
  Type size =
      prim_type_to_type(PyLong_AsSsize_t(PyTuple_GET_ITEM(num_and_type, 1)));
  Type type = TBottom;
  if (size == TCDouble) {
    type = Type::fromCDouble(PyFloat_AsDouble(num));
  } else if (size <= TCBool) {
    type = Type::fromCBool(num == Py_True);
  } else {
    type = (size <= TCUnsigned)
        ? Type::fromCUInt(PyLong_AsUnsignedLong(num), size)
        : Type::fromCInt(PyLong_AsLong(num), size);
  }
  tc.emit<LoadConst>(tmp, type);
  tc.frame.stack.push(tmp);
}

void HIRBuilder::emitPrimitiveBox(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* tmp = temps_.AllocateStack();
  Register* src = tc.frame.stack.pop();
  Type typ = prim_type_to_type(bc_instr.oparg());
  boxPrimitive(tc, tmp, src, typ);
  tc.frame.stack.push(tmp);
}

void HIRBuilder::emitPrimitiveUnbox(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* tmp = temps_.AllocateStack();
  Register* src = tc.frame.stack.pop();
  Type typ = prim_type_to_type(bc_instr.oparg());
  unboxPrimitive(tc, tmp, src, typ);
  tc.frame.stack.push(tmp);
}

void HIRBuilder::boxPrimitive(
    TranslationContext& tc,
    Register* dst,
    Register* src,
    Type type) {
  if (type <= TCBool) {
    tc.emit<PrimitiveBoxBool>(dst, src);
  } else {
    tc.emit<PrimitiveBox>(dst, src, type, tc.frame);
  }
}

void HIRBuilder::unboxPrimitive(
    TranslationContext& tc,
    Register* dst,
    Register* src,
    Type type) {
  tc.emit<PrimitiveUnbox>(dst, src, type);
  if (!(type <= (TCBool | TCDouble))) {
    Register* did_unbox_work = temps_.AllocateStack();
    tc.emit<IsNegativeAndErrOccurred>(did_unbox_work, dst, tc.frame);
  }
}

static inline BinaryOpKind get_primitive_bin_op_kind(
    const jit::BytecodeInstruction& bc_instr) {
  switch (bc_instr.oparg()) {
    case PRIM_OP_ADD_DBL:
    case PRIM_OP_ADD_INT: {
      return BinaryOpKind::kAdd;
    }
    case PRIM_OP_AND_INT: {
      return BinaryOpKind::kAnd;
    }
    case PRIM_OP_DIV_INT: {
      return BinaryOpKind::kFloorDivide;
    }
    case PRIM_OP_DIV_UN_INT: {
      return BinaryOpKind::kFloorDivideUnsigned;
    }
    case PRIM_OP_LSHIFT_INT: {
      return BinaryOpKind::kLShift;
    }
    case PRIM_OP_MOD_INT: {
      return BinaryOpKind::kModulo;
    }
    case PRIM_OP_MOD_UN_INT: {
      return BinaryOpKind::kModuloUnsigned;
    }
    case PRIM_OP_MUL_DBL:
    case PRIM_OP_MUL_INT: {
      return BinaryOpKind::kMultiply;
    }
    case PRIM_OP_OR_INT: {
      return BinaryOpKind::kOr;
    }
    case PRIM_OP_RSHIFT_INT: {
      return BinaryOpKind::kRShift;
    }
    case PRIM_OP_RSHIFT_UN_INT: {
      return BinaryOpKind::kRShiftUnsigned;
    }
    case PRIM_OP_SUB_DBL:
    case PRIM_OP_SUB_INT: {
      return BinaryOpKind::kSubtract;
    }
    case PRIM_OP_XOR_INT: {
      return BinaryOpKind::kXor;
    }
    case PRIM_OP_DIV_DBL: {
      return BinaryOpKind::kTrueDivide;
    }
    case PRIM_OP_POW_UN_INT: {
      return BinaryOpKind::kPowerUnsigned;
    }
    case PRIM_OP_POW_INT:
    case PRIM_OP_POW_DBL: {
      return BinaryOpKind::kPower;
    }
    default: {
      JIT_THROW("Unhandled binary op {}", bc_instr.oparg());
    }
  }
}

static inline bool is_double_binop(int oparg) {
  switch (oparg) {
    case PRIM_OP_ADD_INT:
    case PRIM_OP_AND_INT:
    case PRIM_OP_DIV_INT:
    case PRIM_OP_DIV_UN_INT:
    case PRIM_OP_LSHIFT_INT:
    case PRIM_OP_MOD_INT:
    case PRIM_OP_MOD_UN_INT:
    case PRIM_OP_POW_INT:
    case PRIM_OP_POW_UN_INT:
    case PRIM_OP_MUL_INT:
    case PRIM_OP_OR_INT:
    case PRIM_OP_RSHIFT_INT:
    case PRIM_OP_RSHIFT_UN_INT:
    case PRIM_OP_SUB_INT:
    case PRIM_OP_XOR_INT: {
      return false;
    }
    case PRIM_OP_ADD_DBL:
    case PRIM_OP_SUB_DBL:
    case PRIM_OP_DIV_DBL:
    case PRIM_OP_MUL_DBL:
    case PRIM_OP_POW_DBL: {
      return true;
    }
    default: {
      JIT_THROW("Invalid binary op {}", oparg);
    }
  }
}

static inline Type element_type_from_seq_type(int seq_type) {
  switch (seq_type) {
    case SEQ_LIST:
    case SEQ_LIST_INEXACT:
    case SEQ_CHECKED_LIST:
    case SEQ_TUPLE:
      return TObject;
    case SEQ_ARRAY_INT64:
      return TCInt64;
    default:
      JIT_THROW("Invalid sequence type: ({})", seq_type);
  }
}

void HIRBuilder::emitPrimitiveBinaryOp(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto& stack = tc.frame.stack;
  Register* right = stack.pop();
  Register* left = stack.pop();
  Register* result = temps_.AllocateStack();

  BinaryOpKind op_kind = get_primitive_bin_op_kind(bc_instr);

  if (is_double_binop(bc_instr.oparg())) {
    tc.emit<DoubleBinaryOp>(result, op_kind, left, right);
  } else {
    tc.emit<IntBinaryOp>(result, op_kind, left, right);
  }

  stack.push(result);
}

void HIRBuilder::emitPrimitiveCompare(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto& stack = tc.frame.stack;
  Register* right = stack.pop();
  Register* left = stack.pop();
  Register* result = temps_.AllocateStack();
  PrimitiveCompareOp op;
  switch (bc_instr.oparg()) {
    case PRIM_OP_EQ_INT:
    case PRIM_OP_EQ_DBL:
      op = PrimitiveCompareOp::kEqual;
      break;
    case PRIM_OP_NE_INT:
    case PRIM_OP_NE_DBL:
      op = PrimitiveCompareOp::kNotEqual;
      break;
    case PRIM_OP_LT_INT:
      op = PrimitiveCompareOp::kLessThan;
      break;
    case PRIM_OP_LE_INT:
      op = PrimitiveCompareOp::kLessThanEqual;
      break;
    case PRIM_OP_GT_INT:
      op = PrimitiveCompareOp::kGreaterThan;
      break;
    case PRIM_OP_GE_INT:
      op = PrimitiveCompareOp::kGreaterThanEqual;
      break;
    case PRIM_OP_LT_UN_INT:
    case PRIM_OP_LT_DBL:
      op = PrimitiveCompareOp::kLessThanUnsigned;
      break;
    case PRIM_OP_LE_UN_INT:
    case PRIM_OP_LE_DBL:
      op = PrimitiveCompareOp::kLessThanEqualUnsigned;
      break;
    case PRIM_OP_GT_UN_INT:
    case PRIM_OP_GT_DBL:
      op = PrimitiveCompareOp::kGreaterThanUnsigned;
      break;
    case PRIM_OP_GE_UN_INT:
    case PRIM_OP_GE_DBL:
      op = PrimitiveCompareOp::kGreaterThanEqualUnsigned;
      break;
    default:
      BUILDER_THROW("Unsupported comparison oparg {}", bc_instr.oparg());
  }
  tc.emit<PrimitiveCompare>(result, op, left, right);
  stack.push(result);
}

void HIRBuilder::emitPrimitiveUnaryOp(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* value = tc.frame.stack.pop();
  Register* result = temps_.AllocateStack();
  PrimitiveUnaryOpKind op;
  switch (bc_instr.oparg()) {
    case PRIM_OP_NEG_INT: {
      op = PrimitiveUnaryOpKind::kNegateInt;
      tc.emit<PrimitiveUnaryOp>(result, op, value);
      break;
    }
    case PRIM_OP_INV_INT: {
      op = PrimitiveUnaryOpKind::kInvertInt;
      tc.emit<PrimitiveUnaryOp>(result, op, value);
      break;
    }
    case PRIM_OP_NOT_INT: {
      op = PrimitiveUnaryOpKind::kNotInt;
      tc.emit<PrimitiveUnaryOp>(result, op, value);
      break;
    }
    case PRIM_OP_NEG_DBL: {
      // For doubles, there's no easy way to unary negate a value, so just
      // multiply it by -1
      auto tmp = temps_.AllocateStack();
      tc.emit<LoadConst>(tmp, Type::fromCDouble(-1.0));
      tc.emit<DoubleBinaryOp>(result, BinaryOpKind::kMultiply, tmp, value);
      break;
    }
    default: {
      BUILDER_THROW("Unsupported unary op oparg {}", bc_instr.oparg());
    }
  }
  tc.frame.stack.push(result);
}

void HIRBuilder::emitFastLen(
    CFG& cfg,
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto result = temps_.AllocateStack();
  Register* collection;
  auto oparg = bc_instr.oparg();
  int inexact = oparg & FAST_LEN_INEXACT;
  std::size_t offset = 0;
  auto type = TBottom;

  oparg &= ~FAST_LEN_INEXACT;
  const char* name = "";
  if (oparg == FAST_LEN_LIST) {
    type = TListExact;
    offset = offsetof(PyVarObject, ob_size);
    name = "ob_size";
  } else if (oparg == FAST_LEN_TUPLE) {
    type = TTupleExact;
    offset = offsetof(PyVarObject, ob_size);
    name = "ob_size";
  } else if (oparg == FAST_LEN_ARRAY) {
    type = TArray;
    offset = offsetof(PyVarObject, ob_size);
    name = "ob_size";
  } else if (oparg == FAST_LEN_DICT) {
    type = TDictExact;
    offset = offsetof(PyDictObject, ma_used);
    name = "ma_used";
  } else if (oparg == FAST_LEN_SET) {
    type = TSetExact;
    offset = offsetof(PySetObject, used);
    name = "used";
  } else if (oparg == FAST_LEN_STR) {
    type = TUnicodeExact;
    // Note: In debug mode, the interpreter has an assert that
    // ensures the string is "ready", check PyUnicode_GET_LENGTH
    offset = offsetof(PyASCIIObject, length);
    name = "length";
  }
  JIT_CHECK(offset > 0, "Bad oparg for FAST_LEN");

  if (inexact) {
    TranslationContext deopt_path{cfg.AllocateBlock(), tc.frame};
    deopt_path.frame.cur_instr_offs = bc_instr.baseOffset();
    deopt_path.emitSnapshot();
    deopt_path.emit<Deopt>();
    collection = tc.frame.stack.pop();
    BasicBlock* fast_path = cfg.AllocateBlock();
    tc.emit<CondBranchCheckType>(collection, type, fast_path, deopt_path.block);
    tc.block = fast_path;
    // TASK(T105038867): Remove once we have RefineTypeInsertion
    tc.emit<RefineType>(collection, type, collection);
  } else {
    collection = tc.frame.stack.pop();
  }

  tc.emit<LoadField>(result, collection, name, offset, TCInt64);
  tc.frame.stack.push(result);
}

void HIRBuilder::emitRefineType(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  BorrowedRef<> descr = constArg(bc_instr);
  const OwnedType* type = preloader_.preloadedType(descr);
  if (type == nullptr) {
    BUILDER_THROW(
        "REFINE_TYPE: Can't find type for type descr {}", repr(descr));
  }

  Register* dst = tc.frame.stack.top();
  tc.emit<RefineType>(dst, type->toHir(), dst);
}

void HIRBuilder::emitSequenceGet(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto& stack = tc.frame.stack;
  auto idx = stack.pop();
  auto sequence = stack.pop();
  auto oparg = bc_instr.oparg();
  if (oparg == SEQ_LIST_INEXACT) {
    auto type = temps_.AllocateStack();
    tc.emit<LoadField>(
        type, sequence, "ob_type", offsetof(PyObject, ob_type), TType);
    tc.emit<GuardIs>(type, (PyObject*)&PyList_Type, type);
    tc.emit<RefineType>(sequence, TListExact, sequence);
  }

  Register* adjusted_idx;
  int unchecked = oparg & SEQ_SUBSCR_UNCHECKED;
  if (!unchecked) {
    adjusted_idx = temps_.AllocateStack();
    tc.emit<CheckSequenceBounds>(adjusted_idx, sequence, idx, tc.frame);
  } else {
    adjusted_idx = idx;
    oparg &= ~SEQ_SUBSCR_UNCHECKED;
  }
  auto ob_item = temps_.AllocateStack();
  auto result = temps_.AllocateStack();
  if (oparg == SEQ_LIST || oparg == SEQ_LIST_INEXACT ||
      oparg == SEQ_CHECKED_LIST) {
    int offset = offsetof(PyListObject, ob_item);
    tc.emit<LoadField>(ob_item, sequence, "ob_item", offset, TCPtr);
  } else if (oparg == SEQ_ARRAY_INT64) {
    Register* offset_reg = temps_.AllocateStack();
    tc.emit<LoadConst>(
        offset_reg,
        Type::fromCInt(offsetof(PyStaticArrayObject, ob_item), TCInt64));
    tc.emit<LoadFieldAddress>(ob_item, sequence, offset_reg);
  } else {
    BUILDER_THROW("Unsupported oparg for SEQUENCE_GET: {}", oparg);
  }

  auto type = element_type_from_seq_type(oparg);
  tc.emit<LoadArrayItem>(
      result, ob_item, adjusted_idx, sequence, /*offset=*/0, type);
  stack.push(result);
}

void HIRBuilder::emitSequenceSet(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto& stack = tc.frame.stack;
  auto idx = stack.pop();
  auto sequence = stack.pop();
  auto value = stack.pop();
  auto adjusted_idx = temps_.AllocateStack();
  auto oparg = bc_instr.oparg();
  if (oparg == SEQ_LIST_INEXACT) {
    auto type = temps_.AllocateStack();
    tc.emit<LoadField>(
        type, sequence, "ob_type", offsetof(PyObject, ob_type), TType);
    tc.emit<GuardIs>(type, (PyObject*)&PyList_Type, type);
    tc.emit<RefineType>(sequence, TListExact, sequence);
  }
  tc.emit<CheckSequenceBounds>(adjusted_idx, sequence, idx, tc.frame);
  auto ob_item = temps_.AllocateStack();
  if (oparg == SEQ_ARRAY_INT64) {
    Register* offset_reg = temps_.AllocateStack();
    tc.emit<LoadConst>(
        offset_reg,
        Type::fromCInt(offsetof(PyStaticArrayObject, ob_item), TCInt64));
    tc.emit<LoadFieldAddress>(ob_item, sequence, offset_reg);
  } else if (oparg == SEQ_LIST || oparg == SEQ_LIST_INEXACT) {
    int offset = offsetof(PyListObject, ob_item);
    tc.emit<LoadField>(ob_item, sequence, "ob_item", offset, TCPtr);
  } else {
    BUILDER_THROW("Unsupported oparg for SEQUENCE_SET: {}", oparg);
  }
  tc.emit<StoreArrayItem>(
      ob_item,
      adjusted_idx,
      value,
      sequence,
      element_type_from_seq_type(oparg));
}

#if PY_VERSION_HEX < 0x030C0000
bool HIRBuilder::tryEmitLoadGlobalModuleValue311(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr,
    int name_idx,
    Register* result) {
  if (!getConfig().stable_frame || !getConfig().specialized_opcodes ||
      bc_instr.opcode() != LOAD_GLOBAL) {
    return false;
  }

  BorrowedRef<PyDictObject> globals = preloader_.globals();
  if (globals->ma_values != nullptr || !hasOnlyUnicodeKeys(globals)) {
    return false;
  }

  BorrowedRef<> name = PyTuple_GET_ITEM(code_->co_names, name_idx);
  PyObject* value = PyDict_GetItemWithError(globals, name);
  if (value == nullptr) {
    if (PyErr_Occurred()) {
      PyErr_Clear();
    }
    return false;
  }

  Py_ssize_t index = -1;
  uint32_t keys_version = 0;
  if (bc_instr.specializedOpcode() == LOAD_GLOBAL_MODULE) {
    const _Py_CODEUNIT* instr =
        codeUnit(code_) + bc_instr.opcodeIndex().value();
    auto cache = reinterpret_cast<const _PyLoadGlobalCache*>(instr + 1);
    keys_version = readCacheU32(cache->module_keys_version);
    index = cache->index;
  } else {
    index = findActiveUnicodeDictEntryIndex(globals, name, value);
    if (index < 0) {
      return false;
    }
    keys_version = dictGetKeysVersion(nullptr, globals->ma_keys);
  }
  if (keys_version == 0) {
    return false;
  }

  Register* globals_reg = temps_.AllocateNonStack();
  tc.emit<LoadConst>(
      globals_reg,
      Type::fromObject(env_->addReference(BorrowedRef<>{globals})));

  Register* name_reg = temps_.AllocateNonStack();
  tc.emit<LoadConst>(name_reg, Type::fromObject(env_->addReference(name)));

  Register* keys_version_reg = temps_.AllocateNonStack();
  tc.emit<LoadConst>(keys_version_reg, Type::fromCUInt(keys_version, TCUInt32));

  Register* index_reg = temps_.AllocateNonStack();
  tc.emit<LoadConst>(index_reg, Type::fromCInt(index, TCInt64));

  auto call = tc.emit<CallStatic>(
      4,
      result,
      reinterpret_cast<void*>(JITRT_LoadGlobalModuleValue),
      TOptObject);
  call->SetOperand(0, globals_reg);
  call->SetOperand(1, name_reg);
  call->SetOperand(2, keys_version_reg);
  call->SetOperand(3, index_reg);

  tc.emitSnapshot();
  auto guard = tc.emit<Guard>(result);
  guard->setFrameState(tc.frame);
  guard->setGuiltyReg(result);
  guard->setDescr(
      fmt::format("LOAD_GLOBAL_MODULE: {}", PyUnicode_AsUTF8(name)));

  tc.emit<RefineType>(result, TObject, result);
  return true;
}
#endif

void HIRBuilder::emitLoadGlobal(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  int name_idx = loadGlobalIndex(bc_instr.oparg());
  Register* result = temps_.AllocateStack();

  if constexpr (PY_VERSION_HEX < 0x030E0000) {
    if (bc_instr.oparg() & 1) {
      emitPushNull(tc);
    }
  }

  auto try_fast_path = [&] {
#if PY_VERSION_HEX < 0x030C0000
    return tryEmitLoadGlobalModuleValue311(tc, bc_instr, name_idx, result);
#else
    if (!getConfig().stable_frame) {
      return false;
    }
    BorrowedRef<> value = preloader_.global(name_idx);
    if (value == nullptr) {
      return false;
    }
    tc.emit<LoadGlobalCached>(
        result, code_, preloader_.builtins(), preloader_.globals(), name_idx);
    DeoptBase* guard;
    if (PyLong_CheckExact(value.get())) {
      guard = tc.emit<GuardType>(result, TLongExact, result);
    } else {
      guard = tc.emit<GuardIs>(result, value, result);
    }
    BorrowedRef<> name = PyTuple_GET_ITEM(code_->co_names, name_idx);
    guard->setDescr(fmt::format("LOAD_GLOBAL: {}", PyUnicode_AsUTF8(name)));
    return true;
#endif
  };

  if (!try_fast_path()) {
    tc.emit<LoadGlobal>(result, name_idx, tc.frame);
  }

  tc.frame.stack.push(result);

  if constexpr (PY_VERSION_HEX >= 0x030E0000) {
    if (bc_instr.oparg() & 1) {
      emitPushNull(tc);
    }
  }
}

void HIRBuilder::emitMakeFunction(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  int oparg = bc_instr.oparg();
  Register* func = temps_.AllocateStack();

  // The function's qualname is computed from the code object, so we use a
  // sentinel Nullptr value here.
  Register* qualname = temps_.AllocateNonStack();
  tc.emit<LoadConst>(qualname, TNullptr);

  Register* codeobj = tc.frame.stack.pop();

  // make a function
  tc.emit<MakeFunction>(func, codeobj, qualname, tc.frame);

  if (oparg & MAKE_FUNCTION_CLOSURE) {
    Register* closure = tc.frame.stack.pop();
    tc.emit<SetFunctionAttr>(closure, func, FunctionAttr::kClosure);
  }
  if (oparg & MAKE_FUNCTION_ANNOTATIONS) {
    Register* annotations = tc.frame.stack.pop();
    tc.emit<SetFunctionAttr>(annotations, func, FunctionAttr::kAnnotations);
  }
  if (oparg & MAKE_FUNCTION_KWDEFAULTS) {
    Register* kwdefaults = tc.frame.stack.pop();
    tc.emit<SetFunctionAttr>(kwdefaults, func, FunctionAttr::kKwDefaults);
  }
  if (oparg & MAKE_FUNCTION_DEFAULTS) {
    Register* defaults = tc.frame.stack.pop();
    tc.emit<SetFunctionAttr>(defaults, func, FunctionAttr::kDefaults);
  }

  tc.frame.stack.push(func);
}

void HIRBuilder::emitMakeListTuple(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto num_elems = static_cast<size_t>(bc_instr.oparg());
  auto dst = temps_.AllocateStack();
  if (bc_instr.opcode() == BUILD_TUPLE) {
    tc.emit<MakeTuple>(dst, num_elems, tc.frame);
  } else {
    tc.emit<MakeList>(dst, num_elems, tc.frame);
  }
  if (num_elems > 0) {
    Instr* fill;
    if (bc_instr.opcode() == BUILD_TUPLE) {
      fill = tc.emit<InitTupleElements>(num_elems + 1);
    } else {
      fill = tc.emit<InitListElements>(num_elems + 1);
    }
    fill->SetOperand(0, dst);
    for (size_t i = num_elems; i > 0; i--) {
      auto opnd = tc.frame.stack.pop();
      fill->SetOperand(i, opnd);
    }
  }
  tc.frame.stack.push(dst);
}

void HIRBuilder::emitListExtend(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* iterable = tc.frame.stack.pop();
  Register* list = tc.frame.stack.peek(bc_instr.oparg());
  Register* none = temps_.AllocateStack();
  tc.emit<ListExtend>(none, list, iterable, tc.frame);
}

void HIRBuilder::emitListToTuple(TranslationContext& tc) {
  Register* list = tc.frame.stack.pop();
  Register* tuple = temps_.AllocateStack();
  tc.emit<MakeTupleFromList>(tuple, list, tc.frame);
  tc.frame.stack.push(tuple);
}

void HIRBuilder::emitBuildCheckedList(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  BorrowedRef<> arg = constArg(bc_instr);
  BorrowedRef<> descr = PyTuple_GET_ITEM(arg.get(), 0);
  Py_ssize_t list_size = PyLong_AsLong(PyTuple_GET_ITEM(arg.get(), 1));

  const OwnedType* type = preloader_.preloadedType(descr);
  if (type == nullptr) {
    BUILDER_THROW(
        "BUILD_CHECKED_LIST: Can't find type for type descr {}", repr(descr));
  }
  if (!Ci_CheckedList_TypeCheck(type->type)) {
    BUILDER_THROW("Expected CheckedList type, got {}", type->toHir());
  }

  Register* list = temps_.AllocateStack();
  tc.emit<MakeCheckedList>(list, list_size, type->toHir(), tc.frame);
  if (list_size > 0) {
    auto fill = tc.emit<InitListElements>(list_size + 1);
    fill->SetOperand(0, list);
    for (size_t i = list_size; i > 0; i--) {
      auto operand = tc.frame.stack.pop();
      fill->SetOperand(i, operand);
    }
  }
  tc.frame.stack.push(list);
}

void HIRBuilder::emitBuildCheckedMap(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  BorrowedRef<> arg = constArg(bc_instr);
  BorrowedRef<> descr = PyTuple_GET_ITEM(arg.get(), 0);
  Py_ssize_t dict_size = PyLong_AsLong(PyTuple_GET_ITEM(arg.get(), 1));

  const OwnedType* type = preloader_.preloadedType(descr);
  if (type == nullptr) {
    BUILDER_THROW(
        "BUILD_CHECKED_MAP: Can't find type for type descr {}", repr(descr));
  }
  if (!Ci_CheckedDict_TypeCheck(type->type)) {
    BUILDER_THROW("Expected CheckedDict type, got {}", type->toHir());
  }

  Register* dict = temps_.AllocateStack();
  tc.emit<MakeCheckedDict>(dict, dict_size, type->toHir(), tc.frame);
  // Fill dict
  auto& stack = tc.frame.stack;
  for (auto i = stack.size() - dict_size * 2, end = stack.size(); i < end;
       i += 2) {
    auto key = stack.at(i);
    auto value = stack.at(i + 1);
    auto result = temps_.AllocateStack();
    tc.emit<SetDictItem>(result, dict, key, value, tc.frame);
  }
  stack.discard(dict_size * 2);
  stack.push(dict);
}

void HIRBuilder::emitBuildMap(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto dict_size = bc_instr.oparg();
  Register* dict = temps_.AllocateStack();
  tc.emit<MakeDict>(dict, dict_size, tc.frame);
  // Fill dict
  auto& stack = tc.frame.stack;
  for (auto i = stack.size() - dict_size * 2, end = stack.size(); i < end;
       i += 2) {
    auto key = stack.at(i);
    auto value = stack.at(i + 1);
    auto result = temps_.AllocateStack();
    tc.emit<SetDictItem>(result, dict, key, value, tc.frame);
  }
  stack.discard(dict_size * 2);
  stack.push(dict);
}

void HIRBuilder::emitBuildSet(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* set = temps_.AllocateStack();
  tc.emit<MakeSet>(set, tc.frame);

  int oparg = bc_instr.oparg();
  for (int i = oparg; i > 0; i--) {
    auto item = tc.frame.stack.peek(i);

    auto result = temps_.AllocateStack();
    tc.emit<SetSetItem>(result, set, item, tc.frame);
  }

  tc.frame.stack.discard(oparg);

  tc.frame.stack.push(set);
}

void HIRBuilder::emitBuildConstKeyMap(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto dict_size = bc_instr.oparg();
  Register* dict = temps_.AllocateStack();
  tc.emit<MakeDict>(dict, dict_size, tc.frame);
  // Fill dict
  auto& stack = tc.frame.stack;
  Register* keys = stack.pop();
  // ceval.c checks the type and size of the keys tuple before proceeding; we
  // intentionally skip that here.
  for (auto i = 0; i < dict_size; ++i) {
    Register* key = temps_.AllocateStack();
    tc.emit<LoadTupleItem>(key, keys, i);
    Register* value = stack.at(stack.size() - dict_size + i);
    Register* result = temps_.AllocateStack();
    tc.emit<SetDictItem>(result, dict, key, value, tc.frame);
  }
  stack.discard(dict_size);
  stack.push(dict);
}

void HIRBuilder::emitPopJumpIf(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* var = tc.frame.stack.pop();
  BCOffset true_offset, false_offset;
  auto opcode = bc_instr.opcode();
  switch (opcode) {
    case POP_JUMP_IF_ZERO:
    case POP_JUMP_IF_FALSE:
#if PY_VERSION_HEX < 0x030C0000
    case POP_JUMP_BACKWARD_IF_FALSE:
    case POP_JUMP_FORWARD_IF_FALSE:
#endif
    {
      true_offset = bc_instr.nextInstrOffset();
      false_offset = bc_instr.getJumpTarget();
      break;
    }
    case POP_JUMP_IF_NONZERO:
    case POP_JUMP_IF_TRUE:
#if PY_VERSION_HEX < 0x030C0000
    case POP_JUMP_BACKWARD_IF_TRUE:
    case POP_JUMP_FORWARD_IF_TRUE:
#endif
    {
      true_offset = bc_instr.getJumpTarget();
      false_offset = bc_instr.nextInstrOffset();
      break;
    }
    default: {
      BUILDER_THROW(
          "Trying to translate non pop-jump bytecode {} ({})",
          opcodeName(opcode),
          opcode);
    }
  }

  BasicBlock* true_block = getBlockAtOff(true_offset);
  BasicBlock* false_block = getBlockAtOff(false_offset);

  if (bc_instr.opcode() == POP_JUMP_IF_FALSE ||
      bc_instr.opcode() == POP_JUMP_IF_TRUE
#if PY_VERSION_HEX < 0x030C0000
      || bc_instr.opcode() == POP_JUMP_BACKWARD_IF_FALSE ||
      bc_instr.opcode() == POP_JUMP_BACKWARD_IF_TRUE ||
      bc_instr.opcode() == POP_JUMP_FORWARD_IF_FALSE ||
      bc_instr.opcode() == POP_JUMP_FORWARD_IF_TRUE
#endif
  ) {
    Register* is_true = temps_.AllocateNonStack();
    // In 3.14+ coercion to exactly Py_True or Py_False is performed by earlier
    // instructions. See GH-106008.
    if constexpr (PY_VERSION_HEX >= 0x030E0000) {
      Register* const_true = temps_.AllocateNonStack();
      tc.emit<LoadConst>(const_true, Type::fromObject(Py_True));
      tc.emit<PrimitiveCompare>(
          is_true, PrimitiveCompareOp::kEqual, var, const_true);
    } else {
      tc.emit<IsTruthy>(is_true, var, tc.frame);
    }
    tc.emit<CondBranch>(is_true, true_block, false_block);
  } else {
    tc.emit<CondBranch>(var, true_block, false_block);
  }
}

void HIRBuilder::emitPopJumpIfNone(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* var = tc.frame.stack.pop();
  BCOffset true_offset = bc_instr.getJumpTarget();
  BCOffset false_offset = bc_instr.nextInstrOffset();

  BasicBlock* true_block = getBlockAtOff(true_offset);
  BasicBlock* false_block = getBlockAtOff(false_offset);

  auto none = temps_.AllocateNonStack();
  tc.emit<LoadConst>(none, Type::fromObject(Py_None));
  auto is_true = temps_.AllocateNonStack();
  auto op = bc_instr.opcode() == POP_JUMP_IF_NONE
#if PY_VERSION_HEX < 0x030C0000
          || bc_instr.opcode() == POP_JUMP_BACKWARD_IF_NONE ||
          bc_instr.opcode() == POP_JUMP_FORWARD_IF_NONE
#endif
      ? PrimitiveCompareOp::kEqual
      : PrimitiveCompareOp::kNotEqual;
  tc.emit<PrimitiveCompare>(is_true, op, var, none);
  tc.emit<CondBranch>(is_true, true_block, false_block);
}

void HIRBuilder::emitStoreAttr(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* receiver = tc.frame.stack.pop();
  Register* value = tc.frame.stack.pop();
#ifndef Py_GIL_DISABLED
  if (getConfig().specialized_opcodes &&
      bc_instr.specializedOpcode() == STORE_ATTR_SLOT) {
    // STORE_ATTR oparg is always the co_names index; unlike LOAD_ATTR it does
    // not reserve a low bit for method-call stack shaping.
    PyMemberDef* member_def =
        attrSlotMemberDef(code_, bc_instr, bc_instr.oparg());
    if (member_def == nullptr) {
      tc.emit<StoreAttr>(receiver, value, bc_instr.oparg(), tc.frame);
      return;
    }
    BorrowedRef<PyUnicodeObject> name =
        PyTuple_GET_ITEM(code_->co_names, bc_instr.oparg());
    const char* field_name = PyUnicode_AsUTF8(name);
    if (field_name == nullptr) {
      PyErr_Clear();
      field_name = "<unknown>";
    }
    emitSlotTypeVersionGuard(
        tc, receiver, bc_instr.attrCacheTypeVersion(), "STORE_ATTR_SLOT");
    Register* previous = temps_.AllocateStack();
    tc.emit<LoadField>(
        previous,
        receiver,
        field_name,
        member_def->offset,
        TOptObject,
        /* borrowed= */ false);
    tc.emit<StoreField>(
        receiver,
        field_name,
        member_def->offset,
        value,
        TOptObject,
        previous);
    return;
  }
#endif
  tc.emit<StoreAttr>(receiver, value, bc_instr.oparg(), tc.frame);
}

void HIRBuilder::moveOverwrittenStackRegisters(
    TranslationContext& tc,
    Register* dst) {
  // If we're about to overwrite a register that is on the stack, move it to a
  // new register.
  Register* tmp = nullptr;
  auto& stack = tc.frame.stack;
  for (std::size_t i = 0, stack_size = stack.size(); i < stack_size; i++) {
    if (stack.at(i) == dst) {
      if (tmp == nullptr) {
        tmp = temps_.AllocateStack();
        tc.emit<Assign>(tmp, dst);
      }
      stack.atPut(i, tmp);
    }
  }
}
void HIRBuilder::emitStoreFast(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* src = tc.frame.stack.pop();
  Register* dst = tc.frame.localsplus[bc_instr.oparg()];
  JIT_DCHECK(dst != nullptr, "no register");
  moveOverwrittenStackRegisters(tc, dst);
  tc.emit<Assign>(dst, src);
}

void HIRBuilder::emitStoreFastStoreFast(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  int var_idx1 = bc_instr.oparg() >> 4;
  int var_idx2 = bc_instr.oparg() & 0xf;
  size_t localsplus_size = tc.frame.localsplus.size();
  JIT_CHECK(
      var_idx1 < localsplus_size && var_idx2 < localsplus_size,
      "STORE_FAST_STORE_FAST ({}, {}) out of bounds for localsplus array size "
      "{}",
      var_idx1,
      var_idx2,
      tc.frame.localsplus.size());
  Register* src = tc.frame.stack.pop();
  Register* dst = tc.frame.localsplus[var_idx1];
  moveOverwrittenStackRegisters(tc, dst);
  tc.emit<Assign>(dst, src);

  src = tc.frame.stack.pop();
  dst = tc.frame.localsplus[var_idx2];
  moveOverwrittenStackRegisters(tc, dst);
  tc.emit<Assign>(dst, src);
}

void HIRBuilder::emitStoreFastLoadFast(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  int var_idx1 = bc_instr.oparg() >> 4;
  int var_idx2 = bc_instr.oparg() & 0xf;
  size_t localsplus_size = tc.frame.localsplus.size();
  JIT_CHECK(
      var_idx1 < localsplus_size && var_idx2 < localsplus_size,
      "STORE_FAST_LOAD_FAST ({}, {}) out of bounds for localsplus array size "
      "{}",
      var_idx1,
      var_idx2,
      tc.frame.localsplus.size());
  Register* src = tc.frame.stack.pop();
  Register* dst = tc.frame.localsplus[var_idx1];
  moveOverwrittenStackRegisters(tc, dst);
  tc.emit<Assign>(dst, src);

  Register* var = tc.frame.localsplus[var_idx2];
  tc.frame.stack.push(var);
}

void HIRBuilder::emitBinarySlice(TranslationContext& tc) {
  auto& stack = tc.frame.stack;
  tc.emitVariadic<BuildSlice>(temps_, 2);
  Register* slice = stack.pop();
  Register* container = stack.pop();
  Register* result = temps_.AllocateStack();
  tc.emit<BinaryOp>(
      result, BinaryOpKind::kSubscript, container, slice, tc.frame);
  tc.frame.stack.push(result);
}

void HIRBuilder::emitStoreSlice(TranslationContext& tc) {
  auto& stack = tc.frame.stack;
  tc.emitVariadic<BuildSlice>(temps_, 2);
  Register* slice = stack.pop();
  Register* container = stack.pop();
  Register* values = stack.pop();
  tc.emit<StoreSubscr>(container, slice, values, tc.frame);
}

// Emit the index type guard: check sub is LongExact, unbox to CInt64,
// and check for negative index. Returns the unboxed index register.
// On type mismatch or negative index, branches to slow_path (deopt).
Register* HIRBuilder::emitArrayIndexGuard(
    CFG& cfg,
    TranslationContext& tc,
    Register* sub,
    BasicBlock* slow_path) {
  BasicBlock* idx_ok = cfg.AllocateBlock();
  tc.emit<CondBranchCheckType>(sub, TLongExact, idx_ok, slow_path);
  tc.block = idx_ok;
  tc.emit<RefineType>(sub, TLongExact, sub);
  Register* unboxed_idx = temps_.AllocateStack();
  tc.emit<PrimitiveUnbox>(unboxed_idx, sub, TCInt64);
  Register* neg_check = temps_.AllocateStack();
  tc.emit<IsNegativeAndErrOccurred>(neg_check, unboxed_idx, tc.frame);
  return unboxed_idx;
}

// Emit the typecode == 'd' check: load ob_descr->typecode, compare with
// 'd', and branch. Returns the tc_ok block for the caller to continue.
BasicBlock* HIRBuilder::emitArrayTypecodeCheck(
    CFG& cfg,
    TranslationContext& tc,
    Register* container,
    BasicBlock* slow_path) {
  auto descr = temps_.AllocateStack();
  tc.emit<LoadField>(
      descr,
      container,
      "ob_descr",
      offsetof(StdlibArrayObject, ob_descr),
      TCPtr);
  auto typecode = temps_.AllocateStack();
  tc.emit<LoadField>(
      typecode, descr, "typecode", offsetof(StdlibArrayDescr, typecode), TCInt8);
  auto expected_tc = temps_.AllocateStack();
  tc.emit<LoadConst>(expected_tc, Type::fromCInt('d', TCInt8));
  auto tc_match = temps_.AllocateStack();
  tc.emit<PrimitiveCompare>(
      tc_match, PrimitiveCompareOp::kEqual, typecode, expected_tc);
  BasicBlock* tc_ok = cfg.AllocateBlock();
  tc.emit<CondBranch>(tc_match, tc_ok, slow_path);
  return tc_ok;
}

bool HIRBuilder::tryEmitListPrefixReverseAssign(
    TranslationContext& tc,
    jit::BytecodeInstructionBlock::Iterator& bc_it,
    const jit::BytecodeInstructionBlock::Iterator& bc_end) {
#if PY_VERSION_HEX >= 0x030E0000 && PY_VERSION_HEX < 0x030F0000
  if (!getConfig().hir_opts.list_prefix_reverse_assign) {
    return false;
  }

  auto it = bc_it;
  auto next = [&]() -> std::optional<BytecodeInstruction> {
    if (it == bc_end) {
      return std::nullopt;
    }
    BytecodeInstruction instr = *it;
    ++it;
    return instr;
  };
  auto const_obj = [&](const BytecodeInstruction& instr) -> PyObject* {
    if (instr.opcode() != LOAD_CONST) {
      return nullptr;
    }
    if (instr.oparg() < 0 ||
        instr.oparg() >= PyTuple_GET_SIZE(code_->co_consts)) {
      return nullptr;
    }
    return PyTuple_GET_ITEM(code_->co_consts, instr.oparg());
  };
  auto is_const = [&](const BytecodeInstruction& instr, PyObject* obj) {
    return const_obj(instr) == obj;
  };
  auto is_const_long = [&](const BytecodeInstruction& instr, long value) {
    PyObject* obj = const_obj(instr);
    if (obj == nullptr || !PyLong_CheckExact(obj)) {
      return false;
    }
    int overflow = 0;
    long actual = PyLong_AsLongAndOverflow(obj, &overflow);
    return overflow == 0 && actual == value;
  };

  auto first = next();
  if (!first.has_value() ||
      (first->opcode() != LOAD_FAST_BORROW_LOAD_FAST_BORROW &&
       first->opcode() != LOAD_FAST_LOAD_FAST)) {
    return false;
  }
  int container_idx = first->oparg() >> 4;
  int index_idx = first->oparg() & 0xf;
  size_t localsplus_size = tc.frame.localsplus.size();
  if (container_idx >= localsplus_size || index_idx >= localsplus_size) {
    return false;
  }

  auto rhs_stop = next();
  auto rhs_step = next();
  auto rhs_build_slice = next();
  auto rhs_subscr = next();
  auto lhs_container = next();
  auto lhs_start = next();
  auto lhs_index = next();
  auto lhs_one = next();
  auto lhs_add = next();
  auto store_slice_it = it;
  auto store_slice = next();
  if (!rhs_stop || !rhs_step || !rhs_build_slice || !rhs_subscr ||
      !lhs_container || !lhs_start || !lhs_index || !lhs_one || !lhs_add ||
      !store_slice) {
    return false;
  }
  if (!is_const(*rhs_stop, Py_None) || !is_const_long(*rhs_step, -1) ||
      rhs_build_slice->opcode() != BUILD_SLICE || rhs_build_slice->oparg() != 3 ||
      rhs_subscr->opcode() != BINARY_OP || rhs_subscr->oparg() != NB_SUBSCR ||
      lhs_container->opcode() != LOAD_FAST_BORROW ||
      lhs_container->oparg() != container_idx ||
      !is_const(*lhs_start, Py_None) ||
      lhs_index->opcode() != LOAD_FAST_BORROW ||
      lhs_index->oparg() != index_idx ||
      lhs_one->opcode() != LOAD_SMALL_INT || lhs_one->oparg() != 1 ||
      lhs_add->opcode() != BINARY_OP || lhs_add->oparg() != NB_ADD ||
      store_slice->opcode() != STORE_SLICE) {
    return false;
  }

  Register* container = tc.frame.localsplus[container_idx];
  Register* index = tc.frame.localsplus[index_idx];
  if (container == nullptr || index == nullptr) {
    return false;
  }

  tc.frame.cur_instr_offs = store_slice->baseOffset();
  auto output = temps_.AllocateStack();
  tc.emit<CallStatic>(
      2,
      output,
      reinterpret_cast<void*>(JITRT_ListPrefixReverseAssign),
      TCInt32,
      container,
      index);
  // The helper owns the entire folded operation.  On failure it has already
  // performed any Python-visible work that the original bytecode prefix would
  // have performed, then returns -1 with a Python exception set.  CheckNeg uses
  // the frame state only for the exception edge; it must not resume the
  // interpreter at this bytecode and replay the folded slice operations.
  // RuntimeTests cover the may-raise fallback paths and assert that already
  // executed Python side effects are not repeated.
  tc.emit<CheckNeg>(output, output, tc.frame);
  bc_it = store_slice_it;
  return true;
#else
  return false;
#endif
}

void HIRBuilder::emitStoreSubscr(
    CFG& cfg,
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto& stack = tc.frame.stack;
  Register* sub = stack.pop();
  Register* container = stack.pop();
  Register* value = stack.pop();

  int specialized_opcode = -1;
  if (getConfig().specialized_opcodes) {
    specialized_opcode = bc_instr.specializedOpcode();
  }

  // Fast path for array.array('d') store
  if (getConfig().specialized_opcodes &&
      specialized_opcode != STORE_SUBSCR_DICT &&
      specialized_opcode != STORE_SUBSCR_LIST_INT) {
    auto* array_type = getStdlibArrayType();
    if (array_type != nullptr) {
      Type array_type_guard = Type::fromTypeExact(array_type);
      if (!hasArraySubscrStoreFastPathEvidence(
              container, sub, value, array_type_guard)) {
        tc.emit<StoreSubscr>(container, sub, value, tc.frame);
        return;
      }

      BasicBlock* slow_path = cfg.AllocateBlock();
      BasicBlock* done_path = cfg.AllocateBlock();

      // Guard: container is array.array
      BasicBlock* fast_path = cfg.AllocateBlock();
      tc.frame.cur_instr_offs = bc_instr.baseOffset();
      tc.emitSnapshot();
      tc.emit<CondBranchCheckType>(
          container, array_type_guard, fast_path, slow_path);

      tc.block = fast_path;
      tc.emitSnapshot();
      tc.emit<RefineType>(container, array_type_guard, container);

      if (tryStoreSubscrArray(
              cfg, tc, bc_instr, container, sub, value, slow_path, done_path)) {
        tc.block = done_path;
        return;
      }

      // tryStoreSubscrArray returned false — fall through to generic path
    }
  }

  if (getConfig().specialized_opcodes) {
    if (specialized_opcode == STORE_SUBSCR_DICT) {
      tc.emit<GuardType>(container, TDictExact, container, tc.frame);
    } else if (specialized_opcode == STORE_SUBSCR_LIST_INT) {
      tc.emit<GuardType>(container, TListExact, container, tc.frame);
      tc.emit<GuardType>(sub, TLongExact, sub, tc.frame);
    }
  }

  tc.emit<StoreSubscr>(container, sub, value, tc.frame);
}

// Store fast path for array.array('d'). Container is already known to be
// array.array via the outer guard. Emits the value type check, typecode
// check, bounds check, and direct store. Returns true if the full fast
// path was emitted; false if setup failed (caller should fall through to
// the generic path).
bool HIRBuilder::tryStoreSubscrArray(
    CFG& cfg,
    TranslationContext& tc,
    const jit::BytecodeInstruction& /*bc_instr*/,
    Register* container,
    Register* sub,
    Register* value,
    BasicBlock* slow_path,
    BasicBlock* done_path) {
  // Route non-int indices (e.g. slices) to the generic store path.
  Register* unboxed_idx = emitArrayIndexGuard(cfg, tc, sub, slow_path);

  // Route non-float values to the generic store path (also handles
  // int-to-double coercion that stock array assignment performs).
  BasicBlock* val_ok = cfg.AllocateBlock();
  tc.emit<CondBranchCheckType>(value, TFloatExact, val_ok, slow_path);
  tc.block = val_ok;
  tc.emit<RefineType>(value, TFloatExact, value);
  Register* unboxed_value = temps_.AllocateStack();
  tc.emit<PrimitiveUnbox>(unboxed_value, value, TCDouble);

  // Check typecode == 'd'
  BasicBlock* tc_ok = emitArrayTypecodeCheck(cfg, tc, container, slow_path);

  // typecode matched — bounds check + store
  tc.block = tc_ok;
  auto adjusted_idx = temps_.AllocateStack();
  tc.emit<CheckSequenceBounds>(adjusted_idx, container, unboxed_idx, tc.frame);
  auto ob_item = temps_.AllocateStack();
  tc.emit<LoadField>(
      ob_item,
      container,
      "ob_item",
      offsetof(StdlibArrayObject, ob_item),
      TCPtr);
  tc.emit<StoreArrayItem>(
      ob_item, adjusted_idx, unboxed_value, container, TCDouble);
  tc.emit<Branch>(done_path);

  // --- Slow path ---
  tc.block = slow_path;
  tc.emit<StoreSubscr>(container, sub, value, tc.frame);
  tc.emit<Branch>(done_path);

  return true;
}

void HIRBuilder::emitGetIter(TranslationContext& tc) {
  Register* iterable = tc.frame.stack.pop();
  Register* result = temps_.AllocateStack();
  tc.emit<GetIter>(result, iterable, tc.frame);
  tc.frame.stack.push(result);
  if constexpr (PY_VERSION_HEX >= 0x030F0000) {
    // TASK(T243355471): We should support virtual indexing
    emitPushNull(tc);
  }
}

void HIRBuilder::emitForIter(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* iterator;
  if constexpr (PY_VERSION_HEX >= 0x030F0000) {
    iterator = tc.frame.stack.top(1);
  } else {
    iterator = tc.frame.stack.top();
  }
  Register* next_val = temps_.AllocateStack();
  tc.emit<InvokeIterNext>(next_val, iterator, tc.frame);
  tc.frame.stack.push(next_val);
  BasicBlock* footer = getBlockAtOff(bc_instr.getJumpTarget());
  BasicBlock* body = getBlockAtOff(bc_instr.nextInstrOffset());
  tc.emit<CondBranchIterNotDone>(next_val, body, footer);
}

void HIRBuilder::emitGetYieldFromIter(CFG& cfg, TranslationContext& tc) {
  Register* iter_in = tc.frame.stack.pop();

  bool in_coro = code_->co_flags & (CO_COROUTINE | CO_ITERABLE_COROUTINE);
  BasicBlock* done_block = cfg.AllocateBlock();
  BasicBlock* next_block = cfg.AllocateBlock();
  BasicBlock* nop_block = cfg.AllocateBlock();
  BasicBlock* is_coro_block = in_coro ? nop_block : cfg.AllocateBlock();

  BasicBlock* check_coro_block = cfg.AllocateBlock();
  tc.emit<CondBranchCheckType>(
      iter_in,
      Type::fromTypeExact(cinderx::getModuleState()->coro_type),
      is_coro_block,
      check_coro_block);

  tc.block = check_coro_block;
  tc.emit<CondBranchCheckType>(
      iter_in, Type::fromTypeExact(&PyCoro_Type), is_coro_block, next_block);

  if (!in_coro) {
    tc.block = is_coro_block;
    tc.emit<RaiseStatic>(
        0,
        PyExc_TypeError,
        "cannot 'yield from' a coroutine object in a non-coroutine generator",
        tc.frame);
  }

  tc.block = next_block;

  BasicBlock* slow_path = cfg.AllocateBlock();
  Register* iter_out = temps_.AllocateStack();
  tc.emit<CondBranchCheckType>(iter_in, TGen, nop_block, slow_path);

  tc.block = slow_path;
  tc.emit<GetIter>(iter_out, iter_in, tc.frame);
  tc.emit<Branch>(done_block);

  tc.block = nop_block;
  tc.emit<Assign>(iter_out, iter_in);
  tc.emit<Branch>(done_block);

  tc.block = done_block;
  tc.frame.stack.push(iter_out);
}

void HIRBuilder::emitUnpackEx(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  int oparg = bc_instr.oparg();
  int arg_before = oparg & 0xff;
  int arg_after = oparg >> 8;

  auto& stack = tc.frame.stack;
  Register* seq = stack.pop();

  Register* tuple = temps_.AllocateStack();
  tc.emit<UnpackExToTuple>(tuple, seq, arg_before, arg_after, tc.frame);

  int total_args = arg_before + arg_after + 1;
  for (int i = total_args - 1; i >= 0; i--) {
    Register* item = temps_.AllocateStack();
    tc.emit<LoadTupleItem>(item, tuple, i);
    stack.push(item);
  }
}

void HIRBuilder::emitUnpackSequence(
    CFG& cfg,
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  enum class PreferredSequenceType {
    kUnknown,
    kTuple,
    kList,
  };

  auto& stack = tc.frame.stack;
  Register* seq = stack.top();
  PreferredSequenceType preferred = PreferredSequenceType::kUnknown;

  if (getConfig().specialized_opcodes) {
    switch (bc_instr.specializedOpcode()) {
      case UNPACK_SEQUENCE_LIST:
        preferred = PreferredSequenceType::kList;
        break;
      case UNPACK_SEQUENCE_TUPLE:
      case UNPACK_SEQUENCE_TWO_TUPLE:
        preferred = PreferredSequenceType::kTuple;
        break;
      default:
        break;
    }
  }

  int count = bc_instr.oparg();

  // Determine whether the slow path (iterator protocol) is needed.
  // When the type is statically known to be tuple or list (and list is
  // not disabled for free-threading), we can skip the slow path entirely.
  bool needs_slow_path =
      preferred != PreferredSequenceType::kTuple && !seq->isA(TTupleExact);
  if constexpr (!kFreeThreadedBuild) {
    needs_slow_path = needs_slow_path &&
        preferred != PreferredSequenceType::kList && !seq->isA(TListExact);
  }

  TranslationContext deopt_path{cfg.AllocateBlock(), tc.frame};
  deopt_path.frame.cur_instr_offs = bc_instr.baseOffset();
  deopt_path.emitSnapshot();
  Deopt* deopt = deopt_path.emit<Deopt>();
  deopt->setGuiltyReg(seq);
  deopt->setDescr("UNPACK_SEQUENCE");

  BasicBlock* fast_path = cfg.AllocateBlock();
  BasicBlock* second_check_path = cfg.AllocateBlock();
  BasicBlock* list_fast_path = cfg.AllocateBlock();
  BasicBlock* tuple_fast_path = cfg.AllocateBlock();
  Register* list_mem = temps_.AllocateStack();
  stack.pop();

  // When the slow path is needed, we pre-allocate output registers shared
  // between the fast and slow paths. Both paths write to the same items[]
  // registers (at runtime exactly one path executes), and then branch to
  // done_path where the items are pushed to the stack.
  BasicBlock* slow_path = needs_slow_path ? cfg.AllocateBlock() : nullptr;
  BasicBlock* done_path = needs_slow_path ? cfg.AllocateBlock() : nullptr;
  BasicBlock* unpack_failure_path =
      needs_slow_path ? slow_path : deopt_path.block;
  std::vector<Register*> items;
  if (needs_slow_path) {
    items.resize(count);
    for (int i = 0; i < count; i++) {
      items[i] = temps_.AllocateStack();
    }
  }

  // TODO: The manual type checks and branches should go away once we get
  // PGO support to be able to optimize to known types.

  if (seq->isA(TTupleExact)) {
    tc.emit<Branch>(tuple_fast_path);
  } else if (seq->isA(TListExact)) {
    // TODO(T255264577). Enable this again. See P2169677587.
    if constexpr (kFreeThreadedBuild) {
      tc.emit<Branch>(unpack_failure_path);
    } else {
      tc.emit<Branch>(list_fast_path);
    }
  } else {
    auto emit_list_then_tuple = [&]() {
      // TODO(T255264577). Enable this again. See P2169677587.
      if constexpr (kFreeThreadedBuild) {
        tc.emit<CondBranchCheckType>(
            seq, TTupleExact, tuple_fast_path, unpack_failure_path);
      } else {
        tc.emit<CondBranchCheckType>(
            seq, TListExact, list_fast_path, second_check_path);
        tc.block = second_check_path;
        tc.emit<CondBranchCheckType>(
            seq, TTupleExact, tuple_fast_path, unpack_failure_path);
      }
    };

    auto emit_tuple_then_list = [&]() {
      tc.emit<CondBranchCheckType>(
          seq, TTupleExact, tuple_fast_path, second_check_path);
      tc.block = second_check_path;
      // TODO(T255264577). Enable this again. See P2169677587.
      if constexpr (kFreeThreadedBuild) {
        tc.emit<Branch>(unpack_failure_path);
      } else {
        tc.emit<CondBranchCheckType>(
            seq, TListExact, list_fast_path, unpack_failure_path);
      }
    };

    if (preferred == PreferredSequenceType::kList) {
      emit_list_then_tuple();
    } else {
      emit_tuple_then_list();
    }
  }

  tc.block = tuple_fast_path;
  Register* offset_reg = temps_.AllocateStack();
  tc.emit<LoadConst>(
      offset_reg, Type::fromCInt(offsetof(PyTupleObject, ob_item), TCInt64));
  tc.emit<LoadFieldAddress>(list_mem, seq, offset_reg);
  tc.emit<Branch>(fast_path);

  tc.block = list_fast_path;
  tc.emit<LoadField>(
      list_mem, seq, "ob_item", offsetof(PyListObject, ob_item), TCPtr);
  tc.emit<Branch>(fast_path);

  tc.block = fast_path;

  Register* seq_size = temps_.AllocateStack();
  Register* target_size = temps_.AllocateStack();
  Register* is_equal = temps_.AllocateStack();
  tc.emit<LoadVarObjectSize>(seq_size, seq);
  tc.emit<LoadConst>(target_size, Type::fromCInt(count, TCInt64));
  tc.emit<PrimitiveCompare>(
      is_equal, PrimitiveCompareOp::kEqual, seq_size, target_size);
  fast_path = cfg.AllocateBlock();
  tc.emit<CondBranch>(is_equal, fast_path, deopt_path.block);
  tc.block = fast_path;

  Register* idx_reg = temps_.AllocateStack();
  if (needs_slow_path) {
    // Write to pre-allocated items[] registers shared with the slow path.
    for (int idx = count - 1; idx >= 0; --idx) {
      tc.emit<LoadConst>(idx_reg, Type::fromCInt(idx, TCInt64));
      tc.emit<LoadArrayItem>(items[idx], list_mem, idx_reg, seq, 0, TObject);
    }
    tc.emit<Branch>(done_path);

    // Slow path: use the iterator protocol for arbitrary iterable types.
    // Allocate stack space for the items array and call the runtime helper
    // to fill it. Then load items from the stack array using LoadArrayItem.
    tc.block = slow_path;
    Register* stack_array = temps_.AllocateStack();
    tc.emit<ReserveStack>(stack_array, count);
    Register* result = temps_.AllocateStack();
    tc.emit<UnpackSequence>(result, seq, stack_array, count);
    tc.emit<CheckNeg>(result, result, tc.frame);
    Register* slow_idx = temps_.AllocateStack();
    for (int i = 0; i < count; i++) {
      tc.emit<LoadConst>(slow_idx, Type::fromCInt(i, TCInt64));
      // Items in the stack array are new references from PyIter_Next,
      // so we use borrowed=false to indicate the loaded values are owned.
      tc.emit<LoadArrayItem>(
          items[i], stack_array, slow_idx, seq, 0, TObject, false);
    }
    tc.emit<Branch>(done_path);

    // Both paths wrote to the same pre-allocated items[] registers.
    // Push them to the stack in reverse order (TOS = first element).
    tc.block = done_path;
    for (int i = count - 1; i >= 0; --i) {
      stack.push(items[i]);
    }
  } else {
    // No slow path: push items directly to the stack (original behavior).
    for (int idx = count - 1; idx >= 0; --idx) {
      Register* item = temps_.AllocateStack();
      tc.emit<LoadConst>(idx_reg, Type::fromCInt(idx, TCInt64));
      tc.emit<LoadArrayItem>(item, list_mem, idx_reg, seq, 0, TObject);
      stack.push(item);
    }
  }
}

void HIRBuilder::emitSetupFinally(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  BCOffset handler_off =
      bc_instr.nextInstrOffset() + BCIndex{bc_instr.oparg()}.asOffset();
  int stack_level = tc.frame.stack.size();
  tc.frame.block_stack.push(
      ExecutionBlock{SETUP_FINALLY, handler_off, stack_level});
}

void HIRBuilder::emitAsyncForHeaderYieldFrom(
    CFG& cfg,
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* out = temps_.AllocateStack();
  emitYieldFrom(cfg, tc, out, /*handle_stop_async_iteration=*/true);

  BasicBlock* yf_cont_block = getBlockAtOff(bc_instr.nextInstrOffset());
  BCOffset handler_off{tc.frame.block_stack.top().handler_off};
  BasicBlock* yf_done_block = getBlockAtOff(handler_off);
  tc.emit<CondBranchIterNotDone>(out, yf_cont_block, yf_done_block);
}

void HIRBuilder::emitEndAsyncFor(TranslationContext& tc) {
  // Pop finally block and discard exhausted async iterator.
  const ExecutionBlock& b = tc.frame.block_stack.top();
  JIT_CHECK(
      static_cast<int>(tc.frame.stack.size()) == b.stack_level,
      "Bad stack depth in END_ASYNC_FOR: block stack expects {}, stack is {}",
      b.stack_level,
      tc.frame.stack.size());
  tc.frame.block_stack.pop();
  tc.frame.stack.pop();
}

void HIRBuilder::emitGetAIter(TranslationContext& tc) {
  Register* obj = tc.frame.stack.pop();
  Register* out = temps_.AllocateStack();
  tc.emit<GetAIter>(out, obj, tc.frame);
  tc.frame.stack.push(out);
}

void HIRBuilder::emitGetANext(TranslationContext& tc) {
  Register* obj = tc.frame.stack.top();
  Register* out = temps_.AllocateStack();
  tc.emit<GetANext>(out, obj, tc.frame);
  tc.frame.stack.push(out);
}

Register* HIRBuilder::emitSetupWithCommon(
    TranslationContext& tc,
    PyObject* enter_id,
    PyObject* exit_id,
    bool is_async) {
  // Load the enter and exit attributes from the manager, push exit, and return
  // the result of calling enter().
  auto& stack = tc.frame.stack;
  Register* manager = stack.pop();
  Register* enter = temps_.AllocateStack();
  Register* exit = temps_.AllocateStack();
  tc.emit<LoadAttrSpecial>(
      enter,
      manager,
      enter_id,
      is_async
          ? "'%.200s' object does not support the asynchronous context manager "
            "protocol"
          : "'%.200s' object does not support the context manager protocol",
      tc.frame);
  tc.emit<LoadAttrSpecial>(
      exit,
      manager,
      exit_id,
      is_async
          ? "'%.200s' object does not support the asynchronous context manager "
            "protocol (missed __aexit__ method)"
          : "'%.200s' object does not support the context manager protocol "
            "(missed __exit__ method)",
      tc.frame);
  stack.push(exit);

  Register* enter_result = temps_.AllocateStack();
  auto call = tc.emit<VectorCall>(1, enter_result, CallFlags::None);
  call->setFrameState(tc.frame);
  call->SetOperand(0, enter);
  return enter_result;
}

void HIRBuilder::emitBeforeWith(
    TranslationContext& tc,
    [[maybe_unused]] const jit::BytecodeInstruction& bc_instr) {
  if (bc_instr.opcode() == BEFORE_ASYNC_WITH) {
    tc.frame.stack.push(
        emitSetupWithCommon(tc, &_Py_ID(__aenter__), &_Py_ID(__aexit__), true));
  } else {
    tc.frame.stack.push(
        emitSetupWithCommon(tc, &_Py_ID(__enter__), &_Py_ID(__exit__), false));
  }
}

void HIRBuilder::emitSetupAsyncWith(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  // The finally block should be above the result of __aenter__.
  Register* top = tc.frame.stack.pop();
  emitSetupFinally(tc, bc_instr);
  tc.frame.stack.push(top);
}

void HIRBuilder::emitSetupWith(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* enter_result =
      emitSetupWithCommon(tc, &_Py_ID(__aenter__), &_Py_ID(__aexit__), true);
  emitSetupFinally(tc, bc_instr);
  tc.frame.stack.push(enter_result);
}

void HIRBuilder::emitLoadField(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  BorrowedRef<> descr = constArg(bc_instr);
  const FieldInfo* field = preloader_.fieldInfo(descr);
  if (field == nullptr) {
    BUILDER_THROW("LOAD_FIELD: Can't find field for descr {}", repr(descr));
  }
  auto& [offset, type, name] = *field;

  Register* receiver = tc.frame.stack.pop();
  Register* result = temps_.AllocateStack();
  const char* field_name = PyUnicode_AsUTF8(name);
  if (field_name == nullptr) {
    PyErr_Clear();
    field_name = "";
  }
  tc.emit<LoadField>(result, receiver, field_name, offset, type);
  if (type.couldBe(TNullptr)) {
    CheckField* cf = tc.emit<CheckField>(result, result, name, tc.frame);
    cf->setGuiltyReg(receiver);
  }
  tc.frame.stack.push(result);
}

void HIRBuilder::emitStoreField(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  BorrowedRef<> descr = constArg(bc_instr);
  const FieldInfo* field = preloader_.fieldInfo(descr);
  if (field == nullptr) {
    BUILDER_THROW("STORE_FIELD: Can't find field for descr {}", repr(descr));
  }
  auto& [offset, type, name] = *field;
  const char* field_name = PyUnicode_AsUTF8(name);
  if (field_name == nullptr) {
    PyErr_Clear();
    field_name = "";
  }

  Register* receiver = tc.frame.stack.pop();
  Register* value = tc.frame.stack.pop();
  Register* previous = temps_.AllocateStack();
  if (type <= TPrimitive) {
    Register* converted = temps_.AllocateStack();
    tc.emit<LoadConst>(previous, TNullptr);
    tc.emit<PrimitiveConvert>(converted, value, type);
    value = converted;
  } else {
    tc.emit<LoadField>(previous, receiver, field_name, offset, type, false);
  }
  tc.emit<StoreField>(receiver, field_name, offset, value, type, previous);
}

void HIRBuilder::emitCast(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  BorrowedRef<> descr = constArg(bc_instr);
  const OwnedType* preloaded_type = preloader_.preloadedType(descr);
  if (preloaded_type == nullptr) {
    BUILDER_THROW("CAST: Can't find type for type descr {}", repr(descr));
  }

  Register* value = tc.frame.stack.pop();
  Register* result = temps_.AllocateStack();
  tc.emit<Cast>(
      result,
      value,
      preloaded_type->type,
      preloaded_type->optional,
      preloaded_type->exact,
      tc.frame);
  tc.frame.stack.push(result);
}

void HIRBuilder::emitTpAlloc(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  BorrowedRef<> descr = constArg(bc_instr);
  const OwnedType* type = preloader_.preloadedType(descr);
  if (type == nullptr) {
    BUILDER_THROW("TP_ALLOC: Cannot find type for descr {}", repr(descr));
  }
  if (type->optional) {
    BUILDER_THROW(
        "Cannot use optional {} type for TP_ALLOC", type->type->tp_name);
  }

  Register* result = temps_.AllocateStack();
  tc.emit<TpAlloc>(result, type->type, tc.frame);
  tc.frame.stack.push(result);
}

void HIRBuilder::emitImportFrom(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto& stack = tc.frame.stack;
  Register* name = stack.top();
  Register* res = temps_.AllocateStack();
  tc.emit<ImportFrom>(res, name, bc_instr.oparg(), tc.frame);
  stack.push(res);
}

// Adjusts the oparg for import name to be the name index.
int importNameIdx(int oparg) {
  if constexpr (PY_VERSION_HEX >= 0x030F0000) {
    return oparg >> 2;
  } else {
    return oparg;
  }
}

void HIRBuilder::emitImportName(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto& stack = tc.frame.stack;
  Register* fromlist = stack.pop();
  Register* level = stack.pop();
  Register* res = temps_.AllocateStack();
  if (bc_instr.opcode() == EAGER_IMPORT_NAME) {
    tc.emit<EagerImportName>(res, bc_instr.oparg(), fromlist, level, tc.frame);
  } else {
    tc.emit<ImportName>(
        res, importNameIdx(bc_instr.oparg()), fromlist, level, tc.frame);
  }
  stack.push(res);
}

void HIRBuilder::emitRaiseVarargs(TranslationContext& tc) {
  tc.emit<Raise>(tc.frame);
}

void HIRBuilder::emitYieldFrom(
    CFG& cfg,
    TranslationContext& tc,
    Register* out,
    bool handle_stop_async_iteration) {
  auto& stack = tc.frame.stack;
  // Stack: [..., iter, send_value]
  auto iter = stack.top(1);

  if (code_->co_flags & CO_COROUTINE) {
    tc.emit<SetCurrentAwaiter>(iter);
  }

  BasicBlock* send_bb = cfg.AllocateBlock();
  BasicBlock* yield_bb = cfg.AllocateBlock();
  BasicBlock* done_bb = cfg.AllocateBlock();

  tc.emit<Branch>(send_bb);

  // --- send_block: merge point for initial entry and yield back-edge ---
  TranslationContext send_tc{send_bb, tc.frame};
  auto send_value = send_tc.frame.stack.pop();
  auto iter_reg = send_tc.frame.stack.top();
  // Due to the mixin order (Operands<2>, HasOutput, DeoptBase), the Send
  // constructor maps: arg1→operand[0], arg2→operand[1], arg3→output.
  // So we pass (iter, send_value, result) to match emitSend's convention.
  // Reuse send_value as the output so SSAify creates a proper Phi at this
  // merge point (send_value is defined on both the initial and back-edge
  // paths).
  send_tc.emit<Send>(
      iter_reg,
      send_value,
      send_value,
      send_tc.frame,
      handle_stop_async_iteration);
  auto is_done = temps_.AllocateNonStack();
  send_tc.emit<GetSecondOutput>(is_done, TCInt64, send_value);
  send_tc.frame.stack.push(send_value);
  send_tc.emit<CondBranch>(is_done, done_bb, yield_bb);

  // --- yield_block: yield the intermediate value, loop back ---
  TranslationContext yield_tc{yield_bb, send_tc.frame};
  yield_tc.frame.stack.pop();
  auto* yv = yield_tc.emit<YieldValue>(send_value, send_value, yield_tc.frame);
  yv->setYieldFromIter(yield_tc.frame.stack.top()); // iter
  yield_tc.frame.stack.push(send_value);
  yield_tc.emit<Branch>(send_bb);

  // --- done_block: pop result and iter, push final result ---
  TranslationContext done_tc{done_bb, send_tc.frame};
  auto final_result = done_tc.frame.stack.pop();
  done_tc.frame.stack.pop(); // pop iter

  if (out != final_result) {
    done_tc.emit<Assign>(out, final_result);
  }
  done_tc.frame.stack.push(out);

  // Continue from done_block
  tc.block = done_bb;
  tc.frame = done_tc.frame;
}

void HIRBuilder::emitYieldValue(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto& stack = tc.frame.stack;
  auto in = stack.pop();
  auto out = temps_.AllocateStack();
  if (code_->co_flags & CO_ASYNC_GENERATOR) {
    tc.emitChecked<CallCFunc>(
        1,
        out,
        CallCFunc::Func::kCix_PyAsyncGenValueWrapperNew,
        std::vector<Register*>{in});
    in = out;
    out = temps_.AllocateStack();
  }
  if constexpr (PY_VERSION_HEX < 0x030E0000) {
    auto next_bc =
        BytecodeInstruction{code_, tc.frame.cur_instr_offs}.nextInstr();

    // This mirrors what _PyGen_yf() does. I assume the RESUME oparg exists
    // primarily for this check - values 2 and 3 indicate a "yield from" and
    // "await" respectively.
    if (next_bc.opcode() == RESUME && next_bc.oparg() >= 2) {
      auto* yv = tc.emit<YieldValue>(out, in, tc.frame);
      yv->setYieldFromIter(stack.top());
    } else {
      tc.emit<YieldValue>(out, in, tc.frame);
    }
  } else {
    if (bc_instr.oparg() == 1) {
      auto* yv = tc.emit<YieldValue>(out, in, tc.frame);
      // In 3.15, PUSH_NULL adds a loop index between the sub-iterator and
      // the yield value. The sub-iterator is one below the top.
      if constexpr (PY_VERSION_HEX >= 0x030F0000) {
        yv->setYieldFromIter(stack.top(1));
      } else {
        yv->setYieldFromIter(stack.top());
      }
    } else {
      JIT_CHECK(bc_instr.oparg() == 0, "Invalid oparg {}", bc_instr.oparg());
      tc.emit<YieldValue>(out, in, tc.frame);
    }
  }
  stack.push(out);
}

void HIRBuilder::emitGetAwaitable(
    CFG& cfg,
    TranslationContext& tc,
    const BytecodeInstructionBlock& bc_instrs,
    BytecodeInstruction bc_instr) {
  OperandStack& stack = tc.frame.stack;
  Register* iterable = stack.pop();
  Register* iter = temps_.AllocateStack();

  // Most work is done by existing JitPyCoro_GetAwaitableIter() utility.
  tc.emit<CallCFunc>(
      1,
      iter,
      CallCFunc::Func::kJitCoro_GetAwaitableIter,
      std::vector<Register*>{iterable});

  bool error_aenter = bc_instr.oparg() == 1;
  bool error_aexit = bc_instr.oparg() == 2;
  if (error_aenter || error_aexit) {
    BasicBlock* error_block = cfg.AllocateBlock();
    BasicBlock* ok_block = cfg.AllocateBlock();
    tc.emit<CondBranch>(iter, ok_block, error_block);
    tc.block = error_block;
    Register* type = temps_.AllocateStack();
    tc.emit<LoadField>(
        type, iterable, "ob_type", offsetof(PyObject, ob_type), TType);
    tc.emit<RaiseAwaitableError>(type, error_aenter, tc.frame);

    tc.block = ok_block;
    // TASK(T105038867): Remove once we have RefineTypeInsertion
    tc.emit<RefineType>(iter, TObject, iter);
  } else {
    tc.emit<CheckExc>(iter, iter, tc.frame);
  }

  // For coroutines only, runtime assert it isn't already awaiting by checking
  // if it has a sub-iterator using *Gen_yf().
  BasicBlock* block_assert_not_awaited_coro = cfg.AllocateBlock();
  BasicBlock* block_done = cfg.AllocateBlock();
  BasicBlock* block_check_coro = cfg.AllocateBlock();
  tc.emit<CondBranchCheckType>(
      iter,
      Type::fromTypeExact(cinderx::getModuleState()->coro_type),
      block_assert_not_awaited_coro,
      block_check_coro);
  tc.block = block_check_coro;
  tc.emit<CondBranchCheckType>(
      iter,
      Type::fromTypeExact(&PyCoro_Type),
      block_assert_not_awaited_coro,
      block_done);
  Register* yf = temps_.AllocateStack();
  tc.block = block_assert_not_awaited_coro;
  tc.emit<CallCFunc>(
      1, yf, CallCFunc::Func::kJitGen_yf, std::vector<Register*>{iter});
  BasicBlock* block_coro_already_awaited = cfg.AllocateBlock();
  tc.emit<CondBranch>(yf, block_coro_already_awaited, block_done);
  tc.block = block_coro_already_awaited;
  tc.emit<RaiseStatic>(
      0, PyExc_RuntimeError, "coroutine is being awaited already", tc.frame);

  stack.push(iter);

  tc.block = block_done;
}

void HIRBuilder::emitBuildString(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto num_operands = bc_instr.oparg();
  tc.emitVariadic<BuildString>(temps_, num_operands);
}

void HIRBuilder::emitFormatValue(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto oparg = bc_instr.oparg();

  int have_fmt_spec = (oparg & FVS_MASK) == FVS_HAVE_SPEC;
  Register* fmt_spec;
  if (have_fmt_spec) {
    fmt_spec = tc.frame.stack.pop();
  } else {
    fmt_spec = temps_.AllocateStack();
    tc.emit<LoadConst>(fmt_spec, TNullptr);
  }
  Register* value = tc.frame.stack.pop();
  Register* dst = temps_.AllocateStack();
  int which_conversion = oparg & FVC_MASK;

  tc.emit<FormatValue>(dst, fmt_spec, value, which_conversion, tc.frame);
  tc.frame.stack.push(dst);
}

void HIRBuilder::emitFormatWithSpec(TranslationContext& tc) {
  OperandStack& stack = tc.frame.stack;
  Register* fmt_spec = stack.pop();
  Register* value = stack.pop();
  Register* out = temps_.AllocateStack();
  tc.emit<FormatWithSpec>(out, value, fmt_spec, tc.frame);
  stack.push(out);
}

void HIRBuilder::emitMapAdd(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto oparg = bc_instr.oparg();
  auto& stack = tc.frame.stack;
  auto value = stack.pop();
  auto key = stack.pop();

  auto map = stack.peek(oparg);

  auto result = temps_.AllocateStack();
  tc.emit<SetDictItem>(result, map, key, value, tc.frame);
}

void HIRBuilder::emitSetAdd(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto oparg = bc_instr.oparg();
  auto& stack = tc.frame.stack;

  auto* v = stack.pop();
  auto* set = stack.peek(oparg);

  auto result = temps_.AllocateStack();
  tc.emit<SetSetItem>(result, set, v, tc.frame);
}

void HIRBuilder::emitSetUpdate(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto oparg = bc_instr.oparg();
  auto& stack = tc.frame.stack;
  auto* iterable = stack.pop();
  auto* set = stack.peek(oparg);
  auto result = temps_.AllocateStack();
  tc.emit<SetUpdate>(result, set, iterable, tc.frame);
}

void HIRBuilder::emitDispatchEagerCoroResult(
    CFG& cfg,
    TranslationContext& tc,
    Register* out,
    BasicBlock* await_block,
    BasicBlock* post_await_block) {
  Register* stack_top = tc.frame.stack.top();

  TranslationContext has_wh_block{cfg.AllocateBlock(), tc.frame};
  tc.emit<CondBranchCheckType>(
      stack_top, TWaitHandle, has_wh_block.block, await_block);

  Register* wait_handle = stack_top;
  Register* wh_coro_or_result = temps_.AllocateStack();
  Register* wh_waiter = temps_.AllocateStack();
  has_wh_block.emit<WaitHandleLoadCoroOrResult>(wh_coro_or_result, wait_handle);
  has_wh_block.emit<WaitHandleLoadWaiter>(wh_waiter, wait_handle);
  has_wh_block.emit<WaitHandleRelease>(wait_handle);

  TranslationContext coro_block{cfg.AllocateBlock(), tc.frame};
  TranslationContext res_block{cfg.AllocateBlock(), tc.frame};
  has_wh_block.emit<CondBranch>(wh_waiter, coro_block.block, res_block.block);

  // wh_waiter is OptObject; refine to Object in the true branch.
  coro_block.emit<RefineType>(wh_waiter, TObject, wh_waiter);

  if (code_->co_flags & CO_COROUTINE) {
    coro_block.emit<SetCurrentAwaiter>(wh_coro_or_result);
  }
  // Yield the waiter value first (like YieldAndYieldFrom's skip-initial-send),
  // then enter the yield-from Send loop with the resumed value.
  Register* initial_send = temps_.AllocateStack();
  auto* yv =
      coro_block.emit<YieldValue>(initial_send, wh_waiter, coro_block.frame);
  yv->setYieldFromIter(wh_coro_or_result);
  // Set up stack for emitYieldFrom: [..., iter, send_value]
  coro_block.frame.stack.push(wh_coro_or_result);
  coro_block.frame.stack.push(initial_send);
  emitYieldFrom(cfg, coro_block, out);
  coro_block.emit<Branch>(post_await_block);

  res_block.emit<Assign>(out, wh_coro_or_result);
  res_block.emit<Branch>(post_await_block);
}

void HIRBuilder::emitMatchMappingSequence(
    CFG& cfg,
    TranslationContext& tc,
    uint64_t tf_flag) {
  Register* top = tc.frame.stack.top();
  auto type = temps_.AllocateStack();
  tc.emit<LoadField>(type, top, "ob_type", offsetof(PyObject, ob_type), TType);
  auto tp_flags = temps_.AllocateStack();
  tc.emit<LoadField>(
      tp_flags, type, "tp_flags", offsetof(PyTypeObject, tp_flags), TCUInt64);
  auto flag = temps_.AllocateStack();
  tc.emit<LoadConst>(flag, Type::fromCUInt(tf_flag, TCUInt64));

  auto and_result = temps_.AllocateStack();
  tc.emit<IntBinaryOp>(and_result, BinaryOpKind::kAnd, tp_flags, flag);

  auto true_block = cfg.AllocateBlock();
  auto false_block = cfg.AllocateBlock();
  tc.emit<CondBranch>(and_result, true_block, false_block);

  auto result = temps_.AllocateStack();
  tc.block = true_block;
  tc.emit<LoadConst>(result, Type::fromObject(Py_True));
  auto done = cfg.AllocateBlock();
  tc.emit<Branch>(done);

  tc.block = false_block;
  tc.emit<LoadConst>(result, Type::fromObject(Py_False));
  tc.emit<Branch>(done);

  tc.block = done;

  tc.frame.stack.push(result);
}

void HIRBuilder::emitMatchClass(
    CFG& cfg,
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  auto& stack = tc.frame.stack;
  Register* names = stack.pop();
  Register* type = stack.pop();
  Register* subject = stack.pop();
  auto oparg = bc_instr.oparg();

  auto nargs = temps_.AllocateStack();
  tc.emit<LoadConst>(nargs, Type::fromCUInt(oparg, TCUInt64));

  auto attrs_tuple = temps_.AllocateStack();
  tc.emit<MatchClass>(attrs_tuple, subject, type, nargs, names);
  tc.emit<RefineType>(attrs_tuple, TOptTupleExact, attrs_tuple);

  Register* tuple_or_none = temps_.AllocateStack();
  stack.push(tuple_or_none);

  auto true_block = cfg.AllocateBlock();
  auto false_block = cfg.AllocateBlock();
  auto done = cfg.AllocateBlock();

  tc.emit<CondBranch>(attrs_tuple, true_block, false_block);
  tc.block = true_block;
  tc.emit<RefineType>(tuple_or_none, TTupleExact, attrs_tuple);
  tc.emit<Branch>(done);

  tc.block = false_block;
  tc.emit<CheckErrOccurred>(tc.frame);
  Register* none = temps_.AllocateNonStack();
  tc.emit<LoadConst>(none, Type::fromObject(Py_None));
  tc.emit<Assign>(tuple_or_none, none);
  tc.emit<Branch>(done);

  tc.block = done;
}

void HIRBuilder::emitMatchKeys(CFG& cfg, TranslationContext& tc) {
  auto& stack = tc.frame.stack;
  Register* keys = stack.top();
  Register* subject = stack.top(1);

  auto values_or_none = temps_.AllocateStack();
  tc.emit<MatchKeys>(values_or_none, subject, keys, tc.frame);
  stack.push(values_or_none);

  auto none = temps_.AllocateStack();
  tc.emit<LoadConst>(none, Type::fromObject(Py_None));
  auto is_none = temps_.AllocateStack();
  tc.emit<PrimitiveCompare>(
      is_none, PrimitiveCompareOp::kEqual, values_or_none, none);

  auto true_block = cfg.AllocateBlock();
  auto false_block = cfg.AllocateBlock();
  auto done = cfg.AllocateBlock();

  tc.emit<CondBranch>(is_none, true_block, false_block);
  tc.block = true_block;
  tc.emit<RefineType>(values_or_none, TNoneType, values_or_none);
  tc.emit<Branch>(done);

  tc.block = false_block;
  tc.emit<RefineType>(values_or_none, TTupleExact, values_or_none);
  tc.emit<Branch>(done);
  tc.block = done;
}

void HIRBuilder::emitDictUpdate(
    TranslationContext& tc,
    const BytecodeInstruction& bc_instr) {
  auto& stack = tc.frame.stack;
  Register* update = stack.pop();
  Register* dict = stack.top(bc_instr.oparg() - 1);
  Register* out = temps_.AllocateStack();
  tc.emit<DictUpdate>(out, dict, update, tc.frame);
}

void HIRBuilder::emitDictMerge(
    TranslationContext& tc,
    const BytecodeInstruction& bc_instr) {
  auto& stack = tc.frame.stack;
  Register *dict, *func;
  if constexpr (PY_VERSION_HEX < 0x030E0000) {
    dict = stack.top(bc_instr.oparg());
    func = stack.top(bc_instr.oparg() + 2);
  } else {
    // According to bytecodes.c, at this point on the stack we have:
    //  update (top of the stack)
    //  [unused if oparg is 0]
    //  dict
    //  unused
    //  unused
    //  callable
    // Looking at codegen.c for 3.14, oparg is only ever 1 so the optional
    // "unused" slot is never present. So the 1 and 4 offsets skip to "dict" and
    // "callable" respectively.
    JIT_CHECK(bc_instr.oparg() == 1, "oparg must be 1");
    dict = stack.top(1);
    func = stack.top(4);
  }
  Register* update = stack.pop();
  Register* out = temps_.AllocateStack();
  tc.emit<DictMerge>(out, dict, update, func, tc.frame);
}

void HIRBuilder::emitSend(
    TranslationContext& tc,
    const BytecodeInstruction& bc_instr) {
  OperandStack& stack = tc.frame.stack;
  Register* value_out = stack.pop();
  Register* iter;
  if constexpr (PY_VERSION_HEX >= 0x030F0000) {
    iter = stack.top(1);
  } else {
    iter = stack.top();
  }
  Register* value_in = temps_.AllocateStack();
  tc.emit<Send>(iter, value_out, value_in, tc.frame);
  Register* is_done = temps_.AllocateNonStack();
  tc.emit<GetSecondOutput>(is_done, TCInt64, value_in);
  stack.push(value_in);
  BasicBlock* done_block = getBlockAtOff(bc_instr.getJumpTarget());
  BasicBlock* continue_block = getBlockAtOff(bc_instr.nextInstrOffset());
  tc.emit<CondBranch>(is_done, done_block, continue_block);
}

void HIRBuilder::emitBuildInterpolation(
    [[maybe_unused]] TranslationContext& tc,
    [[maybe_unused]] const jit::BytecodeInstruction& bc_instr) {
#if PY_VERSION_HEX >= 0x030E0000
  OperandStack& stack = tc.frame.stack;
  auto oparg = bc_instr.oparg();
  int conversion = oparg >> 2;

  Register* format;
  if (oparg & 1) {
    format = stack.pop();
  } else {
    PyObject* empty = &_Py_STR(empty);
    format = temps_.AllocateStack();
    tc.emit<LoadConst>(format, Type::fromObject(empty));
  }

  Register* str = stack.pop();
  Register* value = stack.pop();
  Register* out = temps_.AllocateStack();
  tc.emit<BuildInterpolation>(out, value, str, format, conversion, tc.frame);
  stack.push(out);
#endif
}

void HIRBuilder::emitBuildTemplate(TranslationContext& tc) {
  OperandStack& stack = tc.frame.stack;
  Register* interpolations = stack.pop();
  Register* strings = stack.pop();
  Register* out = temps_.AllocateStack();
  tc.emit<BuildTemplate>(strings, interpolations, out, tc.frame);
  stack.push(out);
}

void HIRBuilder::emitConvertValue(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  OperandStack& stack = tc.frame.stack;
  Register* value = stack.pop();
  Register* out = temps_.AllocateStack();
  tc.emit<ConvertValue>(out, value, bc_instr.oparg(), tc.frame);
  stack.push(out);
}

void HIRBuilder::emitFormatSimple(CFG& cfg, TranslationContext& tc) {
  OperandStack& stack = tc.frame.stack;
  Register* value = stack.pop();

  BasicBlock* done_block = cfg.AllocateBlock();
  BasicBlock* do_fmt_block = cfg.AllocateBlock();
  BasicBlock* pass_through_block = cfg.AllocateBlock();

  tc.emit<CondBranchCheckType>(
      value, TUnicodeExact, pass_through_block, do_fmt_block);
  Register* out = temps_.AllocateStack();

  tc.block = do_fmt_block;
  Register* fmt_spec = temps_.AllocateStack();
  tc.emit<LoadConst>(fmt_spec, TNullptr);
  tc.emit<FormatWithSpec>(out, value, fmt_spec, tc.frame);
  tc.emit<Branch>(done_block);

  tc.block = pass_through_block;
  tc.emit<RefineType>(out, TUnicodeExact, value);
  tc.emit<Branch>(done_block);

  tc.block = done_block;
  stack.push(out);
}

void HIRBuilder::emitLoadCommonConstant(
    TranslationContext& tc,
    const BytecodeInstruction& bc_instr) {
  Register* out = temps_.AllocateStack();
  tc.emit<LoadConst>(
      out, getContext()->typeForCommonConstant(bc_instr.oparg()));
  tc.frame.stack.push(out);
}

void HIRBuilder::emitLoadSpecial(
    TranslationContext& tc,
    const BytecodeInstruction& bc_instr) {
  OperandStack& stack = tc.frame.stack;
  Register* self = stack.pop();
  Register* method = temps_.AllocateStack();
  Register* null_or_self = temps_.AllocateStack();
  tc.emit<LoadSpecial>(method, self, bc_instr.oparg(), tc.frame);
  tc.emit<GetSecondOutput>(null_or_self, TOptObject, method);
  stack.push(method);
  stack.push(null_or_self);
}

void HIRBuilder::emitSetFunctionAttribute(
    TranslationContext& tc,
    const BytecodeInstruction& bc_instr) {
  OperandStack& stack = tc.frame.stack;
  Register* func = stack.pop();
  Register* value = stack.pop();

  // Map the bytecode oparg to FunctionAttr enum
  FunctionAttr attr;
  switch (bc_instr.oparg()) {
    case MAKE_FUNCTION_DEFAULTS:
      attr = FunctionAttr::kDefaults;
      break;
    case MAKE_FUNCTION_KWDEFAULTS:
      attr = FunctionAttr::kKwDefaults;
      break;
    case MAKE_FUNCTION_ANNOTATIONS:
      attr = FunctionAttr::kAnnotations;
      break;
    case MAKE_FUNCTION_CLOSURE:
      attr = FunctionAttr::kClosure;
      break;
#if PY_VERSION_HEX >= 0x030E0000
    case MAKE_FUNCTION_ANNOTATE:
      attr = FunctionAttr::kAnnotate;
      break;
#endif
    default:
      BUILDER_THROW(
          "Unsupported SET_FUNCTION_ATTRIBUTE oparg: {}", bc_instr.oparg());
  }

  tc.emit<SetFunctionAttr>(value, func, attr);
  stack.push(func);
}

void HIRBuilder::emitLoadBuildClass(TranslationContext& tc) {
  Register* result = temps_.AllocateStack();
  Register* builtins = temps_.AllocateNonStack();
  Register* key = temps_.AllocateNonStack();
  tc.emit<LoadConst>(builtins, Type::fromObject(tc.frame.builtins));
  // Starting at the preloader the JIT seems to assume builtins will be a
  // dictionary, however I'm not sure there's any guarantee of this.
  Register* builtins_dict = temps_.AllocateNonStack();
  tc.emit<GuardType>(builtins_dict, TDictExact, builtins, tc.frame);
  tc.emit<LoadConst>(key, Type::fromObject(getContext()->strBuildClass()));
  tc.emit<DictSubscr>(result, builtins_dict, key, tc.frame);
  tc.frame.stack.push(result);
}

void HIRBuilder::emitStoreGlobal(
    TranslationContext& tc,
    const BytecodeInstruction& bc_instr) {
  Register* globals = temps_.AllocateNonStack();
  Register* key = temps_.AllocateNonStack();

  tc.emit<LoadConst>(globals, Type::fromObject(tc.frame.globals));
  // Starting at the preloader the JIT seems to assume globals will be a
  // dictionary, however I'm not sure there's any guarantee of this.
  Register* globals_dict = temps_.AllocateNonStack();
  tc.emit<GuardType>(globals_dict, TDictExact, globals, tc.frame);
  tc.emit<LoadConst>(
      key,
      Type::fromObject(PyTuple_GET_ITEM(code_->co_names, bc_instr.oparg())));
  Register* value = tc.frame.stack.pop();
  Register* result = temps_.AllocateNonStack();
  tc.emit<SetDictItem>(result, globals_dict, key, value, tc.frame);
}

void HIRBuilder::insertRunPeriodicActivites(
    CFG& cfg,
    BasicBlock* check_block,
    BasicBlock* succ,
    const FrameState& frame) {
  TranslationContext check(check_block, frame);
  TranslationContext body(cfg.AllocateBlock(), frame);
  if constexpr (kFreeThreadedBuild) {
    check.emit<AtQuiescentState>();
  }
  // Check if the eval breaker has been set
  Register* eval_breaker = temps_.AllocateStack();
  check.emit<LoadEvalBreaker>(eval_breaker);
  check.emit<CondBranch>(eval_breaker, body.block, succ);
  // If set, run periodic tasks
  body.emitSnapshot();
  body.emit<RunPeriodicTasks>(temps_.AllocateStack(), body.frame);
  body.emit<Branch>(succ);
}

void HIRBuilder::insertRunPeriodicActivitesForLoop(
    CFG& cfg,
    BasicBlock* loop_header) {
  auto snap = loop_header->entrySnapshot();
  JIT_CHECK(snap != nullptr, "block {} has no entry snapshot", loop_header->id);
  auto fs = snap->frameState();
  JIT_CHECK(
      fs != nullptr,
      "entry snapshot for block {} has no FrameState",
      loop_header->id);
  auto check_block = cfg.AllocateBlock();
  loop_header->retargetPreds(check_block);
  insertRunPeriodicActivites(cfg, check_block, loop_header, *fs);
}

void HIRBuilder::insertRunPeriodicActivitesForBackedge(
    CFG& cfg,
    BasicBlock* src,
    BasicBlock* target,
    const FrameState& frame) {
  // One poll per back edge, on the taken path only.  This is stock 3.11's
  // shape exactly: the eval-breaker check runs inside the backward-jump
  // handlers on the taken branch, so the loop-entry fallthrough never
  // polls and JUMP_BACKWARD_NO_INTERRUPT never polls.  The header-shared
  // alternative -- one check block in front of the loop header, fed by
  // every predecessor -- polls both of those, and can only name the
  // header's frame state.
  //
  // Two frame states serve two consumers.  The service itself
  // (RunPeriodicTasks) carries the state AT the jump: a failure there is
  // stock's "exception raised by the eval-breaker check inside the jump
  // handler", whose traceback names the jump -- the deopt's default
  // advance reproduces that, and a failure never resumes, so the advanced
  // position is never executed.  The bytecode-boundary snapshots around it
  // carry the TARGET's entry state: an instrumentation transition observed
  // by the polls after them resumes at the jump target, which the jump has
  // already reached -- resuming at the jump itself would execute it a
  // second time against a stack that already had its operand popped.
  auto check_block = cfg.AllocateBlock();
  Instr* terminator = src->GetTerminator();
  JIT_CHECK(terminator != nullptr, "backedge source has no terminator");
  bool retargeted = false;
  for (std::size_t i = 0; i < terminator->numEdges(); i++) {
    if (terminator->successor(i) == target) {
      terminator->set_successor(i, check_block);
      retargeted = true;
    }
  }
  JIT_CHECK(
      retargeted,
      "backedge from block {} no longer targets block {}",
      src->id,
      target->id);

  auto snap = target->entrySnapshot();
  JIT_CHECK(snap != nullptr, "block {} has no entry snapshot", target->id);
  auto target_fs = snap->frameState();
  JIT_CHECK(
      target_fs != nullptr,
      "entry snapshot for block {} has no FrameState",
      target->id);

  TranslationContext check(check_block, *target_fs);
  TranslationContext body(cfg.AllocateBlock(), *target_fs);
  if constexpr (kFreeThreadedBuild) {
    check.emit<AtQuiescentState>();
  }
  Register* eval_breaker = temps_.AllocateStack();
  check.emit<LoadEvalBreaker>(eval_breaker);
  check.emit<CondBranch>(eval_breaker, body.block, target);
  body.emitSnapshot();
  body.emit<RunPeriodicTasks>(temps_.AllocateStack(), frame);
  body.emitSnapshot();
  body.emit<Branch>(target);
}

void HIRBuilder::insertRunPeriodicActivitesForExcept(
    CFG& cfg,
    TranslationContext& tc) {
  TranslationContext succ(cfg.AllocateBlock(), tc.frame);
  succ.emitSnapshot();
  insertRunPeriodicActivites(cfg, tc.block, succ.block, tc.frame);
  tc.block = succ.block;
}

ExecutionBlock HIRBuilder::popBlock(CFG& cfg, TranslationContext& tc) {
  if (tc.frame.block_stack.top().opcode == SETUP_FINALLY) {
    insertRunPeriodicActivitesForExcept(cfg, tc);
  }
  return tc.frame.block_stack.pop();
}

BorrowedRef<> HIRBuilder::constArg(const BytecodeInstruction& bc_instr) {
  return PyTuple_GET_ITEM(code_->co_consts, bc_instr.oparg());
}

void HIRBuilder::checkTranslate() {
  PyObject* names = code_->co_names;
  std::unordered_set<Py_ssize_t> banned_name_ids;
#if PY_VERSION_HEX < 0x030C0000
  const char* name_refuse = nullptr;
  auto name_at = [&](Py_ssize_t i) {
    return nameAtOrRefuse(names, i, &name_refuse);
  };
#else
  auto name_at = [&](Py_ssize_t i) {
    return std::string_view(PyUnicode_AsUTF8(PyTuple_GET_ITEM(names, i)));
  };
#endif
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(names); i++) {
    if (isBannedName(name_at(i))) {
      banned_name_ids.insert(i);
    }
#if PY_VERSION_HEX < 0x030C0000
    if (name_refuse != nullptr) {
      throw std::runtime_error{name_refuse};
    }
#endif
  }
  BytecodeInstructionBlock bc_instrs{code_};
  for (auto bc_it = bc_instrs.begin(); bc_it != bc_instrs.end(); ++bc_it) {
    auto bci = *bc_it;
    auto opcode = bci.opcode();
    int oparg = bci.oparg();
    if (!isSupportedOpcode(opcode)) {
      throw std::runtime_error{fmt::format(
          "Cannot compile {} to HIR because it contains unsupported opcode {} "
          "({})",
          preloader_.fullname(),
          opcode,
          opcodeName(opcode))};
    } else if (opcode == LOAD_GLOBAL) {
      auto loaded = name_at(oparg >> 1);
#if PY_VERSION_HEX < 0x030C0000
      if (name_refuse != nullptr) {
        throw std::runtime_error{name_refuse};
      }
#endif
      if ((oparg & 0x01) && loaded == "super") {
        if (!matchLoadSuperAttrPattern311(code_, bc_it, bc_instrs)
                 .has_value()) {
          // LOAD_GLOBAL NULL + super, super isn't being used with a
          // LOAD_SUPER_ATTR or a CPython 3.11 super().attr/method sequence.
          throw std::runtime_error{fmt::format(
              "Cannot compile {} to HIR because it uses super() without an "
              "attribute or method after it",
              preloader_.fullname())};
        }
      }
      oparg = oparg >> 1;
      if (banned_name_ids.contains(oparg)) {
        throw std::runtime_error{fmt::format(
            "Cannot compile {} to HIR because it uses banned global '{}'",
            preloader_.fullname(),
            name_at(oparg))};
      }
    }
  }
}

} // namespace jit::hir
