// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/code_allocator.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/codegen/code_section.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/trigger_stats.h"

#ifdef WIN32
#include <Windows.h>
#include <memoryapi.h>
#else
#include <sys/mman.h>
#endif

#include <cstring>

namespace jit {

using codegen::CodeSection;
using codegen::codeSectionFromName;

namespace {

#define PROPAGATE_ERROR(EXPR)                                \
  if (asmjit::Error err = (EXPR); err != asmjit::kErrorOk) { \
    return AllocateResult{nullptr, err};                     \
  }

// 2MiB to match Linux's huge-page size.
constexpr size_t kAllocSize = 1024 * 1024 * 2;
constexpr size_t kAArch64InstructionAlignment = 4;

// Allocate memory for JIT'd code.
uint8_t* allocPages(size_t size) {
  // Trigger-proof accounting: every executable-memory allocation the JIT
  // makes funnels through here.
  triggerStatsOnExecutableAlloc(size);
#ifndef WIN32
  void* res = mmap(
      nullptr,
      size,
      PROT_EXEC | PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS,
      -1,
      0);
  JIT_CHECK(
      res != MAP_FAILED,
      "Failed to allocate {} bytes of memory for code",
      size);
#else
  void* res = VirtualAlloc(
      nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  JIT_CHECK(
      res != nullptr, "Failed to allocate {} bytes of memory for code", size);
#endif
  return static_cast<uint8_t*>(res);
}

bool setHugePages([[maybe_unused]] void* ptr, [[maybe_unused]] size_t size) {
#ifdef MADV_HUGEPAGE
  if (madvise(ptr, size, MADV_HUGEPAGE) == 0) {
    return true;
  }

  auto end = static_cast<void*>(static_cast<uint8_t*>(ptr) + size);
  JIT_LOG(
      "Failed to madvise [{}, {}) with MADV_HUGEPAGE, errno={}",
      ptr,
      end,
      errno);
#endif

  return false;
}

void flushInstructionCache(
    [[maybe_unused]] void* ptr,
    [[maybe_unused]] size_t size) {
#if defined(CINDER_AARCH64)
  auto start = static_cast<char*>(ptr);
  __builtin___clear_cache(start, start + size);
#endif
}

} // namespace

SplitAllocationLayout computeSplitAllocationLayout(
    size_t hot_needed,
    size_t cold_needed) {
  size_t total_needed = hot_needed + cold_needed;
  size_t chunk_size = ((total_needed / kAllocSize) + 1) * kAllocSize;
  size_t surplus = chunk_size - total_needed;
  size_t hot_capacity = asmjit::Support::alignUp(
      hot_needed + surplus / 2, kAArch64InstructionAlignment);
  JIT_CHECK(
      hot_capacity + cold_needed <= chunk_size,
      "Aligned split-code layout exceeds its allocation");
  return {chunk_size, hot_capacity};
}

ICodeAllocator* CodeAllocator::make() {
  if (getConfig().use_huge_pages) {
    return new CodeAllocatorCinder{};
  }
  return new CodeAllocator{};
}

AllocateResult CodeAllocator::addCode(asmjit::CodeHolder* code) {
  // The executable-allocation trigger metric counts pool-level
  // allocations: the Cinder allocator counts its chunk mmaps in
  // allocPages(), so this allocator watches the underlying block pool
  // grow rather than counting per-artifact spans -- the metric must not
  // change meaning with the allocator choice.
  const size_t reserved_before =
      runtime_.allocator()->statistics().reservedSize();
  void* addr = nullptr;
  asmjit::Error error = runtime_.add(&addr, code);

  if (addr != nullptr && error == asmjit::kErrorOk) {
    const size_t reserved_after =
        runtime_.allocator()->statistics().reservedSize();
    if (reserved_after > reserved_before) {
      triggerStatsOnExecutableAlloc(reserved_after - reserved_before);
    }
    // Account the physical span rather than the logical code size: the
    // allocator hands back aligned blocks and releaseCode() can only
    // subtract what query() reports, so symmetric accounting is what makes
    // used_bytes_ return to its plateau under compile/release churn --
    // asymmetric accounting underflows the counter one alignment gap at a
    // time.
    asmjit::JitAllocator::Span span;
    JIT_CHECK(
        runtime_.allocator()->query(span, addr) == asmjit::kErrorOk,
        "Code allocator cannot answer for a span it just allocated");
    used_bytes_.fetch_add(span.size(), std::memory_order_relaxed);
  }

  return AllocateResult{addr, error};
}

asmjit::Error CodeAllocator::releaseCode(void* code) {
  // Find the size of the allocated region.
  asmjit::JitAllocator* inner = runtime_.allocator();
  asmjit::JitAllocator::Span span;
  if (auto error = inner->query(span, code); error != asmjit::kErrorOk) {
    return error;
  }

  if (auto error = runtime_.release(code); error != asmjit::kErrorOk) {
    return error;
  }

  used_bytes_.fetch_sub(span.size(), std::memory_order_relaxed);
  return asmjit::kErrorOk;
}

bool CodeAllocator::contains(const void* ptr) const {
  asmjit::JitAllocator::Span unused;
  // asmjit docs don't say that query() is thread-safe, but peeking at the
  // implementation shows that it is.
  return runtime_.allocator()->query(unused, const_cast<void*>(ptr)) ==
      asmjit::kErrorOk;
}

size_t CodeAllocator::usedBytes() const {
  return used_bytes_.load(std::memory_order_relaxed);
}

const asmjit::Environment& CodeAllocator::asmJitEnvironment() const {
  return runtime_.environment();
}

CodeAllocatorCinder::~CodeAllocatorCinder() {
  for (std::span<uint8_t> alloc : allocations_) {
#ifndef WIN32
    JIT_CHECK(
        munmap(alloc.data(), alloc.size()) == 0, "Freeing code memory failed");
#else
    VirtualFree(alloc.data(), 0, MEM_RELEASE);
#endif
  }
}

void CodeAllocatorCinder::ensureSpace(
    uint8_t*& alloc,
    size_t& alloc_free,
    size_t size,
    bool use_huge_pages) {
  if (alloc_free >= size) {
    return;
  }

  lost_bytes_.fetch_add(alloc_free, std::memory_order_relaxed);

  size_t chunk_size = ((size / kAllocSize) + 1) * kAllocSize;
  uint8_t* res = allocPages(chunk_size);
  if (use_huge_pages && setHugePages(res, chunk_size)) {
    huge_allocs_.fetch_add(1, std::memory_order_relaxed);
  } else {
    fragmented_allocs_.fetch_add(1, std::memory_order_relaxed);
  }
  alloc = res;
  allocations_.emplace_back(res, chunk_size);
  alloc_free = chunk_size;
}

void CodeAllocatorCinder::ensureSplitSpace(
    size_t hot_needed,
    size_t cold_needed) {
  if (hot_alloc_free_ >= hot_needed && cold_alloc_free_ >= cold_needed) {
    return;
  }

  // When either side needs a new allocation, allocate a single contiguous
  // region and split it. This guarantees hot and cold code are always within
  // the same mmap region, so cross-section links share one relative address
  // space. Without this,
  // independent mmap() calls could place hot and cold regions too far apart,
  // causing asmjit's relocateToBase()/resolveUnresolvedLinks() to fail with
  // kErrorInvalidDisplacement.
  lost_bytes_.fetch_add(
      hot_alloc_free_ + cold_alloc_free_, std::memory_order_relaxed);

  SplitAllocationLayout layout =
      computeSplitAllocationLayout(hot_needed, cold_needed);
  size_t chunk_size = layout.chunk_size;
  uint8_t* res = allocPages(chunk_size);
  if (setHugePages(res, chunk_size)) {
    huge_allocs_.fetch_add(1, std::memory_order_relaxed);
  } else {
    fragmented_allocs_.fetch_add(1, std::memory_order_relaxed);
  }
  allocations_.emplace_back(res, chunk_size);

  // Hot code grows forward from the start, cold code grows forward from a
  // split point. Split proportionally so each side gets at least what it
  // requested, distributing any surplus evenly.
  size_t hot_share = layout.hot_capacity;
  hot_alloc_ = res;
  hot_alloc_free_ = hot_share;
  cold_alloc_ = res + hot_share;
  cold_alloc_free_ = chunk_size - hot_share;
}

AllocateResult CodeAllocatorCinder::addSplitCode(asmjit::CodeHolder* code) {
  // Compute how much space each section type needs.
  size_t hot_size = 0;
  size_t cold_size = 0;
  for (asmjit::Section* section : code->sections()) {
    CodeSection cs = codeSectionFromName(section->name());
    if (cs == CodeSection::kCold) {
      cold_size += section->realSize();
    } else {
      hot_size += section->realSize();
    }
  }

  // Ensure we have enough space for both hot and cold code.
#if defined(__aarch64__)
  // Keep hot and cold in one relative address space and preserve instruction
  // alignment for cross-section links.
  ensureSplitSpace(hot_size, cold_size);
  JIT_CHECK(
      static_cast<size_t>(cold_alloc_ - hot_alloc_) %
              kAArch64InstructionAlignment ==
          0,
      "AArch64 split-code sections must be instruction-aligned");
#else
  // On x86-64, RIP-relative addressing has a ±2GB range which is large enough
  // that independent allocations are unlikely to exceed it in practice.
  ensureSpace(hot_alloc_, hot_alloc_free_, hot_size, true);
  ensureSpace(
      cold_alloc_,
      cold_alloc_free_,
      cold_size,
      getConfig().cold_code_huge_pages);
#endif

  // Fix up offsets for each code section before resolving links.
  // All offsets are relative to the hot allocation base so that asmjit can
  // resolve cross-section jumps correctly.
  size_t hot_offset = 0;
  size_t cold_offset = static_cast<size_t>(cold_alloc_ - hot_alloc_);
  for (asmjit::Section* section : code->sections()) {
    CodeSection cs = codeSectionFromName(section->name());
    if (cs == CodeSection::kCold) {
      section->setOffset(cold_offset);
      cold_offset += section->realSize();
    } else {
      section->setOffset(hot_offset);
      hot_offset += section->realSize();
    }
  }

  PROPAGATE_ERROR(code->resolveUnresolvedLinks());
  PROPAGATE_ERROR(code->relocateToBase(uintptr_t(hot_alloc_)));

  void* addr = hot_alloc_;

  // Copy each section's data to the appropriate allocation.
  size_t total_size = 0;
  for (asmjit::Section* section : code->_sections) {
    size_t buffer_size = section->bufferSize();
    if (buffer_size == 0) {
      continue;
    }
    CodeSection cs = codeSectionFromName(section->name());
    if (cs == CodeSection::kCold) {
      std::memcpy(cold_alloc_, section->data(), buffer_size);
      flushInstructionCache(cold_alloc_, buffer_size);
      cold_alloc_ += buffer_size;
      cold_alloc_free_ -= buffer_size;
    } else {
      std::memcpy(hot_alloc_, section->data(), buffer_size);
      flushInstructionCache(hot_alloc_, buffer_size);
      hot_alloc_ += buffer_size;
      hot_alloc_free_ -= buffer_size;
    }
    total_size += buffer_size;
  }

  used_bytes_.fetch_add(total_size, std::memory_order_relaxed);
  return AllocateResult{addr, asmjit::kErrorOk};
}

AllocateResult CodeAllocatorCinder::addCode(asmjit::CodeHolder* code) {
  std::lock_guard lock{allocator_mutex_};

  if (getConfig().multiple_code_sections) {
    return addSplitCode(code);
  }

  PROPAGATE_ERROR(code->flatten());
  PROPAGATE_ERROR(code->resolveUnresolvedLinks());

  size_t max_code_size = code->codeSize();
  ensureSpace(hot_alloc_, hot_alloc_free_, max_code_size, true);

  PROPAGATE_ERROR(code->relocateToBase(uintptr_t(hot_alloc_)));

  size_t actual_code_size = code->codeSize();
  JIT_CHECK(actual_code_size <= max_code_size, "Code grew during relocation");

  for (asmjit::Section* section : code->_sections) {
    size_t offset = section->offset();
    size_t buffer_size = section->bufferSize();
    size_t virtual_size = section->virtualSize();

    JIT_CHECK(
        offset + buffer_size <= actual_code_size, "Inconsistent code size");
    std::memcpy(hot_alloc_ + offset, section->data(), buffer_size);

    if (virtual_size > buffer_size) {
      JIT_CHECK(
          offset + virtual_size <= actual_code_size, "Inconsistent code size");
      std::memset(
          hot_alloc_ + offset + buffer_size, 0, virtual_size - buffer_size);
    }
  }

  void* addr = hot_alloc_;
  flushInstructionCache(addr, actual_code_size);

  hot_alloc_ += actual_code_size;
  hot_alloc_free_ -= actual_code_size;
  used_bytes_.fetch_add(actual_code_size, std::memory_order_relaxed);

  return AllocateResult{addr, asmjit::kErrorOk};
}

asmjit::Error CodeAllocatorCinder::releaseCode([[maybe_unused]] void* code) {
  // TODO(T233607793): Actually implement deallocating memory.
  return asmjit::kErrorOk;
}

bool CodeAllocatorCinder::contains(const void* ptr) const {
  std::lock_guard lock{allocator_mutex_};
  for (std::span<uint8_t> alloc : allocations_) {
    if (alloc.data() <= ptr && ptr < alloc.data() + alloc.size()) {
      return true;
    }
  }
  return false;
}

} // namespace jit
