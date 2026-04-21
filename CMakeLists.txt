cmake_minimum_required(VERSION 3.25)

if (VCPKG_TARGET_ANDROID)
    # If we are building for Android, we must load vcpkg_android.cmake before the project() declaration.
    # This ensures that the CMAKE_TOOLCHAIN_FILE is set correctly.
    # (we cannot set CMAKE_TOOLCHAIN_FILE from Gradle, unfortunately, so this is the only place we can do it.)
    include("UI/Android/vcpkg_android.cmake")
endif()

# vcpkg flags depend on what linker we are using
include("Meta/CMake/use_linker.cmake")

if (APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0)
endif()

include("Meta/CMake/gui_framework.cmake")

# Pass additional information to vcpkg if we are using it.
if (CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg.cmake$")
    set(CMAKE_PROJECT_ladybird_INCLUDE_BEFORE "Meta/CMake/vcpkg/generate_vcpkg_toolchain_variables.cmake")
    if (LADYBIRD_GUI_FRAMEWORK STREQUAL "Qt" OR LADYBIRD_GUI_FRAMEWORK STREQUAL "Gtk")
        string(TOLOWER "${LADYBIRD_GUI_FRAMEWORK}" framework_feature)
        set(VCPKG_MANIFEST_FEATURES "${framework_feature}" CACHE STRING "" FORCE)
    endif()
endif()

if (APPLE AND NOT CMAKE_OSX_SYSROOT)
    set(CMAKE_OSX_SYSROOT macosx)
endif()

project(ladybird
        VERSION 0.1.0
        LANGUAGES C CXX
        DESCRIPTION "Ladybird Web Browser"
        HOMEPAGE_URL "https://ladybird.org"
)

if (ANDROID OR IOS)
    set(BUILD_SHARED_LIBS OFF)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Ensure that when single-value arguments are passed to `cmake_parse_arguments()`
# with no or empty string arguments, they are still defined.
if (POLICY CMP0174)
    cmake_policy(SET CMP0174 NEW)
endif()

set(LADYBIRD_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${LADYBIRD_SOURCE_DIR}/Meta/CMake")

include(compile_options)
include(check_for_dependencies)
include(cmake_options)

if(ENABLE_ALL_THE_DEBUG_MACROS)
    include(all_the_debug_macros)
endif()

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

if (ENABLE_COMPILETIME_FORMAT_CHECK)
    add_compile_definitions(ENABLE_COMPILETIME_FORMAT_CHECK)
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
        add_subdirectory(Meta/Lagom/ClangPlugins)
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

include_directories(${LADYBIRD_SOURCE_DIR})
include_directories(Libraries)
include_directories(Services)
include_directories(${CMAKE_BINARY_DIR})
include_directories(${CMAKE_BINARY_DIR}/Libraries)
include_directories(${CMAKE_BINARY_DIR}/Services)

include(lagom_install)

include(rust_crate)
include(targets)

# Plugins need to be installed in order to be used by non-lagom builds
add_lagom_library_install_rules(GenericClangPlugin)
add_lagom_library_install_rules(JSClangPlugin)

# Create mostly empty targets for system libraries we don't need to build for Lagom
add_library(NoCoverage INTERFACE)
# "install" these special targets to placate CMake
install(TARGETS NoCoverage EXPORT LagomTargets)

add_subdirectory(AK)
add_subdirectory(Libraries)

# Code Generators and other host tools
if (BUILD_LAGOM_TOOLS)
    add_subdirectory(Meta/Lagom/Tools)
endif()

if (LAGOM_TOOLS_ONLY)
    return()
endif()

if (ENABLE_FUZZERS)
    add_subdirectory(Meta/Lagom/Fuzzers)
endif()

# No utilities or tests in these configs
if (NOT (ENABLE_FUZZERS OR ENABLE_COMPILER_EXPLORER_BUILD OR ANDROID OR IOS))
    add_subdirectory(Utilities)

    include(CTest)
    if (BUILD_TESTING)
        add_subdirectory(Tests)
    endif()
endif()

if (ENABLE_GUI_TARGETS)
    add_subdirectory(Services)
    add_subdirectory(UI)
endif()

add_custom_target(lint-shell-scripts
    COMMAND "${ladybird_SOURCE_DIR}/Meta/Linters/lint_shell_scripts.sh"
    USES_TERMINAL
)

add_custom_target(check-style
    COMMAND ${Python3_EXECUTABLE} "${ladybird_SOURCE_DIR}/Meta/Linters/check_style.py"
    USES_TERMINAL
)
