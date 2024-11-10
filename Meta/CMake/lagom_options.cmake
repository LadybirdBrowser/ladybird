#
# Options specific to the Lagom (host) build
#

include(${CMAKE_CURRENT_LIST_DIR}/common_options.cmake NO_POLICY_SCOPE)
include(${CMAKE_CURRENT_LIST_DIR}/lagom_install_options.cmake)

serenity_option(ENABLE_ADDRESS_SANITIZER OFF CACHE BOOL "Enable address sanitizer testing in gcc/clang")
serenity_option(ENABLE_MEMORY_SANITIZER OFF CACHE BOOL "Enable memory sanitizer testing in gcc/clang")
serenity_option(ENABLE_FUZZERS OFF CACHE BOOL "Build fuzzing targets")
serenity_option(ENABLE_FUZZERS_LIBFUZZER OFF CACHE BOOL "Build fuzzers using Clang's libFuzzer")
serenity_option(ENABLE_FUZZERS_OSSFUZZ OFF CACHE BOOL "Build OSS-Fuzz compatible fuzzers")
serenity_option(LAGOM_TOOLS_ONLY OFF CACHE BOOL "Don't build libraries, utilities and tests, only host build tools")
serenity_option(ENABLE_LAGOM_CCACHE ON CACHE BOOL "Enable ccache for Lagom builds")
serenity_option(LAGOM_USE_LINKER "" CACHE STRING "The linker to use (e.g. lld, mold) instead of the system default")
serenity_option(ENABLE_LAGOM_COVERAGE_COLLECTION OFF CACHE STRING "Enable code coverage instrumentation for lagom binaries in clang")

if (ANDROID OR APPLE)
    serenity_option(ENABLE_QT OFF CACHE BOOL "Build ladybird application using Qt GUI")
else()
    serenity_option(ENABLE_QT ON CACHE BOOL "Build ladybird application using Qt GUI")
endif()
