option(BUILD_SHARED_LIBS "Build shared libraries instead of static libraries" ON)

option(ENABLE_COMPILETIME_FORMAT_CHECK "Enable compiletime format string checks" ON)
option(ENABLE_UNDEFINED_SANITIZER "Enable undefined behavior sanitizer testing in gcc/clang" OFF)
option(UNDEFINED_BEHAVIOR_IS_FATAL "Make undefined behavior sanitizer errors non-recoverable" OFF)

option(ENABLE_ALL_THE_DEBUG_MACROS "Enable all debug macros to validate they still compile" OFF)

option(INCLUDE_WASM_SPEC_TESTS "Download and include the WebAssembly spec testsuite" OFF)
option(BUILD_BROWSER_TESTS_ONLY "Only build the tests that run the browser's own executables" OFF)

# The optimizing JIT for JavaScript generates code for x86-64 and AArch64 on Linux and macOS, and for x86-64 on Windows.
# Elsewhere, LibJS runs JavaScript in its interpreter alone.
if (((LINUX AND NOT ANDROID) OR APPLE) AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|aarch64|arm64|ARM64)$")
    set(js_jit_default ON)
elseif (WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(js_jit_default ON)
else()
    set(js_jit_default OFF)
endif()
option(ENABLE_JS_JIT "Enable the optimizing JIT for JavaScript" ${js_jit_default})

set(LADYBIRD_CACHE_DIR "${PROJECT_BINARY_DIR}/../caches" CACHE PATH "Location of shared cache of downloaded files")
option(ENABLE_NETWORK_DOWNLOADS "Allow downloads of required files. If OFF, required files must already be present in LADYBIRD_CACHE_DIR" ON)

option(ENABLE_CLANG_PLUGINS "Enable building with the Clang plugins" OFF)
option(ENABLE_CLANG_PLUGINS_INVALID_FUNCTION_MEMBERS "Enable detecting invalid function types as members of GC-allocated objects" OFF)

if ((LINUX AND NOT ANDROID) OR BSD)
    set(freedesktop_files_default ON)
else()
    set(freedesktop_files_default OFF)
endif()

option(ENABLE_GUI_TARGETS "Enable building GUI targets" ON)
option(LADYBIRD_BUILD_QT "Build and bundle Qt with vcpkg on Linux" OFF)
option(ENABLE_INSTALL_HEADERS "Enable installing headers" ON)
option(ENABLE_INSTALL_FREEDESKTOP_FILES "Enable installing .desktop and .service files" ${freedesktop_files_default})
option(LADYBIRD_ENABLE_CPPTRACE "Enable use of cpptrace as the default library for stacktraces. If not available falls back to backtrace.h" ON)
option(LADYBIRD_ENABLE_UPDATE_CHECK "Check for newer releases of Ladybird" ON)
option(LADYBIRD_ENABLE_SPARKLE "Update the macOS app with Sparkle" ON)
if (NOT LADYBIRD_ENABLE_UPDATE_CHECK)
    # Sparkle only installs the releases that the update check finds.
    set(LADYBIRD_ENABLE_SPARKLE OFF)
endif()
option(LADYBIRD_GENERATE_DSYM "Generate dSYM bundles for binaries and libraries (macOS only)" OFF)
option(LADYBIRD_GENERATE_SYMBOL_FILES "Build with debug info and write the symbols of the installed executables to separate files" OFF)
if (LADYBIRD_GENERATE_SYMBOL_FILES AND MSVC)
    message(FATAL_ERROR "LADYBIRD_GENERATE_SYMBOL_FILES is not supported with MSVC-style toolchains")
endif()
option(LADYBIRD_BASELINE_CPU_TARGET "Use a common CPU baseline for portable binaries and ccache sharing" OFF)

set(LADYBIRD_VERSION "" CACHE STRING "Version of a release, such as 2026.46.0")
set(LADYBIRD_BROWSER_VERSION "${PROJECT_VERSION}")
if (NOT LADYBIRD_VERSION STREQUAL "")
    set(LADYBIRD_BROWSER_VERSION "${LADYBIRD_VERSION}")
endif()

# The stage of development that releases are in, if any, such as Alpha or Beta.
set(LADYBIRD_RELEASE_STAGE "Alpha")

# Like other browsers, our Linux packages keep Ladybird in a directory of its own.
set(LADYBIRD_PACKAGE_INSTALL_PREFIX "/opt/ladybird")

option(ENABLE_ADDRESS_SANITIZER "Enable address sanitizer testing in gcc/clang" OFF)
option(ENABLE_MEMORY_SANITIZER "Enable memory sanitizer testing in gcc/clang" OFF)
option(ENABLE_THREAD_SANITIZER "Enable thread sanitizer testing in gcc/clang" OFF)
option(ENABLE_FUZZERS "Build fuzzing targets" OFF)
option(ENABLE_FUZZERS_LIBFUZZER "Build fuzzers using Clang's libFuzzer" OFF)
option(ENABLE_FUZZERS_OSSFUZZ "Build OSS-Fuzz compatible fuzzers" OFF)
option(LADYBIRD_ENABLE_CCACHE "Compile through ccache when it is installed" ON)
set(LAGOM_USE_LINKER "" CACHE STRING "The linker to use (e.g. lld, mold) instead of the system default")
set(LAGOM_LINK_POOL_SIZE "" CACHE STRING "The maximum number of parallel jobs to use for linking")
option(LADYBIRD_ENABLE_LTO "Enable link-time optimization" OFF)
option(ENABLE_LAGOM_COVERAGE_COLLECTION "Enable code coverage instrumentation for lagom binaries in clang" OFF)

if (ENABLE_FUZZERS_LIBFUZZER)
    # With libfuzzer, we need to avoid a duplicate main() linker error giving false negatives
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY CACHE STRING "Type of target to use for try_compile()" FORCE)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/lagom_install_options.cmake)
