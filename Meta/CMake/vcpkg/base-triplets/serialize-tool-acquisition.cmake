# NOTE: This must not set any variables in the including scope, as the triplet's variables are part of every port's ABI.

if(NOT COMMAND vcpkg_find_acquire_program OR NOT COMMAND vcpkg_extract_archive)
    return()
endif()

if(COMMAND z_ladybird_tool_acquisition_is_serialized)
    return()
endif()
function(z_ladybird_tool_acquisition_is_serialized)
endfunction()

function(vcpkg_find_acquire_program program)
    # Acquiring some tools acquires others (e.g. meson needs python), and the lock is not reentrant.
    get_property(depth GLOBAL PROPERTY Z_LADYBIRD_TOOL_ACQUISITION_DEPTH)
    if("${depth}" STREQUAL "")
        set(depth 0)
    endif()
    if(depth EQUAL 0)
        file(MAKE_DIRECTORY "${DOWNLOADS}/tools")
        file(LOCK "${DOWNLOADS}/tools/.ladybird-acquire.lock" GUARD FUNCTION)
    endif()

    math(EXPR inner_depth "${depth} + 1")
    set_property(GLOBAL PROPERTY Z_LADYBIRD_TOOL_ACQUISITION_DEPTH "${inner_depth}")
    _vcpkg_find_acquire_program(${ARGV})
    set_property(GLOBAL PROPERTY Z_LADYBIRD_TOOL_ACQUISITION_DEPTH "${depth}")

    set("${program}" "${${program}}" PARENT_SCOPE)
endfunction()

function(vcpkg_extract_archive)
    get_property(depth GLOBAL PROPERTY Z_LADYBIRD_TOOL_ACQUISITION_DEPTH)
    if("${depth}" STREQUAL "" OR depth EQUAL 0)
        _vcpkg_extract_archive(${ARGV})
        return()
    endif()

    cmake_parse_arguments(PARSE_ARGV 0 arg "" "ARCHIVE;DESTINATION" "")

    # vcpkg only extracts a tool once it failed to find it, and we hold the lock, so anything already here is the
    # remains of an extraction (or post-install step) that never finished.
    if(EXISTS "${arg_DESTINATION}")
        message(STATUS "Removing incomplete tool directory ${arg_DESTINATION}")
        file(REMOVE_RECURSE "${arg_DESTINATION}")
    endif()

    set(partial_destination "${arg_DESTINATION}.partial")
    file(REMOVE_RECURSE "${partial_destination}")
    _vcpkg_extract_archive(ARCHIVE "${arg_ARCHIVE}" DESTINATION "${partial_destination}")
    file(RENAME "${partial_destination}" "${arg_DESTINATION}")
endfunction()
