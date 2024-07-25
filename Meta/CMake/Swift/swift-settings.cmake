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
