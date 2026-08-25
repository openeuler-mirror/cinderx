// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/elf/reader.h"
#include "cinderx/Jit/elf/writer.h"
#include "cinderx/Jit/jit_gdb_support.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

using namespace jit;

using ElfTest = RuntimeTest;

namespace {

bool sectionExists(std::span<const std::byte> bytes, std::string_view name) {
  // Will be non-nullptr if it exists, but can still be empty.
  return elf::findSection(bytes, name).data() != nullptr;
}

std::uint16_t expectedElfMachine() {
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
  return 0x3e;
#elif defined(__aarch64__) || defined(_M_ARM64)
  return 0xb7;
#else
#error Please provide the ELF e_machine value for your architecture.
#endif
}

std::uint16_t readElfMachine(std::span<const std::byte> bytes) {
  std::uint16_t machine = 0;
  constexpr size_t kMachineOffset = offsetof(elf::FileHeader, machine);
  std::memcpy(&machine, bytes.data() + kMachineOffset, sizeof(machine));
  return machine;
}

void verifyElf(std::span<const std::byte> bytes) {
  // Verify the magic ELF bytes at the start.
  ASSERT_EQ(static_cast<uint8_t>(bytes[0]), 0x7f);
  ASSERT_EQ(static_cast<char>(bytes[1]), 'E');
  ASSERT_EQ(static_cast<char>(bytes[2]), 'L');
  ASSERT_EQ(static_cast<char>(bytes[3]), 'F');

  constexpr size_t kMachineOffset = offsetof(elf::FileHeader, machine);
  ASSERT_GE(bytes.size(), kMachineOffset + sizeof(std::uint16_t));
  std::uint16_t machine = readElfMachine(bytes);
  ASSERT_EQ(machine, expectedElfMachine());

  // Standard sections are all there.
  ASSERT_TRUE(sectionExists(bytes, ".text"));
  ASSERT_TRUE(sectionExists(bytes, ".dynsym"));
  ASSERT_TRUE(sectionExists(bytes, ".dynstr"));
  ASSERT_TRUE(sectionExists(bytes, ".dynamic"));
  ASSERT_TRUE(sectionExists(bytes, ".hash"));
  ASSERT_TRUE(sectionExists(bytes, ".shstrtab"));

  // Custom sections are there too.
  ASSERT_TRUE(sectionExists(bytes, elf::kFuncNoteSectionName));
}

std::filesystem::path gdbElfPath(const char* function_name, void* code_addr) {
  std::array<char, 256> path;
  int size = std::snprintf(
      path.data(),
      path.size(),
      "/tmp/cinder_%s_%p_elf",
      function_name,
      code_addr);
  JIT_CHECK(
      size > 0 && static_cast<size_t>(size) < path.size(),
      "Failed to format GDB JIT ELF path");
  return path.data();
}

std::string readFileBytes(const std::filesystem::path& path) {
  std::ifstream file{path, std::ios::binary};
  JIT_CHECK(file.is_open(), "Failed to open {}", path.string());
  return {
      std::istreambuf_iterator<char>{file},
      std::istreambuf_iterator<char>{}};
}

class ScopedGdbConfig {
 public:
  ScopedGdbConfig() {
    auto& gdb_config = jit::getMutableConfig().gdb;
    old_supported_ = gdb_config.supported;
    old_write_elf_objects_ = gdb_config.write_elf_objects;
    gdb_config.supported = true;
    gdb_config.write_elf_objects = true;
  }

  ~ScopedGdbConfig() {
    auto& gdb_config = jit::getMutableConfig().gdb;
    gdb_config.supported = old_supported_;
    gdb_config.write_elf_objects = old_write_elf_objects_;
  }

 private:
  bool old_supported_{false};
  bool old_write_elf_objects_{false};
};

} // namespace

TEST_F(ElfTest, Junk) {
  std::vector<uint8_t> elf;
  elf.push_back(0x7f);
  elf.push_back('E');
  elf.push_back('L');
  elf.push_back('F');
  for (uint8_t i = 1; i < 255; ++i) {
    elf.push_back(i);
  }
  auto bytes = std::as_bytes(std::span{elf});
  ASSERT_THROW(elf::findSection(bytes, ".text"), std::runtime_error);
}

TEST_F(ElfTest, EmptyEntries) {
  std::stringstream ss;
  elf::writeEntries(ss, {});
  std::string result = ss.str();

  verifyElf(std::as_bytes(std::span{result}));
}

TEST_F(ElfTest, OneEntry) {
  constexpr const char* source = R"(
def func(x):
  return x + 1
)";
  Ref<PyObject> func_obj{compileAndGet(source, "func")};
  ASSERT_TRUE(func_obj != nullptr);

  BorrowedRef<PyFunctionObject> func{func_obj};
  BorrowedRef<PyCodeObject> code{func->func_code};
#if PY_VERSION_HEX < 0x030C0000
  alignas(4) std::array<std::byte, 8> compiled_code{};
#else
  std::optional<CompiledFunctionData> compiled_data = Compiler().Compile(func);
  ASSERT_TRUE(compiled_data.has_value());
  auto compiled_func =
      CompiledFunction::create(std::move(*compiled_data), false);
#endif

  std::stringstream ss;

  elf::CodeEntry entry;
  entry.code = code;
#if PY_VERSION_HEX < 0x030C0000
  // Shadow mode does not initialize CompiledFunction, but ELF serialization
  // only needs a code span and entries within it.
  entry.compiled_code = compiled_code;
  entry.normal_entry = compiled_code.data() + 4;
  entry.static_entry = nullptr;
#else
  entry.compiled_code = compiled_func->codeBuffer();
  entry.normal_entry =
      reinterpret_cast<void*>(compiled_func->vectorcallEntry());
  entry.static_entry = compiled_func->staticEntry();
#endif
  entry.func_name = "func";
  entry.file_name = "spaghetti.exe";
  entry.lineno = 15;

  elf::writeEntries(ss, {entry});
  std::string result = ss.str();
  auto result_bytes = std::as_bytes(std::span{result});

  verifyElf(result_bytes);

  std::span<const std::byte> func_note_section =
      elf::findSection(result_bytes, elf::kFuncNoteSectionName);

  elf::NoteArray notes = elf::readNoteSection(func_note_section);
  ASSERT_EQ(notes.notes().size(), 1);
  ASSERT_EQ(notes.notes()[0].name, entry.func_name);

  elf::CodeNoteData note_data = elf::parseCodeNote(notes.notes()[0]);
  ASSERT_EQ(note_data.file_name, entry.file_name);
  ASSERT_EQ(note_data.lineno, entry.lineno);
  ASSERT_GT(note_data.size, 0);
  ASSERT_LT(note_data.size, 10000);
  ASSERT_GT(note_data.normal_entry_offset, 0);
  ASSERT_LT(note_data.normal_entry_offset, 10000);
#if PY_VERSION_HEX < 0x030C0000
  ASSERT_EQ(note_data.size, compiled_code.size());
  ASSERT_EQ(note_data.normal_entry_offset, 4);
#endif
  ASSERT_EQ(note_data.static_entry_offset, std::nullopt);
}

TEST_F(ElfTest, GdbJitElfUsesBuildMachine) {
  const char* function_name = "gdb_jit_elf_machine_test";
  std::array<std::byte, 4> code{
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
  void* code_addr = code.data();
  std::filesystem::path path = gdbElfPath(function_name, code_addr);

  std::filesystem::remove(path);

  ScopedGdbConfig gdb_config;

  ASSERT_EQ(
      register_raw_debug_symbol(
          function_name,
          "gdb_jit_elf_machine_test.py",
          1,
          code_addr,
          code.size(),
          0),
      1);

  auto bytes = readFileBytes(path);
  std::filesystem::remove(path);
  auto byte_span = std::as_bytes(std::span{bytes});
  constexpr size_t kMachineOffset = offsetof(elf::FileHeader, machine);
  ASSERT_GE(byte_span.size(), kMachineOffset + sizeof(std::uint16_t));

  ASSERT_EQ(readElfMachine(byte_span), expectedElfMachine());
}
