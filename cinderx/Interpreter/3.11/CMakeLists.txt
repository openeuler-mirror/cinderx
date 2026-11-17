# Copyright (c) Meta Platforms, Inc. and affiliates.

cmake_minimum_required(VERSION 3.18)
project(cinderx_cpython311_interpreter LANGUAGES C)

# TODO(cp311-integration): Delete this standalone build file after the
# CPython 3.11 interpreter is fully integrated into CinderX's top-level build.

find_package(
  Python 3.11.6 EXACT
  COMPONENTS Interpreter Development.Module
  REQUIRED)

set(REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(GENERATED_HEADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
include(${CMAKE_CURRENT_LIST_DIR}/coroutine_call.cmake)
cinderx_generate_coroutine_ceval_311("${GENERATED_HEADER_DIR}")

add_library(
  cinderx_cpython311_interpreter
  STATIC
  borrowed-ceval.gen_cached.c
  ceval_shims.c
  ceval_wrapper.c
  frame_wrapper.c
  specialize_wrapper.c)
set_target_properties(
  cinderx_cpython311_interpreter
  PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(
  cinderx_cpython311_interpreter
  PRIVATE
  "${REPOSITORY_ROOT}"
  "${GENERATED_HEADER_DIR}"
  "${CMAKE_CURRENT_LIST_DIR}/upstream"
  "${Python_INCLUDE_DIRS}"
  "${Python_INCLUDE_DIRS}/internal")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(
    cinderx_cpython311_interpreter
    PRIVATE
    -fvisibility=hidden
    -Wall
    -Wextra
    -Wno-cast-function-type
    -Wno-deprecated-declarations
    -Wno-missing-field-initializers
    -Wno-sign-compare
    -Wno-unknown-pragmas
    -Wno-unused-function
    -Wno-unused-parameter)
endif()
