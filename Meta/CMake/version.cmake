if (DEFINED CMAKE_SCRIPT_MODE_FILE)
    set(_source_dir "${SOURCE_DIR}")
    set(_template "${TEMPLATE}")
    set(_output "${OUTPUT}")
else()
    set(_source_dir "${LADYBIRD_SOURCE_DIR}")
    set(_template "${CMAKE_CURRENT_LIST_DIR}/Version.h.in")
    set(_output "${CMAKE_BINARY_DIR}/GeneratedVersion.h")
endif()

find_package(Git QUIET)

set(CURRENT_COMMIT_SHA "")
if (Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${_source_dir}"
        OUTPUT_VARIABLE CURRENT_COMMIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _git_rev_parse_result
    )
    if (NOT _git_rev_parse_result EQUAL 0)
        set(CURRENT_COMMIT_SHA "")
    endif()
endif()

configure_file("${_template}" "${_output}" @ONLY)

if (NOT DEFINED CMAKE_SCRIPT_MODE_FILE)
    add_custom_target(generate_version_header ALL
        BYPRODUCTS "${CMAKE_BINARY_DIR}/GeneratedVersion.h"
        COMMAND ${CMAKE_COMMAND}
            -D "SOURCE_DIR=${LADYBIRD_SOURCE_DIR}"
            -D "TEMPLATE=${CMAKE_CURRENT_LIST_DIR}/Version.h.in"
            -D "OUTPUT=${CMAKE_BINARY_DIR}/GeneratedVersion.h"
            -P "${CMAKE_CURRENT_LIST_FILE}"
        COMMENT "Re-checking current commit SHA..."
        VERBATIM
    )
endif()
