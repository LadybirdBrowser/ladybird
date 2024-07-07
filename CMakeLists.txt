cmake_minimum_required(VERSION 3.25)

if (VCPKG_TARGET_ANDROID)
    # If we are building for Android, we must load vcpkg_android.cmake before the project() declaration.
    # This ensures that the CMAKE_TOOLCHAIN_FILE is set correctly.
    # (we cannot set CMAKE_TOOLCHAIN_FILE from Gradle, unfortunately, so this is the only place we can do it.)
    include("Ladybird/Android/vcpkg_android.cmake")
endif()

project(ladybird
        VERSION 0.1.0
        LANGUAGES C CXX
        DESCRIPTION "Ladybird Web Browser"
        HOMEPAGE_URL "https://ladybird.dev"
)

if (ANDROID OR IOS)
    set(BUILD_SHARED_LIBS OFF)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(LADYBIRD_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${LADYBIRD_SOURCE_DIR}/Meta/CMake")

include(Ladybird/cmake/EnableLagom.cmake)
include(use_linker)
include(lagom_compile_options)
include(lagom_install_options)

if (ENABLE_ADDRESS_SANITIZER)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

if (ENABLE_MEMORY_SANITIZER)
    add_compile_options(-fsanitize=memory -fsanitize-memory-track-origins -fno-omit-frame-pointer)
    add_link_options(-fsanitize=memory -fsanitize-memory-track-origins)
endif()

if (ENABLE_UNDEFINED_SANITIZER)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    if (UNDEFINED_BEHAVIOR_IS_FATAL)
        add_compile_options(-fno-sanitize-recover=undefined)
    endif()
    if (APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang$" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "17")
        add_compile_options(-fno-sanitize=function)
    endif()
    add_link_options(-fsanitize=undefined)
endif()

if (HAIKU)
    # Haiku needs some extra compile definitions to make important stuff in its headers available.
    add_compile_definitions(_DEFAULT_SOURCE)
    add_compile_definitions(_GNU_SOURCE)
    add_compile_definitions(__USE_GNU)
endif()

add_compile_options(-DAK_DONT_REPLACE_STD)
add_compile_options(-Wno-expansion-to-defined)
add_compile_options(-Wno-user-defined-literals)

if (ANDROID OR APPLE)
    serenity_option(ENABLE_QT OFF CACHE BOOL "Build ladybird application using Qt GUI")
else()
    serenity_option(ENABLE_QT ON CACHE BOOL "Build ladybird application using Qt GUI")
endif()

if (ANDROID AND ENABLE_QT)
    message(STATUS "Disabling Qt for Android")
    set(ENABLE_QT OFF CACHE BOOL "" FORCE)
endif()

if (ENABLE_QT AND ENABLE_GUI_TARGETS)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)
endif()

include(CTest) # for BUILD_TESTING option, default ON

if (ENABLE_GUI_TARGETS)
    add_subdirectory(Ladybird)
endif()

add_custom_target(lint-shell-scripts
    COMMAND "${ladybird_SOURCE_DIR}/Meta/lint-shell-scripts.sh"
    USES_TERMINAL
)

find_package(Python3 COMPONENTS Interpreter)
if (Python3_FOUND)
    add_custom_target(check-style
        COMMAND ${Python3_EXECUTABLE} "${ladybird_SOURCE_DIR}/Meta/check-style.py"
        USES_TERMINAL
    )
endif()
