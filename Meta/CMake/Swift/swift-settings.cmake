enable_language(Swift)

if (CMAKE_Swift_COMPILER_VERSION VERSION_LESS 6.0)
    message(FATAL_ERROR "Swift 6.0 or newer is required to parse C++ headers in C++23 mode")
endif()

# Check for a Swift-aware clang
include(CheckCXXSourceCompiles)
check_cxx_source_compiles([=[
    struct __attribute__((swift_name("CxxS"))) __attribute__((swift_attr("~Copyable"))) S { int& x; };
    int main() {}
]=] CXX_COMPILER_SUPPORTS_SWIFT_ATTRS)
if (NOT CXX_COMPILER_SUPPORTS_SWIFT_ATTRS)
    message(FATAL_ERROR "A Swift-aware C++ compiler is required to build with Swift interop enabled")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/InitializeSwift.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/GenerateSwiftHeader.cmake)

find_package(SwiftTesting REQUIRED)

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
    cmake_parse_arguments(PARSE_ARGV 1 SWIFT_TARGET "" "" "LAGOM_LIBRARIES;COMPILE_DEFINITIONS;COMPILE_OPTIONS")

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

    # Swift-testing in swift.org toolchains on macOS has its .swiftmodule in a testing/ subdirectory of
    # the swift compiler's built-in lib dirs.
    get_target_property(DEPENDENCIES ${target_name} LINK_LIBRARIES)
    if (SwiftTesting::SwiftTesting IN_LIST DEPENDENCIES)
        get_target_property(SWIFT_TESTING_INCLUDE_DIRS SwiftTesting::SwiftTesting INTERFACE_INCLUDE_DIRECTORIES)
        list(APPEND _NATIVE_DIRS ${SWIFT_TESTING_INCLUDE_DIRS})
    endif()

    set(EXTRA_COMPILE_DEFINITIONS "")
    foreach (compile_definition IN LISTS SWIFT_TARGET_COMPILE_DEFINITIONS)
        list(APPEND EXTRA_COMPILE_DEFINITIONS "-Xcc" "-D${compile_definition}")
    endforeach()

    _swift_generate_cxx_header(${target_name} "${target_name}-Swift.h"
        SEARCH_PATHS ${_NATIVE_DIRS}
        COMPILE_OPTIONS ${VFS_OVERLAY_OPTIONS} ${EXTRA_COMPILE_DEFINITIONS} ${SWIFT_TARGET_COMPILE_OPTIONS}
    )
endfunction()
