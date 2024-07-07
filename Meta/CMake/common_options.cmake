#
# Options common for the Serenity (target) and Lagom (host) builds
#

# Make relative paths in depfiles be relative to CMAKE_CURRENT_BINARY_DIR rather than to CMAKE_BINARY_DIR
if (POLICY CMP0116)
    cmake_policy(SET CMP0116 NEW)
endif()

serenity_option(ENABLE_COMPILETIME_FORMAT_CHECK ON CACHE BOOL "Enable compiletime format string checks")
serenity_option(ENABLE_UNDEFINED_SANITIZER OFF CACHE BOOL "Enable undefined behavior sanitizer testing in gcc/clang")
serenity_option(UNDEFINED_BEHAVIOR_IS_FATAL OFF CACHE BOOL "Make undefined behavior sanitizer errors non-recoverable")

serenity_option(ENABLE_ALL_THE_DEBUG_MACROS OFF CACHE BOOL "Enable all debug macros to validate they still compile")
serenity_option(ENABLE_ALL_DEBUG_FACILITIES OFF CACHE BOOL "Enable all noisy debug symbols and options. Not recommended for normal developer use")
serenity_option(ENABLE_ADOBE_ICC_PROFILES_DOWNLOAD ON CACHE BOOL "Enable download of Adobe's ICC profiles")
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
