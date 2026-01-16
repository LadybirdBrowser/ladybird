cmake_minimum_required (VERSION 3.21)

if (APPLE AND NOT CMAKE_OSX_SYSROOT)
    set(CMAKE_OSX_SYSROOT macosx)
endif()

project(
    Lagom
    VERSION 0.0.0
    DESCRIPTION "Host build of SerenityOS libraries and applications"
    HOMEPAGE_URL "https://github.com/SerenityOS/serenity"
    LANGUAGES C CXX
)

if (${ENABLE_LAGOM_LADYBIRD} OR $CACHE{ENABLE_LAGOM_LADYBIRD})
    message(FATAL_ERROR
        "The ENABLE_LAGOM_LADYBIRD option is no longer supported.\n"
        "Please use the top-level CMakeLists.txt to enable Ladybird builds.\n")
endif()

# This is required for CMake (when invoked for a Lagom-only build) to
# ignore any files downloading during the build, e.g. public_suffix_list.dat.
# https://cmake.org/cmake/help/latest/policy/CMP0058.html
cmake_policy(SET CMP0058 NEW)

# Make CMAKE_EXE_LINKER_FLAGS have an effect on `try_compile()` jobs.
# This is required if we want to have the `LAGOM_USE_LINKER` option
# take effect in `check_linker_flag` checks.
cmake_policy(SET CMP0056 NEW)

# Ensure that when single-value arguments are passed to `cmake_parse_arguments()`
# with no or empty string arguments, they are still defined.
if (POLICY CMP0174)
    cmake_policy(SET CMP0174 NEW)
endif()

get_filename_component(
    LADYBIRD_PROJECT_ROOT "${PROJECT_SOURCE_DIR}/../.."
    ABSOLUTE CACHE
)
set(SerenityOS_SOURCE_DIR "${LADYBIRD_PROJECT_ROOT}" CACHE STRING "")

list(APPEND CMAKE_MODULE_PATH "${LADYBIRD_PROJECT_ROOT}/Meta/CMake")

if(NOT COMMAND ladybird_option)
    macro(ladybird_option)
        set(${ARGV})
    endmacro()
endif()

include(check_for_dependencies)
include(use_linker)
include(lagom_options NO_POLICY_SCOPE)

if(ENABLE_ALL_THE_DEBUG_MACROS)
    include(all_the_debug_macros)
endif()

find_package(Threads REQUIRED)
# FIXME: This global link libraries is required to workaround linker issues (on some systems)
# from the Ladybird import. See https://github.com/SerenityOS/serenity/issues/16847
link_libraries(Threads::Threads)

if (ENABLE_LAGOM_CCACHE)
    include(setup_ccache)
endif()

if (ENABLE_FUZZERS_LIBFUZZER OR ENABLE_FUZZERS_OSSFUZZ)
    set(ENABLE_FUZZERS ON)
endif()

# We need to make sure not to build code generators for Fuzzer builds, as they already have their own main.cpp
# Instead, we import them from a previous install of Lagom. This mandates a two-stage build for fuzzers.
# The same concern goes for cross-compile builds, where we need the tools built for the host
set(BUILD_LAGOM_TOOLS ON)
if (ENABLE_FUZZERS OR CMAKE_CROSSCOMPILING)
    find_package(LagomTools REQUIRED)
    set(BUILD_LAGOM_TOOLS OFF)
endif()

include(lagom_compile_options)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (ENABLE_COMPILETIME_FORMAT_CHECK)
    add_compile_definitions(ENABLE_COMPILETIME_FORMAT_CHECK)
endif()

if (HAIKU)
    # Haiku needs some extra compile definitions to make important stuff in its headers available.
    add_compile_definitions(_DEFAULT_SOURCE)
    add_compile_definitions(_GNU_SOURCE)
    add_compile_definitions(__USE_GNU)
endif()

if (ENABLE_FUZZERS)
    add_cxx_compile_options(-fno-omit-frame-pointer)
endif()

add_library(JSClangPlugin INTERFACE)
add_library(GenericClangPlugin INTERFACE)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang$")
    if (ENABLE_FUZZERS_LIBFUZZER)
        add_cxx_compile_options(-fsanitize=fuzzer)
        set(LINKER_FLAGS "${LINKER_FLAGS} -fsanitize=fuzzer")
    endif()

    # Vanilla host builds only for building the clang plugins
    if (ENABLE_CLANG_PLUGINS AND NOT CROSS_COMPILING AND NOT ENABLE_FUZZERS AND NOT ENABLE_COMPILER_EXPLORER_BUILD)
        add_subdirectory(ClangPlugins)
        depend_on_clang_plugin(JSClangPlugin LibJSGCClangPlugin)
        depend_on_clang_plugin(GenericClangPlugin LambdaCaptureClangPlugin)
    endif()
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if (ENABLE_FUZZERS_LIBFUZZER)
        message(FATAL_ERROR
            "Fuzzer Sanitizer (-fsanitize=fuzzer) is only supported for Fuzzer targets with LLVM. "
            "Reconfigure CMake with -DCMAKE_C_COMPILER and -DCMAKE_CXX_COMPILER pointing to a clang-based toolchain "
     "or build binaries without built-in fuzzing support by setting -DENABLE_FUZZERS instead."
        )
    endif()
endif()

# These are here to support Fuzzili builds further down the directory stack
set(ORIGINAL_CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
set(ORIGINAL_CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
set(ORIGINAL_CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS}")

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${LINKER_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${LINKER_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${LINKER_FLAGS}")

configure_file(../../AK/Debug.h.in AK/Debug.h @ONLY)

include_directories(../..)
include_directories(../../Libraries)
include_directories(../../Services)
include_directories(${CMAKE_BINARY_DIR})
include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_BINARY_DIR}/Libraries)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/Services)

# install rules, think about moving to its own helper cmake file
include(CMakePackageConfigHelpers)

# find_package(<package>) call for consumers to find this project
set(package Lagom CACHE STRING "")

# Allow package maintainers to freely override the path for the configs
set(Lagom_INSTALL_CMAKEDIR "${CMAKE_INSTALL_DATADIR}/${package}"
    CACHE PATH "CMake package config location relative to the install prefix")
mark_as_advanced(Lagom_INSTALL_CMAKEDIR)

install(
    FILES "${LADYBIRD_PROJECT_ROOT}/Meta/CMake/lagom-install-config.cmake"
    DESTINATION "${Lagom_INSTALL_CMAKEDIR}"
    RENAME "${package}Config.cmake"
    COMPONENT Lagom_Development
)

install(
    EXPORT LagomTargets
    NAMESPACE Lagom::
    DESTINATION "${Lagom_INSTALL_CMAKEDIR}"
    COMPONENT Lagom_Development
)

if (WIN32)
    find_package(pthread REQUIRED)
    find_package(mman REQUIRED)
endif()

if (ENABLE_RUST)
    include(FetchContent)
    FetchContent_Declare(
        Corrosion
        GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
        GIT_TAG v0.5.1
    )
    FetchContent_MakeAvailable(Corrosion)
endif()

include(targets)

# Plugins need to be installed in order to be used by non-lagom builds
add_lagom_library_install_rules(GenericClangPlugin)
add_lagom_library_install_rules(JSClangPlugin)

# Create mostly empty targets for system libraries we don't need to build for Lagom
add_library(NoCoverage INTERFACE)
# "install" these special targets to placate CMake
install(TARGETS NoCoverage EXPORT LagomTargets)

# AK
add_ladybird_subdirectory(AK)

# LibCore
add_ladybird_subdirectory(Libraries/LibCore)

# LibMain
add_ladybird_subdirectory(Libraries/LibMain)

# LibFileSystem
# This is needed even if Lagom is not enabled because it is depended upon by code generators.
add_ladybird_subdirectory(Libraries/LibFileSystem)

# LibIDL
# This is used by the BindingsGenerator so needs to always be built.
add_ladybird_subdirectory(Libraries/LibIDL)

# Code Generators and other host tools
if (BUILD_LAGOM_TOOLS)
    add_subdirectory(Tools)
endif()

if (LAGOM_TOOLS_ONLY)
    return()
endif()

# Lagom Libraries
# FIXME: Move these calls to the relevant client library CMakeLists
# Note that the Services themselves are only built if ENABLE_GUI_TARGETS, from top level.
compile_ipc(${LADYBIRD_PROJECT_ROOT}/Services/RequestServer/RequestClient.ipc Services/RequestServer/RequestClientEndpoint.h)
compile_ipc(${LADYBIRD_PROJECT_ROOT}/Services/RequestServer/RequestServer.ipc Services/RequestServer/RequestServerEndpoint.h)
compile_ipc(${LADYBIRD_PROJECT_ROOT}/Services/WebContent/WebContentServer.ipc Services/WebContent/WebContentServerEndpoint.h)
compile_ipc(${LADYBIRD_PROJECT_ROOT}/Services/WebContent/WebContentClient.ipc Services/WebContent/WebContentClientEndpoint.h)
compile_ipc(${LADYBIRD_PROJECT_ROOT}/Services/WebContent/WebDriverClient.ipc Services/WebContent/WebDriverClientEndpoint.h)
compile_ipc(${LADYBIRD_PROJECT_ROOT}/Services/WebContent/WebDriverServer.ipc Services/WebContent/WebDriverServerEndpoint.h)
compile_ipc(${LADYBIRD_PROJECT_ROOT}/Services/WebContent/WebUIClient.ipc Services/WebContent/WebUIClientEndpoint.h)
compile_ipc(${LADYBIRD_PROJECT_ROOT}/Services/WebContent/WebUIServer.ipc Services/WebContent/WebUIServerEndpoint.h)

add_ladybird_subdirectory(Libraries)

if (ENABLE_FUZZERS)
    add_subdirectory(Fuzzers)
endif()

# No utilities or tests in these configs
if (ENABLE_FUZZERS OR ENABLE_COMPILER_EXPLORER_BUILD OR ANDROID OR IOS)
    return()
endif()

# Lagom Utilities
add_ladybird_subdirectory(Utilities)

# Tests
include(CTest)
if (BUILD_TESTING)
    add_ladybird_subdirectory(Tests)
endif()
