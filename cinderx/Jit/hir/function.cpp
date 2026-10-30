// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/function.h"

#include <algorithm>
#include <limits>

namespace jit::hir {

// Be intentional about HIR structure sizes.  There's no hard limit on what
// these sizes have to be, but we should be aware when we change them.
//
// Ignore it for libc++ and Windows for now though, too tricky to track multiple
// implementations.
#if !defined(_LIBCPP_VERSION) && !defined(WIN32)
#if PY_VERSION_HEX < 0x030C0000
static_assert(sizeof(Function) == 65 * kPointerSize);
#else
static_assert(sizeof(Function) == 60 * kPointerSize);
#endif
static_assert(sizeof(CFG) == 5 * kPointerSize);
static_assert(sizeof(BasicBlock) == 20 * kPointerSize);
static_assert(sizeof(Instr) == 6 * kPointerSize);
#endif

Function::Function() {}

Function::~Function() {
  // Serialize as we alter ref-counts on potentially global objects.
  ThreadedCompileSerialize guard;
  code.reset();
  builtins.reset();
  globals.reset();
  prim_args_info.reset();
}

void Function::setCode(BorrowedRef<PyCodeObject> new_code) {
  code.reset(new_code);
  frameMode = getConfig().frame_mode;
}

std::size_t Function::numBlocks() const {
  return cfg.numBlocks();
}

std::size_t Function::numInstrs() const {
  return cfg.numInstrs();
}

std::size_t Function::CountInstrs(InstrPredicate pred) const {
  std::size_t result = 0;
  for (const auto& block : cfg.blocks) {
    for (const auto& instr : block) {
      if (pred(instr)) {
        result++;
      }
    }
  }
  return result;
}

bool Function::returnsPrimitive() const {
  return return_type <= TPrimitive;
}

bool Function::returnsPrimitiveDouble() const {
  return return_type <= TCDouble;
}

void Function::setCompilationPhaseTimer(
    std::unique_ptr<CompilationPhaseTimer> cpt) {
  compilation_phase_timer = std::move(cpt);
}

int Function::numArgs() const {
  if (code == nullptr) {
    // code might be null if we parsed from textual ir
    return 0;
  }
  return code->co_argcount + code->co_kwonlyargcount +
      bool(code->co_flags & CO_VARARGS) + bool(code->co_flags & CO_VARKEYWORDS);
}

Py_ssize_t Function::numVars() const {
  // Code might be null if we parsed from textual HIR.
  return code != nullptr ? numLocalsplus(code) : 0;
}

bool Function::canDeopt() const {
  for (const BasicBlock& block : cfg.blocks) {
    for (const Instr& instr : block) {
      if (instr.asDeoptBase()) {
        return true;
      }
    }
  }
  return false;
}

namespace {

bool parseExceptionTableVarint(
    const unsigned char*& cursor,
    const unsigned char* end,
    bool is_entry_start,
    int* result) {
  if (cursor >= end) {
    return false;
  }
  if (((cursor[0] & 128) != 0) != is_entry_start) {
    return false;
  }

  unsigned int value = cursor[0] & 63;
  while ((cursor[0] & 64) != 0) {
    cursor++;
    if (cursor >= end) {
      return false;
    }
    if ((cursor[0] & 128) != 0) {
      return false;
    }

    unsigned int chunk = cursor[0] & 63;
    if (value > (std::numeric_limits<int>::max() - chunk) >> 6) {
      return false;
    }
    value = (value << 6) | chunk;
  }
  *result = static_cast<int>(value);
  cursor++;
  return true;
}

} // namespace

bool isInExceptionProtectedRange(
    BorrowedRef<PyCodeObject> code,
    BCOffset target_offset) {
  auto* cursor = reinterpret_cast<const unsigned char*>(
      PyBytes_AS_STRING(code->co_exceptiontable));
  const auto* end = cursor + PyBytes_GET_SIZE(code->co_exceptiontable);
  const int target_index = target_offset.asIndex().value();

  while (cursor < end) {
    int start = 0;
    int size = 0;
    int unused = 0;
    if (!parseExceptionTableVarint(cursor, end, true, &start) ||
        !parseExceptionTableVarint(cursor, end, false, &size) ||
        !parseExceptionTableVarint(cursor, end, false, &unused) ||
        !parseExceptionTableVarint(cursor, end, false, &unused)) {
      return true;
    }
    if (target_index >= start && target_index - start < size) {
      return true;
    }
  }
  return false;
}

namespace {

int findLocalsplusIndex(const FrameState& fs, Register* reg) {
  auto it = std::find(fs.localsplus.begin(), fs.localsplus.end(), reg);
  return it == fs.localsplus.end() ? -1 : it - fs.localsplus.begin();
}

} // namespace

void Function::markOSREntries(
    const std::vector<BCOffset>& offsets,
    BorrowedRef<PyCodeObject> osr_code) {
  UnorderedSet<BCOffset> wanted(offsets.begin(), offsets.end());
  for (auto& block : cfg.blocks) {
    Snapshot* snapshot = block.entrySnapshot();
    if (snapshot == nullptr) {
      continue;
    }
    FrameState* fs = snapshot->frameState();
    if (fs == nullptr || !wanted.contains(fs->instrOffset()) ||
        !isEligibleOSREntry(*fs, osr_code)) {
      continue;
    }

    OSREntry* entry = OSREntry::create(fs->instrOffset());
    block.insert(entry, std::next(block.iterator_to(*snapshot)));
    osr_entries_.emplace(fs->instrOffset(), entry);
  }
}

bool Function::hasOSREntries() const {
  return !osr_entries_.empty();
}

bool Function::isEligibleOSREntry(
    const FrameState& fs,
    BorrowedRef<PyCodeObject> osr_code) const {
  return fs.parent == nullptr && fs.stack.isEmpty() &&
      fs.block_stack.isEmpty() && !osr_entries_.contains(fs.instrOffset()) &&
      !isInExceptionProtectedRange(osr_code, fs.instrOffset());
}

void Function::refreshOSREntries() {
  osr_entries_.clear();
  osr_metadata_.clear();
  for (auto& block : cfg.blocks) {
    for (auto& instr : block) {
      if (!instr.IsOSREntry()) {
        continue;
      }
      auto& entry = static_cast<OSREntry&>(instr);
      osr_entries_[entry.targetOffset()] = &entry;
    }
  }
}

void Function::extractOSRLiveIns() {
  refreshOSREntries();

  std::vector<BCOffset> rejected;
  for (auto& [offset, entry] : osr_entries_) {
    FrameState* fs = entry->frameState();
    if (fs == nullptr) {
      rejected.push_back(offset);
      continue;
    }

    std::vector<OSRLiveIn> live_ins;
    bool reject = false;
    for (const RegState& state : entry->live_regs()) {
      OSRLiveIn live_in;
      live_in.localsplus_index = findLocalsplusIndex(*fs, state.reg);
      live_in.stack_index = -1;
      live_in.value_kind = state.value_kind;
      live_in.ref_kind = state.ref_kind;
      live_in.reconstructible =
          state.value_kind == ValueKind::kObject &&
          state.ref_kind == RefKind::kOwned &&
          live_in.localsplus_index >= 0;
      if (!live_in.reconstructible) {
        reject = true;
        break;
      }
      live_in.is_phi = state.reg->instr()->IsPhi();
      live_in.hir_reg = state.reg;
      live_ins.emplace_back(live_in);
    }
    if (reject) {
      rejected.push_back(offset);
      continue;
    }

    OSRMetadata metadata;
    metadata.target_offset = offset;
    metadata.live_ins.resize(live_ins.size());
    metadata.owned_ref_count = live_ins.size();
    for (std::size_t i = 0; i < live_ins.size(); ++i) {
      metadata.live_ins[i] = live_ins[i];
    }
    osr_metadata_.emplace(entry, std::move(metadata));
  }

  for (BCOffset offset : rejected) {
    auto it = osr_entries_.find(offset);
    if (it == osr_entries_.end()) {
      continue;
    }
    OSREntry* entry = it->second;
    entry->unlink();
    delete entry;
    osr_entries_.erase(it);
  }
}

const OSRMetadata* Function::osrMetadataFor(const OSREntry& entry) const {
  auto it = osr_metadata_.find(&entry);
  return it == osr_metadata_.end() ? nullptr : &it->second;
}

BorrowedRef<PyCodeObject> Function::codeFor(const Instr& instr) const {
  if (instr.IsBeginInlinedFunction()) {
    auto bif = static_cast<const BeginInlinedFunction*>(&instr);
    return bif->func()->func_code;
  }
  if (instr.IsLoadGlobalCached()) {
    auto load_global = static_cast<const LoadGlobalCached*>(&instr);
    return load_global->code();
  }
  if (auto deopt_base = instr.asDeoptBase()) {
    auto fs = deopt_base->frameState();
    return fs != nullptr ? fs->code : nullptr;
  }
  const FrameState* fs = instr.getDominatingFrameState();
  return fs == nullptr ? code : fs->code;
}

OpcodeCounts count_opcodes(const Function& func) {
  OpcodeCounts counts{};
  for (const BasicBlock& block : func.cfg.blocks) {
    for (const Instr& instr : block) {
      counts[static_cast<size_t>(instr.opcode())]++;
    }
  }
  return counts;
}

} // namespace jit::hir
