ladybird_option(BUILD_SHARED_LIBS ON CACHE BOOL "Build shared libraries instead of static libraries")

ladybird_option(ENABLE_COMPILETIME_FORMAT_CHECK ON CACHE BOOL "Enable compiletime format string checks")
ladybird_option(ENABLE_UNDEFINED_SANITIZER OFF CACHE BOOL "Enable undefined behavior sanitizer testing in gcc/clang")
ladybird_option(UNDEFINED_BEHAVIOR_IS_FATAL OFF CACHE BOOL "Make undefined behavior sanitizer errors non-recoverable")

ladybird_option(ENABLE_ALL_THE_DEBUG_MACROS OFF CACHE BOOL "Enable all debug macros to validate they still compile")

ladybird_option(INCLUDE_WASM_SPEC_TESTS OFF CACHE BOOL "Download and include the WebAssembly spec testsuite")

ladybird_option(LADYBIRD_CACHE_DIR "${PROJECT_BINARY_DIR}/../caches" CACHE PATH "Location of shared cache of downloaded files")
ladybird_option(ENABLE_NETWORK_DOWNLOADS ON CACHE BOOL "Allow downloads of required files. If OFF, required files must already be present in LADYBIRD_CACHE_DIR")

ladybird_option(ENABLE_CLANG_PLUGINS OFF CACHE BOOL "Enable building with the Clang plugins")
ladybird_option(ENABLE_CLANG_PLUGINS_INVALID_FUNCTION_MEMBERS OFF CACHE BOOL "Enable detecting invalid function types as members of GC-allocated objects")

if ((LINUX AND NOT ANDROID) OR BSD)
    set(freedesktop_files_default ON)
else()
    set(freedesktop_files_default OFF)
endif()

ladybird_option(ENABLE_GUI_TARGETS ON CACHE BOOL "Enable building GUI targets")
ladybird_option(ENABLE_INSTALL_HEADERS ON CACHE BOOL "Enable installing headers")
ladybird_option(ENABLE_INSTALL_FREEDESKTOP_FILES ${freedesktop_files_default} CACHE BOOL "Enable installing .desktop and .service files")
ladybird_option(LADYBIRD_ENABLE_CPPTRACE ON CACHE BOOL "Enable use of cpptrace as the default library for stacktraces. If not available falls back to backtrace.h")
ladybird_option(LADYBIRD_GENERATE_DSYM OFF CACHE BOOL "Generate dSYM bundles for binaries and libraries (macOS only)")
ladybird_option(ENABLE_CI_BASELINE_CPU OFF CACHE BOOL "Use a baseline CPU target for improved ccache sharing")

# lto1 uses a crazy amount of RAM in static builds.
# Disable LTO for static gcc builds unless explicitly asked for.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT BUILD_SHARED_LIBS)
    set(RELEASE_LTO_DEFAULT OFF)
else()
    set(RELEASE_LTO_DEFAULT ON)
endif()

ladybird_option(ENABLE_ADDRESS_SANITIZER OFF CACHE BOOL "Enable address sanitizer testing in gcc/clang")
ladybird_option(ENABLE_MEMORY_SANITIZER OFF CACHE BOOL "Enable memory sanitizer testing in gcc/clang")
ladybird_option(ENABLE_FUZZERS OFF CACHE BOOL "Build fuzzing targets")
ladybird_option(ENABLE_FUZZERS_LIBFUZZER OFF CACHE BOOL "Build fuzzers using Clang's libFuzzer")
ladybird_option(ENABLE_FUZZERS_OSSFUZZ OFF CACHE BOOL "Build OSS-Fuzz compatible fuzzers")
ladybird_option(LAGOM_TOOLS_ONLY OFF CACHE BOOL "Don't build libraries, utilities and tests, only host build tools")
ladybird_option(ENABLE_LAGOM_CCACHE ON CACHE BOOL "Enable ccache for Lagom builds")
ladybird_option(LAGOM_USE_LINKER "" CACHE STRING "The linker to use (e.g. lld, mold) instead of the system default")
ladybird_option(LAGOM_LINK_POOL_SIZE "" CACHE STRING "The maximum number of parallel jobs to use for linking")
ladybird_option(ENABLE_LTO_FOR_RELEASE ${RELEASE_LTO_DEFAULT} CACHE BOOL "Enable link-time optimization for release builds")
ladybird_option(ENABLE_LAGOM_COVERAGE_COLLECTION OFF CACHE STRING "Enable code coverage instrumentation for lagom binaries in clang")

ladybird_option(ENABLE_RUST ON CACHE BOOL "Build Rust components")

if (ENABLE_FUZZERS_LIBFUZZER)
    # With libfuzzer, we need to avoid a duplicate main() linker error giving false negatives
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY CACHE STRING "Type of target to use for try_compile()" FORCE)
endif()

include(CheckCXXSourceCompiles)
set(BLOCKS_REQUIRED_LIBRARIES "")
if (NOT APPLE)
    find_package(BlocksRuntime)
    if (BlocksRuntime_FOUND)
        set(BLOCKS_REQUIRED_LIBRARIES BlocksRuntime::BlocksRuntime)
        set(CMAKE_REQUIRED_LIBRARIES BlocksRuntime::BlocksRuntime)
    endif()
endif()
check_cxx_source_compiles([=[
    int main() { __block int x = 0; auto b = ^{++x;}; b(); }
]=] CXX_COMPILER_SUPPORTS_BLOCKS)

set(CMAKE_REQUIRED_FLAGS "-fobjc-arc")
check_cxx_source_compiles([=[
    int main() { auto b = ^{}; auto __weak w = b; w(); }
]=] CXX_COMPILER_SUPPORTS_OBJC_ARC)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LIBRARIES)

include(${CMAKE_CURRENT_LIST_DIR}/lagom_install_options.cmake)
