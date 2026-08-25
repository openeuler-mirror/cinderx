# Copyright (c) Meta Platforms, Inc. and affiliates.

include(FetchContent)
include(GoogleTest)

# Deliberately a tag, not a commit pin: build environments resolve
# github.com through differing lineages (some via curated local mirrors
# whose googletest history is not the public upstream's), so no single
# commit SHA exists in all of them, while the v1.17.0 tag resolves in
# each.  googletest is a test-only dependency and never ships in a
# wheel; pinning it by SHA waits until the lineages are unified
# (issue #18).
cinderx_declare_dependency(
  googletest
  https://github.com/google/googletest.git
  v1.17.0
)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

file(GLOB RUNTIME_TEST_SOURCES CONFIGURE_DEPENDS
  ${PROJECT_SOURCE_DIR}/RuntimeTests/*_test.cpp
  ${PROJECT_SOURCE_DIR}/RuntimeTests/fixtures.cpp
  ${PROJECT_SOURCE_DIR}/RuntimeTests/main.cpp
  ${PROJECT_SOURCE_DIR}/RuntimeTests/testutil.cpp
)

if (${PY_VERSION} EQUAL 3.11)
  # OSR is out of scope on CPython 3.11 (Jit/osr.cpp is excluded from the
  # build in favour of Jit/osr_stub_311.cpp); its tests exercise the OSR
  # C-API and 3.14 frame internals, so they are excluded with it.
  list(FILTER RUNTIME_TEST_SOURCES EXCLUDE REGEX ".*/osr_[a-z_]*_test\\.cpp$")
  # The sanity suite is execution-focused (it runs compiled code and the
  # Static Python data model); the capability-gated 3.11 build executes no
  # machine code and does not build Static Python.  Re-enabled with the
  # execution MR, guarded per test.
  list(FILTER RUNTIME_TEST_SOURCES EXCLUDE REGEX ".*/sanity_test\\.cpp$")
endif()

add_executable(runtime_tests ${RUNTIME_TEST_SOURCES} ${CINDERX_LIB_SOURCES})

target_link_libraries(
  runtime_tests
  PRIVATE
  Python::Python
  borrowed
  cached-properties
  common
  immortalize
  interpreter
  jit
  parallel-gc
  static-python
  asmjit::asmjit
  fmt::fmt
  gtest
)

if(ENABLE_DISASSEMBLER)
  target_link_libraries(runtime_tests PRIVATE capstone_static)
endif()

set(CINDERX_RUNTIME_TESTS_PYTHONLIB
  "${CMAKE_BINARY_DIR}/runtime_tests_pythonpath"
)
file(REMOVE_RECURSE "${CINDERX_RUNTIME_TESTS_PYTHONLIB}")
file(COPY "${PROJECT_SOURCE_DIR}/PythonLib/"
     DESTINATION "${CINDERX_RUNTIME_TESTS_PYTHONLIB}")
file(COPY "${PROJECT_SOURCE_DIR}/PythonLib/opcodes/${PY_VERSION}/opcode.py"
     DESTINATION "${CINDERX_RUNTIME_TESTS_PYTHONLIB}/cinderx")
file(TO_CMAKE_PATH
     "${CINDERX_RUNTIME_TESTS_PYTHONLIB}"
     CINDERX_RUNTIME_TESTS_PYTHONLIB)
target_compile_definitions(
  runtime_tests
  PRIVATE
  CINDERX_RUNTIME_TESTS_CMAKE
  BAKED_IN_PYTHONPATH="${CINDERX_RUNTIME_TESTS_PYTHONLIB}"
)

if(NOT APPLE)
  target_link_options(runtime_tests PRIVATE -Wl,--export-dynamic)
endif()

gtest_discover_tests(
  runtime_tests
  DISCOVERY_TIMEOUT 120
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
)
