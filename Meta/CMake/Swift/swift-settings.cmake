enable_language(Swift)

if (CMAKE_Swift_COMPILER_VERSION VERSION_LESS 6.0)
    message(FATAL_ERROR
        "Swift 6.0 or newer is required to parse C++ headers in C++23 mode"
    )
endif()

# FIXME: How to verify this on non-Apple?
if (APPLE AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    message(FATAL_ERROR
        "Swift files must use Clang that was bundled with swiftc"
    )
endif()

include(${CMAKE_CURRENT_LIST_DIR}/InitializeSwift.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/GenerateSwiftHeader.cmake)

# FIXME: https://gitlab.kitware.com/cmake/cmake/-/issues/26174
if (APPLE)
    set(CMAKE_Swift_COMPILER_TARGET "${SWIFT_TARGET_TRIPLE}")
endif()

add_compile_options("SHELL:$<$<COMPILE_LANGUAGE:Swift>:-enable-experimental-feature Extern>")

set(VFS_OVERLAY_DIRECTORY "${CMAKE_BINARY_DIR}/vfs_overlays" CACHE PATH "Directory to put VFS overlays in")

# FIXME: https://gitlab.kitware.com/cmake/cmake/-/issues/26195
# For now, we'll just manually massage the flags.
function(swizzle_target_properties_for_swift target_name)
    get_property(compile_options TARGET ${target_name} PROPERTY INTERFACE_COMPILE_OPTIONS)
    set(munged_properties "")
    foreach(property IN LISTS compile_options)
        set(cxx_property "$<$<COMPILE_LANGUAGE:C,CXX,ASM>:${property}>")
        set(swift_property "SHELL:$<$<COMPILE_LANGUAGE:Swift>:-Xcc ${property}>")
        list(APPEND munged_properties "${cxx_property}" "${swift_property}")
    endforeach()
    set_property(TARGET ${target_name} PROPERTY INTERFACE_COMPILE_OPTIONS ${munged_properties})
endfunction()

function(add_swift_target_properties target_name)
    cmake_parse_arguments(PARSE_ARGV 1 SWIFT_TARGET "" "" "LAGOM_LIBRARIES")

    target_compile_features(${target_name} PUBLIC cxx_std_${CMAKE_CXX_STANDARD})
    target_compile_options(${target_name} PUBLIC "SHELL:$<$<COMPILE_LANGUAGE:Swift>:-Xcc -std=c++23 -cxx-interoperability-mode=default>")

    string(REPLACE "Lib" "" module_name ${target_name})

    string(TOUPPER ${target_name} TARGET_NAME_UPPER)
    target_compile_definitions(${target_name} PRIVATE "${TARGET_NAME_UPPER}_USE_SWIFT")
    set_target_properties(${target_name} PROPERTIES Swift_MODULE_NAME ${module_name})

    # FIXME: These should be pulled automatically from interface compile options for the target
    set(VFS_OVERLAY_OPTIONS "-Xcc" "-ivfsoverlay${VFS_OVERLAY_DIRECTORY}/${target_name}_vfs_overlay.yaml")
    foreach(internal_library IN LISTS SWIFT_TARGET_LAGOM_LIBRARIES)
        list(APPEND VFS_OVERLAY_OPTIONS "-Xcc" "-ivfsoverlay${VFS_OVERLAY_DIRECTORY}/${internal_library}_vfs_overlay.yaml")
    endforeach()

    get_target_property(_NATIVE_DIRS ${target_name} INCLUDE_DIRECTORIES)
    list(APPEND _NATIVE_DIRS ${CMAKE_Swift_MODULE_DIRECTORY})

    _swift_generate_cxx_header(${target_name} "${target_name}-Swift.h"
        SEARCH_PATHS ${_NATIVE_DIRS}
        COMPILE_OPTIONS ${VFS_OVERLAY_OPTIONS}
    )
endfunction()
