# execute_process runs at configure time. Patch edits must trigger a new
# configure even when the caller only invokes cmake --build.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/0001-specialize-getattr-only-types.patch"
  "${CMAKE_CURRENT_LIST_DIR}/0002-load-attr-with-hint-split-dict.patch"
  "${CMAKE_CURRENT_LIST_DIR}/0003-global-name-cache.patch"
  "${CMAKE_CURRENT_LIST_DIR}/0004-jit-off-inline-call.patch")

find_package(Git REQUIRED)
set(ATTR311_DIR "${CMAKE_CURRENT_BINARY_DIR}/ci311-attr")
file(MAKE_DIRECTORY "${ATTR311_DIR}")
file(SHA256 "${PROJECT_SOURCE_DIR}/Interpreter/3.11/upstream/ceval.c" INPUT_HASH)
if(NOT INPUT_HASH STREQUAL "10723274040fd4da5ac82365712ae91462286faff6421ffcf32fa5fba7ab8048")
  message(FATAL_ERROR "Attribute patch upstream hash mismatch: ceval.c")
endif()
configure_file("${PROJECT_SOURCE_DIR}/Interpreter/3.11/upstream/ceval.c" "${ATTR311_DIR}/ceval.c" COPYONLY)
file(SHA256 "${PROJECT_SOURCE_DIR}/Interpreter/3.11/upstream/specialize.c" INPUT_HASH)
if(NOT INPUT_HASH STREQUAL "0b8dadd2bf0b257eed7ba827d6399d4a18bdd40d47a88cd601e4c6cadfe597dc")
  message(FATAL_ERROR "Attribute patch upstream hash mismatch: specialize.c")
endif()
configure_file("${PROJECT_SOURCE_DIR}/Interpreter/3.11/upstream/specialize.c" "${ATTR311_DIR}/specialize.c" COPYONLY)
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --unsafe-paths "${PROJECT_SOURCE_DIR}/Interpreter/3.11/attr-patches/0001-specialize-getattr-only-types.patch" WORKING_DIRECTORY "${ATTR311_DIR}" RESULT_VARIABLE PATCH_RESULT)
if(NOT PATCH_RESULT EQUAL 0)
  message(FATAL_ERROR "Attribute patch failed: 0001-specialize-getattr-only-types.patch")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --unsafe-paths "${PROJECT_SOURCE_DIR}/Interpreter/3.11/attr-patches/0002-load-attr-with-hint-split-dict.patch" WORKING_DIRECTORY "${ATTR311_DIR}" RESULT_VARIABLE PATCH_RESULT)
if(NOT PATCH_RESULT EQUAL 0)
  message(FATAL_ERROR "Attribute patch failed: 0002-load-attr-with-hint-split-dict.patch")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --unsafe-paths "${PROJECT_SOURCE_DIR}/Interpreter/3.11/attr-patches/0003-global-name-cache.patch" WORKING_DIRECTORY "${ATTR311_DIR}" RESULT_VARIABLE PATCH_RESULT)
if(NOT PATCH_RESULT EQUAL 0)
 message(FATAL_ERROR "Global-name cache patch failed")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --unsafe-paths "${PROJECT_SOURCE_DIR}/Interpreter/3.11/attr-patches/0004-jit-off-inline-call.patch" WORKING_DIRECTORY "${ATTR311_DIR}" RESULT_VARIABLE PATCH_RESULT)
if(NOT PATCH_RESULT EQUAL 0)
 message(FATAL_ERROR "JIT-off inline-call patch failed")
endif()
