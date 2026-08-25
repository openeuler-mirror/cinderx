# (c) Meta Platforms, Inc. and affiliates.

message("=====================================================================")
message("=                            WARNING                                =")
message("=====================================================================")
message("=                                                                   =")
message("= This CMake build is not well supported, and is primarily used for =")
message("= experimentation.  Do not expect this to work on your machine.     =")
message("=====================================================================")
message("")

cmake_minimum_required(VERSION 3.12)
project(_cinderx)

set(PROJECT_SOURCE_DIR ${PROJECT_SOURCE_DIR}/cinderx)

##############################################################################
# PGO (Profile-Guided Optimization) configuration

option(ENABLE_PGO_GENERATE "Build with PGO profile generation" OFF)
option(ENABLE_PGO_USE "Build with PGO profile use" OFF)
option(ENABLE_COVERAGE "Build with GCC coverage instrumentation" OFF)
set(PGO_PROFILE_FILE "" CACHE STRING "Path to PGO profile data")

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  set(MACOS 1)
  set(WINDOWS 0)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Windows")
  set(MACOS 0)
  set(WINDOWS 1)
else()
  set(MACOS 0)
endif()

##############################################################################
# Figure out capabilities / supported features and set compiler flags
# appropriately.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (WINDOWS)
    set(SHARED_FLAGS "-DPy_BUILD_CORE -DPy_BUILD_CORE_MODULE")
    set(CXX_FLAGS "")
else()
    set(SHARED_FLAGS "-DPy_BUILD_CORE -DPy_BUILD_CORE_MODULE -fPIC -fvisibility=hidden -Wall -Wextra -Wno-attributes -Wno-c99-designator -Wno-c++11-narrowing -Wno-cast-function-type -Wno-cast-function-type-mismatch -Wno-deprecated-declarations -Wno-missing-field-initializers -Wno-null-pointer-subtraction -Wno-parentheses -Wno-sign-compare -Wno-unknown-pragmas -Wno-unused-function -Wno-unused-parameter")
    set(CXX_FLAGS "-fvisibility-inlines-hidden")
endif()

macro(set_flag VAR)
  if (DEFINED ${VAR} AND ${${VAR}})
    set(SHARED_FLAGS "-D${VAR} ${SHARED_FLAGS}")
  endif()
endmacro()

if (DEFINED PY_VERSION AND ${PY_VERSION} EQUAL 3.11)
  # There is no 3.11 product without the interpreter loop: the runtime paths
  # call into it unconditionally, so the option cannot be turned off here.
  set(ENABLE_INTERPRETER_LOOP ON)
  # The materialized _PyInterpreterFrame is the only frame mode in the 3.11
  # delivery; a lightweight-frames build would silently change frame
  # ownership semantics, so it fails here instead of at runtime.
  if (DEFINED ENABLE_LIGHTWEIGHT_FRAMES AND ${ENABLE_LIGHTWEIGHT_FRAMES})
    message(FATAL_ERROR "ENABLE_LIGHTWEIGHT_FRAMES is not supported on CPython 3.11")
  endif()
endif()

set_flag(ENABLE_ADAPTIVE_STATIC_PYTHON)
set_flag(ENABLE_DISASSEMBLER)
set_flag(ENABLE_ELF_READER)
set_flag(ENABLE_EVAL_HOOK)
set_flag(ENABLE_FUNC_EVENT_MODIFY_QUALNAME)
set_flag(ENABLE_GENERATOR_AWAITER)
set_flag(ENABLE_INTERPRETER_LOOP)
set_flag(ENABLE_LAZY_IMPORTS)
set_flag(ENABLE_LIGHTWEIGHT_FRAMES)
set_flag(ENABLE_PARALLEL_GC)
set_flag(ENABLE_PEP523_HOOK)
set_flag(ENABLE_PERF_TRAMPOLINE)
set_flag(ENABLE_SYMBOLIZER)
set_flag(ENABLE_USDT)
set_flag(ENABLE_ZLIB)
set_flag(ENABLE_XXCLASSLOADER)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SHARED_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CXX_FLAGS} ${SHARED_FLAGS}")

##############################################################################
# Apply LTO (Link-Time Optimization) if enabled

option(ENABLE_LTO "Enable Link-Time Optimization (full)" OFF)

if(ENABLE_COVERAGE AND ENABLE_LTO)
  message(FATAL_ERROR "ENABLE_COVERAGE cannot be combined with ENABLE_LTO")
endif()

if(ENABLE_COVERAGE AND (ENABLE_PGO_GENERATE OR ENABLE_PGO_USE))
  message(FATAL_ERROR "ENABLE_COVERAGE cannot be combined with PGO")
endif()

if(ENABLE_LTO)
  # Only support Linux
  if(MACOS)
    message(FATAL_ERROR "LTO is only supported on Linux")
  endif()

  message(STATUS "LTO: Enabled (full LTO)")

  # Detect compiler type
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(USING_CLANG TRUE)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(USING_GCC TRUE)
  else()
    message(FATAL_ERROR "LTO is only supported with Clang or GCC compilers")
  endif()

  # For Clang, we need llvm-ar instead of regular ar
  if(USING_CLANG)
    find_program(LLVM_AR llvm-ar)
    if(NOT LLVM_AR)
      message(FATAL_ERROR "llvm-ar is required for LTO with Clang but was not found")
    endif()
    set(CMAKE_AR ${LLVM_AR})
    message(STATUS "LTO: Using llvm-ar: ${CMAKE_AR}")

    set(LTO_FLAG "-flto")
    set(LTO_LINKER_FLAGS "-flto")
  elseif(USING_GCC)
    set(LTO_FLAG "-flto")
    set(LTO_LINKER_FLAGS "-flto -fuse-linker-plugin -ffat-lto-objects")
  endif()

  # Apply flags to all C and C++ compilation
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${LTO_FLAG}")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${LTO_FLAG}")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${LTO_LINKER_FLAGS}")
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${LTO_LINKER_FLAGS}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${LTO_LINKER_FLAGS}")
endif()

##############################################################################
# Apply coverage flags if enabled

if(ENABLE_COVERAGE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    message(FATAL_ERROR "ENABLE_COVERAGE only supports GCC/gcov")
  endif()

  find_program(GCOV_EXECUTABLE gcov REQUIRED)
  message(STATUS "Coverage: Enabled with ${GCOV_EXECUTABLE}")

  foreach(lang C CXX)
    set(CMAKE_${lang}_FLAGS "${CMAKE_${lang}_FLAGS} --coverage -O0 -g -fno-lto")
    foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
      set(CMAKE_${lang}_FLAGS_${config} "${CMAKE_${lang}_FLAGS_${config}} -O0 -g")
    endforeach()
  endforeach()
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} --coverage -fno-lto")
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} --coverage -fno-lto")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage -fno-lto")
endif()

##############################################################################
# Apply PGO flags if enabled

if(ENABLE_PGO_GENERATE)
  message(STATUS "PGO: Building with profile generation instrumentation")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-instr-generate")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-instr-generate")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fprofile-instr-generate")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-generate")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-generate")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fprofile-generate")
  else()
    message(WARNING "PGO profile generation is not supported with compiler: ${CMAKE_CXX_COMPILER_ID}")
  endif()
endif()

if(ENABLE_PGO_USE)
  message(STATUS "PGO: Building with profile-guided optimizations")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(NOT PGO_PROFILE_FILE)
      message(FATAL_ERROR "PGO_PROFILE_FILE must be set when ENABLE_PGO_USE=ON")
    endif()
    if(NOT EXISTS "${PGO_PROFILE_FILE}")
      message(FATAL_ERROR "PGO profile file does not exist: ${PGO_PROFILE_FILE}")
    endif()
    message(STATUS "PGO: Using profile data from ${PGO_PROFILE_FILE}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-instr-use=${PGO_PROFILE_FILE}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-instr-use=${PGO_PROFILE_FILE}")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    message(STATUS "PGO: Using GCC profile data from build directory")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-use -fprofile-correction")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-use -fprofile-correction")
  else()
    message(WARNING "PGO profile use is not supported with compiler: ${CMAKE_CXX_COMPILER_ID}")
  endif()
endif()

set(GENERATED_HEADER_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_HEADER_DIR})

option(ENABLE_RUNTIME_TESTS "Build RuntimeTests with CMake" OFF)

##############################################################################
# Dependencies

include(FetchContent)

set(CINDERX_LOCAL_DEPS_DIR "" CACHE PATH "Local cache directory for CMake FetchContent dependencies")

function(cinderx_normalize_git_url URL OUT_VAR)
  set(normalized_url "${URL}")
  string(REGEX REPLACE "/+$" "" normalized_url "${normalized_url}")
  string(REGEX REPLACE "\\.git$" "" normalized_url "${normalized_url}")
  set(${OUT_VAR} "${normalized_url}" PARENT_SCOPE)
endfunction()

function(cinderx_local_dep_matches NAME REPOSITORY TAG LOCAL_DIR OUT_VAR)
  set(matches FALSE)

  if(IS_DIRECTORY "${LOCAL_DIR}/.git")
    execute_process(
      COMMAND "${CINDERX_GIT_EXECUTABLE}" -C "${LOCAL_DIR}" remote get-url origin
      RESULT_VARIABLE remote_result
      OUTPUT_VARIABLE remote_url
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE)

    cinderx_normalize_git_url("${remote_url}" normalized_remote_url)
    cinderx_normalize_git_url("${REPOSITORY}" normalized_repository)

    if(remote_result EQUAL 0 AND normalized_remote_url STREQUAL normalized_repository)
      execute_process(
        COMMAND "${CINDERX_GIT_EXECUTABLE}" -C "${LOCAL_DIR}" rev-parse HEAD
        RESULT_VARIABLE current_result
        OUTPUT_VARIABLE current_commit
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)

      if(current_result EQUAL 0)
        if("${TAG}" MATCHES "^[0-9a-fA-F]{40}$")
          string(TOLOWER "${TAG}" expected_commit)
          string(TOLOWER "${current_commit}" current_commit)
          if(current_commit STREQUAL expected_commit)
            set(matches TRUE)
          endif()
        else()
          execute_process(
            COMMAND "${CINDERX_GIT_EXECUTABLE}" -C "${LOCAL_DIR}" rev-parse "${TAG}^{commit}"
            RESULT_VARIABLE expected_result
            OUTPUT_VARIABLE expected_commit
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE)
          if(expected_result EQUAL 0 AND current_commit STREQUAL expected_commit)
            set(matches TRUE)
          endif()
        endif()
      endif()
    endif()
  endif()

  # A dirty checkout must not be compiled while the provenance claims the
  # clean declared commit; refresh it instead of trusting it.
  if(matches)
    execute_process(
      COMMAND "${CINDERX_GIT_EXECUTABLE}" -C "${LOCAL_DIR}" status --porcelain
      RESULT_VARIABLE status_result
      OUTPUT_VARIABLE status_output
      ERROR_QUIET)
    if(NOT status_result EQUAL 0 OR NOT status_output STREQUAL "")
      message(STATUS "Local dependency ${NAME} checkout is dirty; refreshing")
      set(matches FALSE)
    endif()
  endif()

  set(${OUT_VAR} ${matches} PARENT_SCOPE)
endfunction()

function(cinderx_declare_dependency NAME REPOSITORY TAG)
  # 本地构建
  if(CINDERX_LOCAL_DEPS_DIR)
    if(NOT CINDERX_GIT_EXECUTABLE)
      find_program(CINDERX_GIT_EXECUTABLE git REQUIRED)
    endif()

    get_filename_component(local_deps_root "${CINDERX_LOCAL_DEPS_DIR}" ABSOLUTE)
    file(MAKE_DIRECTORY "${local_deps_root}")
    set(local_dir "${local_deps_root}/${NAME}")
    # 缓存校验
    cinderx_local_dep_matches(
      "${NAME}"
      "${REPOSITORY}"
      "${TAG}"
      "${local_dir}"
      local_dep_matches)

    if(NOT local_dep_matches)
      message(STATUS "Refreshing local dependency ${NAME} in ${local_dir}")
      file(REMOVE_RECURSE "${local_dir}")
      execute_process(
        COMMAND "${CINDERX_GIT_EXECUTABLE}" clone "${REPOSITORY}" "${local_dir}"
        RESULT_VARIABLE clone_result)
      if(NOT clone_result EQUAL 0)
        message(FATAL_ERROR "Failed to clone ${NAME} from ${REPOSITORY} into ${local_dir}")
      endif()

      execute_process(
        COMMAND "${CINDERX_GIT_EXECUTABLE}" -C "${local_dir}" checkout --detach "${TAG}"
        RESULT_VARIABLE checkout_result)
      if(NOT checkout_result EQUAL 0)
        message(FATAL_ERROR "Failed to checkout ${NAME} at ${TAG} in ${local_dir}")
      endif()
    else()
      message(STATUS "Using local dependency ${NAME} from ${local_dir}")
    endif()

    FetchContent_Declare(${NAME} SOURCE_DIR "${local_dir}")
  else()
  # 网络构建
    FetchContent_Declare(
      ${NAME}
      GIT_REPOSITORY "${REPOSITORY}"
      GIT_TAG "${TAG}"
    )
  endif()
endfunction()

########################################
# python

if(ENABLE_RUNTIME_TESTS)
  find_package(
    Python ${PY_VERSION} EXACT
    COMPONENTS Interpreter Development.Module Development.Embed
    REQUIRED)
else()
  find_package(Python ${PY_VERSION} EXACT COMPONENTS Interpreter Development.Module REQUIRED)
endif()

# Some of our files are partially generated from CPython source, and they
# expect to be able to include files relative to Include/internal.
set(Python_INCLUDE_DIRS ${Python_INCLUDE_DIRS} "${Python_INCLUDE_DIRS}/internal")

########################################
# asmjit (vendored)

set(ASMJIT_STATIC TRUE)
set(ASMJIT_NO_CUSTOM_FLAGS TRUE)
add_subdirectory(${PROJECT_SOURCE_DIR}/ThirdParty/asmjit asmjit)
if (NOT WINDOWS)
    target_compile_options(asmjit PRIVATE -Wno-class-memaccess)
endif()

########################################
# fmt

cinderx_declare_dependency(
  fmt
  https://github.com/fmtlib/fmt
  40626af88bd7df9a5fb80be7b25ac85b122d6c21 # tag 11.2.0, 2025-May-03
)
FetchContent_MakeAvailable(fmt)

########################################
# parallel-hashmap

cinderx_declare_dependency(
  parallel-hashmap
  https://github.com/greg7mdp/parallel-hashmap
  896f1a03e429c45d9fe9638e892fc1da73befadd # 2025-Apr-11
)
FetchContent_MakeAvailable(parallel-hashmap)

########################################
# usdt

cinderx_declare_dependency(
  usdt
  https://github.com/libbpf/usdt
  f4ea2f524efa80d062f4d586d78daafb83dc7d24 # 2025-Apr-24
)
FetchContent_MakeAvailable(usdt)

# Get directory format for `#include <usdt/usdt.h>` to work.
set(USDT_DIR ${GENERATED_HEADER_DIR}/usdt)
file(MAKE_DIRECTORY ${USDT_DIR})
file(COPY ${usdt_SOURCE_DIR}/usdt.h DESTINATION ${USDT_DIR})

########################################
# zlib

if (${ENABLE_ZLIB})
    FetchContent_Declare(
        zlib
        GIT_REPOSITORY "https://github.com/madler/zlib.git"
        FIND_PACKAGE_ARGS NAMES ZLIB
    )
    find_package(ZLIB REQUIRED)
endif()

########################################
# capstone (optional, for JIT disassembly)

if (${ENABLE_DISASSEMBLER})
  cinderx_declare_dependency(
    capstone
    https://github.com/capstone-engine/capstone
    52c66920fc7bfa15fd9626dfd9f646c698aaa99b # tag 5.0.7
  )

  # Skip unsupported architectures while preserving x86's Capstone default,
  # except on ARM64 targets where CinderX only uses ARM64 disassembly.
  set(CAPSTONE_AARCH64_SUPPORT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_ARM64_SUPPORT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_ARM_SUPPORT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_BPF_SUPPORT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_BUILD_CSTOOL OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_EVM_SUPPORT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_M680X_SUPPORT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_M68K_SUPPORT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_MIPS_SUPPORT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_MOS65XX_SUPPORT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_PPC_SUPPORT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_RISCV_SUPPORT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_SH_SUPPORT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_SPARC_SUPPORT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_SYSZ_SUPPORT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_TMS320C64X_SUPPORT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_TRICORE_SUPPORT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_WASM_SUPPORT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_XCORE_SUPPORT OFF CACHE BOOL "" FORCE)

  if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(CAPSTONE_X86_SUPPORT ON CACHE BOOL "" FORCE)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(CAPSTONE_X86_SUPPORT OFF CACHE BOOL "" FORCE)
    set(CAPSTONE_AARCH64_SUPPORT ON CACHE BOOL "" FORCE)
    set(CAPSTONE_ARM64_SUPPORT ON CACHE BOOL "" FORCE)
  endif()

  FetchContent_MakeAvailable(capstone)
  target_compile_options(capstone PRIVATE -w)
  target_compile_options(capstone_static PRIVATE -w)
  # CAPSTONE_STATIC suppresses CAPSTONE_EXPORT's visibility("default") annotation,
  # keeping capstone symbols hidden with the rest of the extension internals.
  target_compile_definitions(capstone PRIVATE CAPSTONE_STATIC)
  target_compile_definitions(capstone_static PRIVATE CAPSTONE_STATIC)
endif()

##############################################################################
# Build cinderx

include_directories(
  ${PROJECT_SOURCE_DIR}/..
  ${GENERATED_HEADER_DIR}
  ${Python_INCLUDE_DIRS}
  ${parallel-hashmap_SOURCE_DIR}
  ${usdt_SOURCE_DIR}/..
)

########################################
# Common/

file(GLOB_RECURSE COMMON_SOURCES ${PROJECT_SOURCE_DIR}/Common/*.cpp ${PROJECT_SOURCE_DIR}/Common/*.c)
add_library(common ${COMMON_SOURCES})
if (${ENABLE_ZLIB})
    target_link_libraries(common PRIVATE asmjit::asmjit fmt::fmt ZLIB::ZLIB)
else()
    target_link_libraries(common PRIVATE asmjit::asmjit fmt::fmt)
endif()

########################################
# UpstreamBorrow/

if (${PY_VERSION} EQUAL 3.11 OR ${PY_VERSION} EQUAL 3.12 OR ${PY_VERSION} EQUAL 3.14 OR ${PY_VERSION} EQUAL 3.15)
  set(BORROWED_C ${PROJECT_SOURCE_DIR}/UpstreamBorrow/borrowed-${PY_VERSION}.gen_cached.c)
else()
  set(BORROWED_C "${GENERATED_HEADER_DIR}/dummy-borrowed.c")
  file(WRITE ${BORROWED_C} "")
endif()

add_library(borrowed ${BORROWED_C})

########################################
# CachedProperties/

file(GLOB_RECURSE CACHED_PROPERTY_SOURCES ${PROJECT_SOURCE_DIR}/CachedProperties/*.c)
add_library(cached-properties ${CACHED_PROPERTY_SOURCES})
target_link_libraries(cached-properties PRIVATE common borrowed)

########################################
# Interpreter/

if (${PY_VERSION} EQUAL 3.11)
  # The 3.11 source set carries no generated opcode headers; the vendored
  # evaluator takes its tables from pycore_opcode.h via NEED_OPCODE_TABLES,
  # so cinder_opcode.c and the header-copy step below do not apply.
  set(INTERP_SOURCES
    ${PROJECT_SOURCE_DIR}/Interpreter/interpreter_base.cpp
    ${PROJECT_SOURCE_DIR}/Interpreter/iter_helpers.c
    ${PROJECT_SOURCE_DIR}/Interpreter/3.11/cinder_opcode_tables.c
  )
  if (${ENABLE_INTERPRETER_LOOP})
    set(INTERP_SOURCES
      ${INTERP_SOURCES}
      ${PROJECT_SOURCE_DIR}/Interpreter/3.11/borrowed-ceval.gen_cached.c
      ${PROJECT_SOURCE_DIR}/Interpreter/3.11/ceval_shims.c
      ${PROJECT_SOURCE_DIR}/Interpreter/3.11/ceval_wrapper.c
      ${PROJECT_SOURCE_DIR}/Interpreter/3.11/eval_hook.c
      ${PROJECT_SOURCE_DIR}/Interpreter/3.11/frame_wrapper.c
      ${PROJECT_SOURCE_DIR}/Interpreter/3.11/interpreter.c
      ${PROJECT_SOURCE_DIR}/Interpreter/3.11/observe.c
      ${PROJECT_SOURCE_DIR}/Interpreter/3.11/specialize_wrapper.c
    )
  endif()
else()
set(INTERP_SOURCES
  ${PROJECT_SOURCE_DIR}/Interpreter/cinder_opcode.c
  ${PROJECT_SOURCE_DIR}/Interpreter/interpreter_base.cpp
  ${PROJECT_SOURCE_DIR}/Interpreter/iter_helpers.c
)
endif()

if (${PY_VERSION} EQUAL 3.12 OR ${PY_VERSION} EQUAL 3.14 OR ${PY_VERSION} EQUAL 3.15)
  set(SOURCE_CINDERX_OPCODE_TARGETS_H ${PROJECT_SOURCE_DIR}/Interpreter/${PY_VERSION}/cinderx_opcode_targets.h)
  set(SOURCE_CINDER_OPCODE_H ${PROJECT_SOURCE_DIR}/Interpreter/${PY_VERSION}/cinder_opcode.h)
  set(SOURCE_CINDER_OPCODE_IDS_H ${PROJECT_SOURCE_DIR}/Interpreter/${PY_VERSION}/cinder_opcode_ids.h)
  set(SOURCE_CINDER_OPCODE_METADATA_H ${PROJECT_SOURCE_DIR}/Interpreter/${PY_VERSION}/cinder_opcode_metadata.h)

  if (${ENABLE_INTERPRETER_LOOP})
    set(INTERP_SOURCES ${INTERP_SOURCES} ${PROJECT_SOURCE_DIR}/Interpreter/${PY_VERSION}/interpreter.c)
  endif()
endif()

# Generate cinderx/Interpreter header files.

if (${PY_VERSION} EQUAL 3.11)
  # 3.11 generates no opcode metadata: shared code gets the target
  # interpreter's own tables through a thin renaming header.
  file(READ ${PROJECT_SOURCE_DIR}/Interpreter/3.11/cinder_opcode.h FILE_CONTENTS)
  file(WRITE ${GENERATED_HEADER_DIR}/cinderx/Interpreter/cinder_opcode.h "${FILE_CONTENTS}")
else()
set(CINDERX_OPCODE_TARGETS_H ${GENERATED_HEADER_DIR}/cinderx/Interpreter/cinderx_opcode_targets.h)
set(CINDER_OPCODE_H ${GENERATED_HEADER_DIR}/cinderx/Interpreter/cinder_opcode.h)
set(CINDER_OPCODE_IDS_H ${GENERATED_HEADER_DIR}/cinderx/Interpreter/cinder_opcode_ids.h)
set(CINDER_OPCODE_METADATA_H ${GENERATED_HEADER_DIR}/cinderx/Interpreter/cinder_opcode_metadata.h)

file(READ ${SOURCE_CINDERX_OPCODE_TARGETS_H} FILE_CONTENTS)
file(WRITE ${CINDERX_OPCODE_TARGETS_H} "${FILE_CONTENTS}")

file(READ ${SOURCE_CINDER_OPCODE_IDS_H} FILE_CONTENTS)
file(WRITE ${CINDER_OPCODE_IDS_H} "${FILE_CONTENTS}")

file(READ ${SOURCE_CINDER_OPCODE_METADATA_H} FILE_CONTENTS)
file(WRITE ${CINDER_OPCODE_METADATA_H} "${FILE_CONTENTS}")

file(READ ${SOURCE_CINDER_OPCODE_H} FILE_CONTENTS)
file(WRITE ${CINDER_OPCODE_H} "${FILE_CONTENTS}")
endif()

add_library(interpreter ${INTERP_SOURCES})
if (NOT ${PY_VERSION} EQUAL 3.11)
target_include_directories(
  interpreter
  PUBLIC
  ${PROJECT_SOURCE_DIR}/Interpreter/${PY_VERSION}/Includes
)
endif()

########################################
# Immortalize/

file(GLOB_RECURSE IMMORTALIZE_SOURCES ${PROJECT_SOURCE_DIR}/Immortalize/*.cpp)
add_library(immortalize ${IMMORTALIZE_SOURCES})
target_link_libraries(immortalize PRIVATE asmjit::asmjit fmt::fmt)

########################################
# StaticPython/

if (${PY_VERSION} EQUAL 3.11)
  # Static Python is out of scope on 3.11; a stub carries the runtime-facing
  # surface (StrictModule minimum plus cache-clear hooks).
  set(STATIC_PYTHON_SOURCES ${PROJECT_SOURCE_DIR}/StaticPython/static_python_stub.cpp)
else()
  file(GLOB_RECURSE STATIC_PYTHON_SOURCES ${PROJECT_SOURCE_DIR}/StaticPython/*.c)
endif()
add_library(static-python ${STATIC_PYTHON_SOURCES})
target_link_libraries(static-python PRIVATE fmt::fmt asmjit::asmjit common borrowed)

########################################
# Jit/

# The full JIT source set compiles on every supported version, 3.11 included.
# On 3.11 the explicit shadow mode initializes only the compiler pipeline;
# generated code is relocated for validation and discarded. Observe/off never
# initialize it, and no 3.11 mode publishes or enters machine code.
file(GLOB_RECURSE JIT_SOURCES ${PROJECT_SOURCE_DIR}/Jit/*.cpp ${PROJECT_SOURCE_DIR}/Jit/*.c)
if (${PY_VERSION} EQUAL 3.11)
  # OSR is out of scope on 3.11: Jit/osr.cpp is 3.14 machine-code machinery
  # (reads _PyStackRef and internal/pycore_interpframe.h).  It is excluded and
  # Jit/osr_stub_311.cpp supplies inert OSR symbols in its place.
  list(REMOVE_ITEM JIT_SOURCES ${PROJECT_SOURCE_DIR}/Jit/osr.cpp)
endif()

add_library(jit ${JIT_SOURCES})
target_compile_options(jit PRIVATE -Wno-free-nonheap-object)
target_link_libraries(jit PRIVATE asmjit::asmjit interpreter fmt::fmt)
if (${ENABLE_DISASSEMBLER})
  target_link_libraries(jit PRIVATE capstone_static)
endif()

########################################
# ParallelGC/

if (${ENABLE_PARALLEL_GC})
  file(GLOB_RECURSE PARALLEL_GC_SOURCES ${PROJECT_SOURCE_DIR}/ParallelGC/*.c)
else()
  set(PARALLEL_GC_SOURCES "${GENERATED_HEADER_DIR}/dummy-parallel-gc.c")
  file(WRITE ${PARALLEL_GC_SOURCES} "")
endif()
add_library(parallel-gc ${PARALLEL_GC_SOURCES})

########################################
# _cinderx.cpp

set(CINDERX_LIB_SOURCES
  ${PROJECT_SOURCE_DIR}/_cinderx-lib.cpp
  ${PROJECT_SOURCE_DIR}/async_lazy_value.cpp
  ${PROJECT_SOURCE_DIR}/module_state.cpp
  ${PROJECT_SOURCE_DIR}/module_c_state.cpp
  ${PROJECT_SOURCE_DIR}/python_runtime.cpp
)

set(SOURCES
  # Manually listed out to not accidentally include stuff in .git.
  ${PROJECT_SOURCE_DIR}/_cinderx.cpp
  ${CINDERX_LIB_SOURCES}
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE
  Python::Module
  borrowed cached-properties common immortalize interpreter jit parallel-gc static-python
  asmjit::asmjit fmt::fmt)

# macOS doesn't allow depending on other shared libraries by default.
if (${MACOS})
  target_link_options(${PROJECT_NAME} PRIVATE -undefined dynamic_lookup)
endif()

# Bind intra-DSO function calls at link time instead of routing them through
# the PLT. The JIT runtime helpers (inline caches, frame setup, JITRT_*) call
# each other on every attribute access / function call from JIT-compiled code,
# and the PLT indirection shows up prominently in profiles (~9% of cycles on
# call-heavy workloads like sqlalchemy_imperative). Nothing interposes
# _cinderx.so's internal symbols, so this is safe.
if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  target_link_options(${PROJECT_NAME} PRIVATE -Wl,-Bsymbolic-functions)
endif()

# The 3.11 build links a vendored interpreter whose PyAPI_FUNC definitions
# carry default visibility; export nothing but the module init so they can
# never shadow libpython's own under RTLD_GLOBAL loading.
if (DEFINED PY_VERSION AND ${PY_VERSION} EQUAL 3.11 AND ${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  target_link_options(${PROJECT_NAME} PRIVATE
    -Wl,--version-script=${PROJECT_SOURCE_DIR}/_cinderx.version)
  set_target_properties(${PROJECT_NAME} PROPERTIES
    LINK_DEPENDS ${PROJECT_SOURCE_DIR}/_cinderx.version)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX "")
if (WINDOWS)
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "_cinderx.pyd")
else()
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "_cinderx.so")
endif()

########################################
# RuntimeTests/

if(ENABLE_RUNTIME_TESTS)
  enable_testing()
  add_subdirectory(${PROJECT_SOURCE_DIR}/RuntimeTests)
endif()
