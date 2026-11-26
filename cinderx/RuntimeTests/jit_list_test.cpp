// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Common/util.h"
#include "cinderx/Jit/jit_list.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

using JITListTest = RuntimeTest;
using WildcardJITListTest = RuntimeTest;

using jit::JITList;
using jit::WildcardJITList;

TEST_F(JITListTest, ParseLine) {
  auto jitlist = JITList::create();
  ASSERT_NE(jitlist, nullptr);

  // Valid
  EXPECT_TRUE(jitlist->parseLine("foo:bar"));
  EXPECT_TRUE(jitlist->parseLine(""));
  EXPECT_TRUE(jitlist->parseLine("# foo"));
  EXPECT_TRUE(jitlist->parseLine("    foo:bar"));
  EXPECT_TRUE(jitlist->parseLine("foo:bar   "));
  EXPECT_TRUE(jitlist->parseLine("    foo:bar   "));

  // Invalid
  EXPECT_FALSE(jitlist->parseLine("foo"));
}

TEST_F(JITListTest, LookupName) {
  auto jitlist = JITList::create();
  ASSERT_NE(jitlist, nullptr);

  ASSERT_TRUE(jitlist->parseLine("foo:bar"));
  ASSERT_TRUE(jitlist->parseLine("foo:baz"));

  auto foo = Ref<>::steal(PyUnicode_FromString("foo"));
  ASSERT_NE(foo, nullptr);
  auto bar = Ref<>::steal(PyUnicode_FromString("bar"));
  ASSERT_NE(bar, nullptr);
  auto baz = Ref<>::steal(PyUnicode_FromString("baz"));
  ASSERT_NE(baz, nullptr);
  auto quux = Ref<>::steal(PyUnicode_FromString("quux"));
  ASSERT_NE(quux, nullptr);

  EXPECT_TRUE(jitlist->lookupName(foo, bar));
  EXPECT_TRUE(jitlist->lookupName(foo, baz));
  EXPECT_FALSE(jitlist->lookupName(foo, quux));
  EXPECT_FALSE(jitlist->lookupName(quux, bar));
}

TEST_F(JITListTest, LookupFuncCode) {
  auto jitlist = JITList::create();
  ASSERT_NE(jitlist, nullptr);

  auto obj = compileAndGet("def f(): pass", "f");

  BorrowedRef<PyFunctionObject> func =
      reinterpret_cast<PyFunctionObject*>(obj.get());
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jitlist->lookupFunc(func), 0);

  BorrowedRef<PyCodeObject> code =
      reinterpret_cast<PyCodeObject*>(func->func_code);
  ASSERT_NE(code, nullptr);
  ASSERT_EQ(jitlist->lookupCode(code), 0);
}

TEST_F(WildcardJITListTest, ParseLine) {
  auto jitlist = WildcardJITList::create();
  ASSERT_NE(jitlist, nullptr);
  ASSERT_FALSE(jitlist->parseLine("*:*"));
}

TEST_F(WildcardJITListTest, Lookup) {
  auto jitlist = WildcardJITList::create();
  ASSERT_NE(jitlist, nullptr);

  ASSERT_TRUE(jitlist->parseLine("foo:*"));
  ASSERT_TRUE(jitlist->parseLine("*:baz"));
  ASSERT_TRUE(jitlist->parseLine("bar:quux"));
  ASSERT_TRUE(jitlist->parseLine("*:*.__init__"));
  ASSERT_TRUE(jitlist->parseLine("foo:*.evaluate"));

  auto foo = Ref<>::steal(PyUnicode_FromString("foo"));
  ASSERT_NE(foo, nullptr);
  auto bar = Ref<>::steal(PyUnicode_FromString("bar"));
  ASSERT_NE(bar, nullptr);
  auto baz = Ref<>::steal(PyUnicode_FromString("baz"));
  ASSERT_NE(baz, nullptr);
  auto quux = Ref<>::steal(PyUnicode_FromString("quux"));
  ASSERT_NE(quux, nullptr);
  auto foo_init = Ref<>::steal(PyUnicode_FromString("Foo.__init__"));
  ASSERT_NE(foo_init, nullptr);
  auto foo_evaluate = Ref<>::steal(PyUnicode_FromString("Foo.evaluate"));
  ASSERT_NE(foo_evaluate, nullptr);
  auto foo_bar_evaluate =
      Ref<>::steal(PyUnicode_FromString("Foo.Bar.evaluate"));
  ASSERT_NE(foo_bar_evaluate, nullptr);

  // All funcs in foo are enabled
  EXPECT_TRUE(jitlist->lookupName(foo, bar));
  EXPECT_TRUE(jitlist->lookupName(foo, baz));
  EXPECT_TRUE(jitlist->lookupName(foo, quux));

  // All qualnames of baz are enabled
  EXPECT_TRUE(jitlist->lookupName(quux, baz));

  // Can't wildcard everything
  EXPECT_FALSE(jitlist->lookupName(bar, foo));

  // Exact lookups should still work
  EXPECT_TRUE(jitlist->lookupName(bar, quux));

  // Unconditionally wildcarded instance methods
  EXPECT_TRUE(jitlist->lookupName(bar, foo_init));
  EXPECT_TRUE(jitlist->lookupName(quux, foo_init));

  // Per-module wildcarded instance methods
  EXPECT_TRUE(jitlist->lookupName(foo, foo_evaluate));
  EXPECT_TRUE(jitlist->lookupName(foo, foo_bar_evaluate));
  EXPECT_FALSE(jitlist->lookupName(bar, foo_evaluate));
}

using JITListExtendedTest = RuntimeTest;

using jit::JITList;
using jit::WildcardJITList;

TEST_F(JITListExtendedTest, ParseMultipleEntries) {
  auto jitlist = JITList::create();
  ASSERT_NE(jitlist, nullptr);

  EXPECT_TRUE(jitlist->parseLine("module_a:func_x"));
  EXPECT_TRUE(jitlist->parseLine("module_b:func_y"));
  EXPECT_TRUE(jitlist->parseLine("module_c:func_z"));

  auto mod_a = Ref<>::steal(PyUnicode_FromString("module_a"));
  auto func_x = Ref<>::steal(PyUnicode_FromString("func_x"));
  ASSERT_NE(mod_a, nullptr);
  ASSERT_NE(func_x, nullptr);

  EXPECT_EQ(jitlist->lookupName(mod_a, func_x), 1);
}

TEST_F(JITListExtendedTest, ParseLineWithWhitespace) {
  auto jitlist = JITList::create();
  ASSERT_NE(jitlist, nullptr);

  EXPECT_TRUE(jitlist->parseLine("   module:func   "));
  EXPECT_TRUE(jitlist->parseLine("\tmodule2:func2"));
}


TEST_F(JITListExtendedTest, LookupNonExistentModule) {
  auto jitlist = JITList::create();
  ASSERT_NE(jitlist, nullptr);

  EXPECT_TRUE(jitlist->parseLine("real_module:real_func"));

  auto fake_mod = Ref<>::steal(PyUnicode_FromString("fake_module"));
  auto fake_func = Ref<>::steal(PyUnicode_FromString("fake_func"));
  ASSERT_NE(fake_mod, nullptr);
  ASSERT_NE(fake_func, nullptr);

  EXPECT_EQ(jitlist->lookupName(fake_mod, fake_func), 0);
}

TEST_F(JITListExtendedTest, LookupNonExistentFunc) {
  auto jitlist = JITList::create();
  ASSERT_NE(jitlist, nullptr);

  EXPECT_TRUE(jitlist->parseLine("my_module:existing_func"));

  auto my_mod = Ref<>::steal(PyUnicode_FromString("my_module"));
  auto missing_func = Ref<>::steal(PyUnicode_FromString("missing_func"));
  ASSERT_NE(my_mod, nullptr);
  ASSERT_NE(missing_func, nullptr);

  EXPECT_EQ(jitlist->lookupName(my_mod, missing_func), 0);
}


TEST_F(JITListExtendedTest, LookupFuncCompiled) {
  auto jitlist = JITList::create();
  ASSERT_NE(jitlist, nullptr);

  EXPECT_TRUE(jitlist->parseLine("jittestmodule:test_func"));

  const char* py_src = R"(
def test_func():
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "test_func"));
  ASSERT_NE(func, nullptr);

  int result = jitlist->lookupFunc(func);
  EXPECT_TRUE(result == 0 || result == 1);
}

TEST_F(JITListExtendedTest, LookupCodeCompiled) {
  auto jitlist = JITList::create();
  ASSERT_NE(jitlist, nullptr);

  EXPECT_TRUE(jitlist->parseLine("jittestmodule:test_code"));

  const char* py_src = R"(
def test_code():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "test_code"));
  ASSERT_NE(func, nullptr);

  BorrowedRef<PyCodeObject> code = func->func_code;
  int result = jitlist->lookupCode(code);
  EXPECT_TRUE(result == 0 || result == 1);
}

TEST_F(JITListExtendedTest, WildcardParseModule) {
  auto jitlist = WildcardJITList::create();
  ASSERT_NE(jitlist, nullptr);

  EXPECT_TRUE(jitlist->parseLine("my_module:*"));

  auto my_mod = Ref<>::steal(PyUnicode_FromString("my_module"));
  auto any_func = Ref<>::steal(PyUnicode_FromString("any_func"));
  ASSERT_NE(my_mod, nullptr);
  ASSERT_NE(any_func, nullptr);

  EXPECT_EQ(jitlist->lookupName(my_mod, any_func), 1);
}

TEST_F(JITListExtendedTest, WildcardParseMethod) {
  auto jitlist = WildcardJITList::create();
  ASSERT_NE(jitlist, nullptr);

  EXPECT_TRUE(jitlist->parseLine("*:MyClass.method"));

  auto any_mod = Ref<>::steal(PyUnicode_FromString("any_mod"));
  auto method = Ref<>::steal(PyUnicode_FromString("MyClass.method"));
  ASSERT_NE(any_mod, nullptr);
  ASSERT_NE(method, nullptr);

  EXPECT_EQ(jitlist->lookupName(any_mod, method), 1);
}

TEST_F(JITListExtendedTest, WildcardParseAll) {
  auto jitlist = WildcardJITList::create();
  ASSERT_NE(jitlist, nullptr);

  EXPECT_FALSE(jitlist->parseLine("*:*"));
}

TEST_F(JITListExtendedTest, WildcardLookupExact) {
  auto jitlist = WildcardJITList::create();
  ASSERT_NE(jitlist, nullptr);

  EXPECT_TRUE(jitlist->parseLine("exact_module:exact_func"));

  auto exact_mod = Ref<>::steal(PyUnicode_FromString("exact_module"));
  auto exact_func = Ref<>::steal(PyUnicode_FromString("exact_func"));
  auto wrong_func = Ref<>::steal(PyUnicode_FromString("wrong_func"));
  ASSERT_NE(exact_mod, nullptr);
  ASSERT_NE(exact_func, nullptr);
  ASSERT_NE(wrong_func, nullptr);

  EXPECT_EQ(jitlist->lookupName(exact_mod, exact_func), 1);
  EXPECT_EQ(jitlist->lookupName(exact_mod, wrong_func), 0);
}


TEST_F(JITListExtendedTest, ParseFileWithContent) {
  auto jitlist = JITList::create();
  ASSERT_NE(jitlist, nullptr);

  std::error_code error;
  const auto tmp_dir = std::filesystem::temp_directory_path(error);
  ASSERT_FALSE(error) << error.message();
  std::string tmp_path = (tmp_dir / "cinderx-jit-list-XXXXXX").string();
  const int fd = ::mkstemp(tmp_path.data());
  ASSERT_GE(fd, 0) << "Unable to create a temporary file in " << tmp_dir;
  SCOPE_EXIT({
    ::close(fd);
    ::unlink(tmp_path.c_str());
  });

  constexpr char kContents[] =
      "test_module:test_func1\n"
      "test_module:test_func2\n"
      "# comment line\n"
      "other_module:other_func\n";
  constexpr auto kSize = static_cast<ssize_t>(sizeof(kContents) - 1);
  ASSERT_EQ(::write(fd, kContents, kSize), kSize);

  jitlist->parseFile(tmp_path.c_str());

  auto test_mod = Ref<>::steal(PyUnicode_FromString("test_module"));
  auto func1 = Ref<>::steal(PyUnicode_FromString("test_func1"));
  auto func2 = Ref<>::steal(PyUnicode_FromString("test_func2"));
  auto other_mod = Ref<>::steal(PyUnicode_FromString("other_module"));
  auto other_func = Ref<>::steal(PyUnicode_FromString("other_func"));
  ASSERT_NE(test_mod, nullptr);
  ASSERT_NE(func1, nullptr);
  ASSERT_NE(func2, nullptr);
  ASSERT_NE(other_mod, nullptr);
  ASSERT_NE(other_func, nullptr);

  EXPECT_EQ(jitlist->lookupName(test_mod, func1), 1);
  EXPECT_EQ(jitlist->lookupName(test_mod, func2), 1);
  EXPECT_EQ(jitlist->lookupName(other_mod, other_func), 1);
}
