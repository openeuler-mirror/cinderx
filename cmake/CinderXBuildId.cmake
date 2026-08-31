# Copyright (c) Meta Platforms, Inc. and affiliates.

function(cinderx_resolve_core_build_id SOURCE_DIR EXPLICIT_BUILD_ID OUT_VAR)
  set(build_id "${EXPLICIT_BUILD_ID}")

  if("${build_id}" STREQUAL "")
    find_package(Git QUIET)
    if(GIT_FOUND)
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse --show-toplevel
        RESULT_VARIABLE git_top_level_result
        OUTPUT_VARIABLE git_top_level
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
      if(git_top_level_result EQUAL 0 AND NOT "${git_top_level}" STREQUAL "")
        get_filename_component(source_dir_real "${SOURCE_DIR}" REALPATH)
        get_filename_component(git_top_level_real "${git_top_level}" REALPATH)
        if("${git_top_level_real}" STREQUAL "${source_dir_real}")
          execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse --verify HEAD
            RESULT_VARIABLE git_sha_result
            OUTPUT_VARIABLE git_sha
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
          if(git_sha_result EQUAL 0 AND NOT "${git_sha}" STREQUAL "")
            set(build_id "${git_sha}")
          endif()
        endif()
      endif()
    endif()
  endif()

  if("${build_id}" STREQUAL "")
    set(pkg_info "${SOURCE_DIR}/PKG-INFO")
    if(EXISTS "${pkg_info}")
      file(STRINGS "${pkg_info}" version_lines LIMIT_COUNT 1 REGEX "^Version:")
      if(version_lines)
        list(GET version_lines 0 package_version)
        string(REGEX REPLACE "^Version:[ \t]*" "" package_version "${package_version}")
        string(STRIP "${package_version}" package_version)
        if(NOT "${package_version}" STREQUAL "")
          set(build_id "sdist-${package_version}")
        endif()
      endif()
    endif()
  endif()

  if("${build_id}" STREQUAL "")
    message(FATAL_ERROR
      "Cannot determine a trustworthy CINDERX_CORE_BUILD_ID: provide an explicit "
      "value, configure from the root of a Git checkout, or build from an sdist "
      "with a Version field in PKG-INFO")
  endif()

  string(LENGTH "${build_id}" build_id_length)
  if(
    build_id_length LESS 1
    OR build_id_length GREATER 128
    OR NOT build_id MATCHES "^[A-Za-z0-9._+-]+$")
    message(FATAL_ERROR
      "CINDERX_CORE_BUILD_ID must be 1-128 characters from [A-Za-z0-9._+-]")
  endif()

  set("${OUT_VAR}" "${build_id}" PARENT_SCOPE)
endfunction()
