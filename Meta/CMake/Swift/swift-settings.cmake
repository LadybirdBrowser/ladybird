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

add_compile_options("SHELL:$<$<COMPILE_LANGUAGE:Swift>:-Xcc -std=c++23 -cxx-interoperability-mode=default>")

# FIXME: https://gitlab.kitware.com/cmake/cmake/-/issues/26174
if (APPLE)
    set(CMAKE_Swift_COMPILER_TARGET "${CMAKE_SYSTEM_PROCESSOR}-apple-macosx${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

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
