# Make relative paths in depfiles be relative to CMAKE_CURRENT_BINARY_DIR rather than to CMAKE_BINARY_DIR
if (POLICY CMP0116)
    cmake_policy(SET CMP0116 NEW)
endif()

# Enable better flags for configuring swift compilation mode
if (POLICY CMP0157)
   cmake_policy(SET CMP0157 NEW)
   set(CMAKE_Swift_COMPILATION_MODE "$<IF:$<CONFIG:Release>,wholemodule,incremental>")
endif()

# Check arguments to return()
if (POLICY CMP0140)
    cmake_policy(SET CMP0140 NEW)
endif()

ladybird_option(BUILD_SHARED_LIBS ON CACHE BOOL "Build shared libraries instead of static libraries")

ladybird_option(ENABLE_COMPILETIME_FORMAT_CHECK ON CACHE BOOL "Enable compiletime format string checks")
ladybird_option(ENABLE_UNDEFINED_SANITIZER OFF CACHE BOOL "Enable undefined behavior sanitizer testing in gcc/clang")
ladybird_option(UNDEFINED_BEHAVIOR_IS_FATAL OFF CACHE BOOL "Make undefined behavior sanitizer errors non-recoverable")

ladybird_option(ENABLE_ALL_THE_DEBUG_MACROS OFF CACHE BOOL "Enable all debug macros to validate they still compile")
ladybird_option(ENABLE_ALL_DEBUG_FACILITIES OFF CACHE BOOL "Enable all noisy debug symbols and options. Not recommended for normal developer use")
ladybird_option(ENABLE_COMPILETIME_HEADER_CHECK OFF CACHE BOOL "Enable compiletime check that each library header compiles stand-alone")

ladybird_option(INCLUDE_WASM_SPEC_TESTS OFF CACHE BOOL "Download and include the WebAssembly spec testsuite")

ladybird_option(LADYBIRD_CACHE_DIR "${PROJECT_BINARY_DIR}/../caches" CACHE PATH "Location of shared cache of downloaded files")
ladybird_option(ENABLE_NETWORK_DOWNLOADS ON CACHE BOOL "Allow downloads of required files. If OFF, required files must already be present in LADYBIRD_CACHE_DIR")

ladybird_option(ENABLE_CLANG_PLUGINS OFF CACHE BOOL "Enable building with the Clang plugins")
ladybird_option(ENABLE_CLANG_PLUGINS_INVALID_FUNCTION_MEMBERS OFF CACHE BOOL "Enable detecting invalid function types as members of GC-allocated objects")

if (LINUX AND NOT ANDROID)
    set(freedesktop_files_default ON)
else()
    set(freedesktop_files_default OFF)
endif()

ladybird_option(ENABLE_GUI_TARGETS ON CACHE BOOL "Enable building GUI targets")
ladybird_option(ENABLE_INSTALL_HEADERS ON CACHE BOOL "Enable installing headers")
ladybird_option(ENABLE_INSTALL_FREEDESKTOP_FILES ${freedesktop_files_default} CACHE BOOL "Enable installing .desktop and .service files")
ladybird_option(ENABLE_SWIFT OFF CACHE BOOL "Enable building Swift files")
ladybird_option(ENABLE_STD_STACKTRACE OFF CACHE BOOL "Force use of std::stacktrace instead of libbacktrace. If it is not supported the build will fail")
ladybird_option(ENABLE_WINDOWS_CI OFF CACHE BOOL "Enable building targets supported on Windows for CI")
ladybird_option(ENABLE_CI_BASELINE_CPU OFF CACHE BOOL "Use a baseline CPU target for improved ccache sharing")

if (ENABLE_FUZZERS_LIBFUZZER)
    # With libfuzzer, we need to avoid a duplicate main() linker error giving false negatives
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY CACHE STRING "Type of target to use for try_compile()" FORCE)
endif()

if (ENABLE_SWIFT)
    include(${CMAKE_CURRENT_LIST_DIR}/Swift/swift-settings.cmake)
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
