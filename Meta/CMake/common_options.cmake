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

serenity_option(ENABLE_COMPILETIME_FORMAT_CHECK ON CACHE BOOL "Enable compiletime format string checks")
serenity_option(ENABLE_UNDEFINED_SANITIZER OFF CACHE BOOL "Enable undefined behavior sanitizer testing in gcc/clang")
serenity_option(UNDEFINED_BEHAVIOR_IS_FATAL OFF CACHE BOOL "Make undefined behavior sanitizer errors non-recoverable")

serenity_option(ENABLE_ALL_THE_DEBUG_MACROS OFF CACHE BOOL "Enable all debug macros to validate they still compile")
serenity_option(ENABLE_ALL_DEBUG_FACILITIES OFF CACHE BOOL "Enable all noisy debug symbols and options. Not recommended for normal developer use")
serenity_option(ENABLE_COMPILETIME_HEADER_CHECK OFF CACHE BOOL "Enable compiletime check that each library header compiles stand-alone")

serenity_option(ENABLE_PUBLIC_SUFFIX_DOWNLOAD ON CACHE BOOL "Enable download of the Public Suffix List at build time")
serenity_option(INCLUDE_WASM_SPEC_TESTS OFF CACHE BOOL "Download and include the WebAssembly spec testsuite")
serenity_option(INCLUDE_FLAC_SPEC_TESTS OFF CACHE BOOL "Download and include the FLAC spec testsuite")
serenity_option(ENABLE_CACERT_DOWNLOAD ON CACHE BOOL "Enable download of cacert.pem at build time")

serenity_option(SERENITY_CACHE_DIR "${PROJECT_BINARY_DIR}/../caches" CACHE PATH "Location of shared cache of downloaded files")
serenity_option(ENABLE_NETWORK_DOWNLOADS ON CACHE BOOL "Allow downloads of required files. If OFF, required files must already be present in SERENITY_CACHE_DIR")

serenity_option(ENABLE_CLANG_PLUGINS OFF CACHE BOOL "Enable building with the Clang plugins")
serenity_option(ENABLE_CLANG_PLUGINS_INVALID_FUNCTION_MEMBERS OFF CACHE BOOL "Enable detecting invalid function types as members of GC-allocated objects")

serenity_option(ENABLE_GUI_TARGETS ON CACHE BOOL "Enable building GUI targets")
serenity_option(ENABLE_INSTALL_HEADERS ON CACHE BOOL "Enable installing headers")
serenity_option(ENABLE_SWIFT OFF CACHE BOOL "Enable building Swift files")
serenity_option(ENABLE_STD_STACKTRACE OFF CACHE BOOL "Force use of std::stacktrace instead of libbacktrace. If it is not supported the build will fail")

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
