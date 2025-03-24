cmake_minimum_required (VERSION 3.21)

project(
    Lagom
    VERSION 0.0.0
    DESCRIPTION "Host build of SerenityOS libraries and applications"
    HOMEPAGE_URL "https://github.com/SerenityOS/serenity"
    LANGUAGES C CXX
)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "12")
  message(FATAL_ERROR
      "A GCC version less than 12 was detected (${CMAKE_CXX_COMPILER_VERSION}), this is unsupported.\n"
      "Please re-read the build instructions documentation, and upgrade your host compiler.\n")
endif()

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

get_filename_component(
    SERENITY_PROJECT_ROOT "${PROJECT_SOURCE_DIR}/../.."
    ABSOLUTE CACHE
)
set(SerenityOS_SOURCE_DIR "${SERENITY_PROJECT_ROOT}" CACHE STRING "")

list(APPEND CMAKE_MODULE_PATH "${SERENITY_PROJECT_ROOT}/Meta/CMake")

if(NOT COMMAND serenity_option)
    macro(serenity_option)
        set(${ARGV})
    endmacro()
endif()

include(check_for_dependencies)
include(use_linker)
include(lagom_options NO_POLICY_SCOPE)

if(ENABLE_ALL_THE_DEBUG_MACROS)
    include(all_the_debug_macros)
endif()

# FIXME: BUILD_SHARED_LIBS has a default of OFF, as it's intended to be set by the
#        user when configuring the project. We should instead change libjs-test262
#        and oss-fuzz to set this option on their end, and enable it by default in
#        Meta/ladybird.sh. This is #9867.
option(BUILD_SHARED_LIBS "Build shared libraries instead of static libraries" ON)

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

include(ca_certificates_data)
include(lagom_compile_options)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (EMSCRIPTEN)
    set(CMAKE_EXECUTABLE_SUFFIX ".js")
    add_cxx_compile_options(-gsource-map)
    add_cxx_link_options(--emrun "SHELL:-s ALLOW_MEMORY_GROWTH")
endif()

if (ENABLE_ADDRESS_SANITIZER)
    add_cxx_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_cxx_link_options(-fsanitize=address)
    add_swift_compile_options(-sanitize=address)
    add_swift_link_options(-sanitize=address)
endif()

if (ENABLE_MEMORY_SANITIZER)
    add_cxx_compile_options(-fsanitize=memory -fsanitize-memory-track-origins -fno-omit-frame-pointer)
    add_cxx_link_options(-fsanitize=memory -fsanitize-memory-track-origins)
endif()

if (ENABLE_UNDEFINED_SANITIZER AND (APPLE OR NOT ENABLE_SWIFT))
    add_cxx_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    if (UNDEFINED_BEHAVIOR_IS_FATAL)
        add_cxx_compile_options(-fno-sanitize-recover=undefined)
    endif()
    if (APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang$" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "17")
        add_cxx_compile_options(-fno-sanitize=function)
    endif()
    add_cxx_link_options(-fsanitize=undefined)
    add_swift_compile_options(-sanitize=undefined)
    add_swift_link_options(-sanitize=undefined)
endif()

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
    add_cxx_compile_options(-Wno-overloaded-virtual)
    # FIXME: Re-enable this check when the warning stops triggering, or document why we can't stop it from triggering.
    # For now, there is a lot of unused private fields in LibWeb that trigger this that could be removed.
    # See issue #14137 for details
    add_cxx_compile_options(-Wno-unused-private-field)

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
    FILES "${SERENITY_PROJECT_ROOT}/Meta/CMake/lagom-install-config.cmake"
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

function(lagom_lib target_name fs_name)
    cmake_parse_arguments(LAGOM_LIBRARY "" "LIBRARY_TYPE" "SOURCES;LIBS" ${ARGN})
    string(REPLACE "Lib" "" library ${target_name})
    if (NOT LAGOM_LIBRARY_LIBRARY_TYPE)
        set(LAGOM_LIBRARY_LIBRARY_TYPE "")
    endif()
    add_library(${target_name} ${LAGOM_LIBRARY_LIBRARY_TYPE} ${LAGOM_LIBRARY_SOURCES})
    set_target_properties(
        ${target_name} PROPERTIES
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION_MAJOR}"
        EXPORT_NAME ${library}
        OUTPUT_NAME lagom-${fs_name}
    )
    target_link_libraries(${target_name} PRIVATE ${LAGOM_LIBRARY_LIBS})
    target_link_libraries(${target_name} PUBLIC GenericClangPlugin)

    if (NOT "${target_name}" STREQUAL "AK")
        target_link_libraries(${target_name} PRIVATE AK)
    endif()

    if (WIN32)
        find_package(pthread REQUIRED)
        target_include_directories(${target_name} PRIVATE ${PTHREAD_INCLUDE_DIR})
        target_link_libraries(${target_name} PRIVATE ${PTHREAD_LIBRARY})

        find_package(mman REQUIRED)
        target_include_directories(${target_name} PRIVATE ${MMAN_INCLUDE_DIR})
        target_link_libraries(${target_name} PRIVATE ${MMAN_LIBRARY})
    endif()

    # FIXME: Clean these up so that we don't need so many include dirs
    if (ENABLE_INSTALL_HEADERS)
        target_include_directories(${target_name} INTERFACE
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Services>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Libraries>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Services>
        )
    endif()
    add_lagom_library_install_rules(${target_name} ALIAS_NAME ${library})
    if (ENABLE_INSTALL_HEADERS)
        install(
            DIRECTORY "${SERENITY_PROJECT_ROOT}/Libraries/Lib${library}"
            COMPONENT Lagom_Development
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            FILES_MATCHING PATTERN "*.h"
        )
    endif()
    serenity_generated_sources(${target_name})
endfunction()

function(lagom_test source)
    cmake_parse_arguments(LAGOM_TEST "" "NAME;WORKING_DIRECTORY" "LIBS" ${ARGN})
    if (NOT DEFINED LAGOM_TEST_NAME)
        get_filename_component(LAGOM_TEST_NAME ${source} NAME_WE)
    endif()
    add_executable(${LAGOM_TEST_NAME} ${source})
    target_link_libraries(${LAGOM_TEST_NAME} PRIVATE AK LibCore LibFileSystem LibTest LibTestMain ${LAGOM_TEST_LIBS})
    add_test(
        NAME ${LAGOM_TEST_NAME}
        COMMAND ${LAGOM_TEST_NAME}
        WORKING_DIRECTORY ${LAGOM_TEST_WORKING_DIRECTORY}
    )
endfunction()

function(lagom_utility name)
    cmake_parse_arguments(LAGOM_UTILITY "" "" "SOURCES;LIBS" ${ARGN})

    add_executable("${name}" ${LAGOM_UTILITY_SOURCES})
    target_link_libraries("${name}" PRIVATE AK LibCore ${LAGOM_UTILITY_LIBS})
endfunction()

function(serenity_test test_src sub_dir)
    cmake_parse_arguments(PARSE_ARGV 2 SERENITY_TEST "MAIN_ALREADY_DEFINED" "CUSTOM_MAIN;NAME" "LIBS")
    # FIXME: Pass MAIN_ALREADY_DEFINED and CUSTOM_MAIN to support tests that use them.
    lagom_test(${test_src} LIBS ${SERENITY_TEST_LIBS} WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} NAME ${SERENITY_TEST_NAME})
endfunction()

function(serenity_bin name)
    add_executable(${name} ${SOURCES} ${GENERATED_SOURCES})
    add_executable(Lagom::${name} ALIAS ${name})
    target_link_libraries(${name} PUBLIC GenericClangPlugin)
    install(
        TARGETS ${target_name}
        EXPORT LagomTargets
        RUNTIME #
            COMPONENT Lagom_Runtime
        LIBRARY #
            COMPONENT Lagom_Runtime
            NAMELINK_COMPONENT Lagom_Development
        ARCHIVE #
            COMPONENT Lagom_Development
        INCLUDES #
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    serenity_generated_sources(${name})
endfunction()

function(serenity_lib name fs_name)
    cmake_parse_arguments(PARSE_ARGV 2 SERENITY_LIB "" "TYPE" "")
    lagom_lib(${name} ${fs_name} LIBRARY_TYPE ${SERENITY_LIB_TYPE} SOURCES ${SOURCES} ${GENERATED_SOURCES})
endfunction()

macro(add_serenity_subdirectory path)
    add_subdirectory("${SERENITY_PROJECT_ROOT}/${path}" "${CMAKE_CURRENT_BINARY_DIR}/${path}")
endmacro()

if (NOT TARGET all_generated)
    # Meta target to run all code-gen steps in the build.
    add_custom_target(all_generated)
endif()

# Plugins need to be installed in order to be used by non-lagom builds
add_lagom_library_install_rules(GenericClangPlugin)
add_lagom_library_install_rules(JSClangPlugin)

# Create mostly empty targets for system libraries we don't need to build for Lagom
add_library(LibCrypt INTERFACE)
if (NOT APPLE AND NOT ANDROID AND NOT EMSCRIPTEN AND NOT ${CMAKE_SYSTEM_NAME} MATCHES "OpenBSD" AND NOT HAIKU AND NOT WIN32)
    target_link_libraries(LibCrypt INTERFACE crypt) # LibCore::Account uses crypt() but it's not in libcrypt on macOS
endif()
add_library(NoCoverage INTERFACE)
# "install" these special targets to placate CMake
install(TARGETS LibCrypt NoCoverage EXPORT LagomTargets)

# AK
add_serenity_subdirectory(AK)

# LibCoreMinimal
add_serenity_subdirectory(Libraries/LibCore)
if (${CMAKE_SYSTEM_NAME} MATCHES "NetBSD")
    # NetBSD has its shm_open and shm_unlink functions in librt so we need to link that
    target_link_libraries(LibCoreMinimal PRIVATE rt)
endif()

# LibMain
add_serenity_subdirectory(Libraries/LibMain)

# LibFileSystem
# This is needed even if Lagom is not enabled because it is depended upon by code generators.
add_serenity_subdirectory(Libraries/LibFileSystem)

# LibIDL
# This is used by the BindingsGenerator so needs to always be built.
add_serenity_subdirectory(Libraries/LibIDL)

# Manually install AK headers
if (ENABLE_INSTALL_HEADERS)
    install(
        DIRECTORY "${SERENITY_PROJECT_ROOT}/AK"
        COMPONENT Lagom_Development
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h"
    )
    install(FILES
        ${Lagom_BINARY_DIR}/AK/Debug.h
        ${Lagom_BINARY_DIR}/AK/Backtrace.h
        COMPONENT Lagom_Development
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/AK"
    )
endif()

# Code Generators and other host tools
if (BUILD_LAGOM_TOOLS)
    add_subdirectory(Tools)
endif()

if (LAGOM_TOOLS_ONLY)
    return()
endif()

# Lagom Libraries
set(lagom_standard_libraries
    Compress
    Crypto
    Diff
    DNS
    GC
    HTTP
    IPC
    JS
    Line
    Regex
    Requests
    RIFF
    Syntax
    TextCodec
    Threading
    TLS
    Unicode
    URL
    Wasm
    WebSocket
    XML
)

if (ENABLE_GUI_TARGETS)
    list(APPEND lagom_standard_libraries
        DevTools
        Gfx
        ImageDecoderClient
        Media
        WebView
        Web
    )
endif()

target_link_libraries(LibCore PRIVATE LibURL Threads::Threads)
if (${CMAKE_SYSTEM_NAME} MATCHES "NetBSD")
    # NetBSD has its shm_open and shm_unlink functions in librt so we need to link that
    target_link_libraries(LibCore PRIVATE rt)
endif()
if (${CMAKE_SYSTEM_NAME} MATCHES "SunOS")
    # Solaris has socket and networking related functions in two extra libraries
    target_link_libraries(LibCore PRIVATE nsl socket)
endif()
if (HAIKU)
    # Haiku has networking related functions in the network library
    target_link_libraries(LibCore PRIVATE network)
endif()

compile_ipc(${SERENITY_PROJECT_ROOT}/Services/RequestServer/RequestClient.ipc Services/RequestServer/RequestClientEndpoint.h)
compile_ipc(${SERENITY_PROJECT_ROOT}/Services/RequestServer/RequestServer.ipc Services/RequestServer/RequestServerEndpoint.h)
compile_ipc(${SERENITY_PROJECT_ROOT}/Services/WebContent/WebContentServer.ipc Services/WebContent/WebContentServerEndpoint.h)
compile_ipc(${SERENITY_PROJECT_ROOT}/Services/WebContent/WebContentClient.ipc Services/WebContent/WebContentClientEndpoint.h)
compile_ipc(${SERENITY_PROJECT_ROOT}/Services/WebContent/WebDriverClient.ipc Services/WebContent/WebDriverClientEndpoint.h)
compile_ipc(${SERENITY_PROJECT_ROOT}/Services/WebContent/WebDriverServer.ipc Services/WebContent/WebDriverServerEndpoint.h)
compile_ipc(${SERENITY_PROJECT_ROOT}/Services/WebContent/WebUIClient.ipc Services/WebContent/WebUIClientEndpoint.h)
compile_ipc(${SERENITY_PROJECT_ROOT}/Services/WebContent/WebUIServer.ipc Services/WebContent/WebUIServerEndpoint.h)

foreach(lib IN LISTS lagom_standard_libraries)
    add_serenity_subdirectory("Libraries/Lib${lib}")
endforeach()

if (ENABLE_FUZZERS)
    add_subdirectory(Fuzzers)
endif()

# No utilities or tests in these configs
if (ENABLE_FUZZERS OR ENABLE_COMPILER_EXPLORER_BUILD OR ANDROID OR IOS)
    return()
endif()

# Lagom Utilities
lagom_utility(abench SOURCES ../../Utilities/abench.cpp LIBS LibMain LibFileSystem LibMedia)

lagom_utility(dns SOURCES ../../Utilities/dns.cpp LIBS LibDNS LibMain LibTLS LibCrypto)

if (ENABLE_GUI_TARGETS)
    lagom_utility(animation SOURCES ../../Utilities/animation.cpp LIBS LibGfx LibMain)
    lagom_utility(image SOURCES ../../Utilities/image.cpp LIBS LibGfx LibMain)
endif()

lagom_utility(js SOURCES ../../Utilities/js.cpp LIBS LibCrypto LibJS LibLine LibUnicode LibMain LibTextCodec LibGC Threads::Threads)
lagom_utility(test262-runner SOURCES ../../Tests/LibJS/test262-runner.cpp LIBS LibJS LibFileSystem LibGC)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(CheckCSourceCompiles)
    # Check for musl's declaration of __assert_fail
    check_c_source_compiles(
        "
        #include <assert.h>
        __attribute__((__noreturn__)) void __assert_fail(char const* assertion, char const* file, int line, char const* function) {}
        int main() {}
        "
        ASSERT_FAIL_HAS_INT
    )
endif()

if (ASSERT_FAIL_HAS_INT OR EMSCRIPTEN)
    target_compile_definitions(test262-runner PRIVATE ASSERT_FAIL_HAS_INT)
endif()

lagom_utility(wasm SOURCES ../../Utilities/wasm.cpp LIBS LibFileSystem LibWasm LibLine LibMain LibJS)
lagom_utility(xml SOURCES ../../Utilities/xml.cpp LIBS LibFileSystem LibMain LibXML LibURL)

include(CTest)
if (BUILD_TESTING)
    # LibTest
    file(GLOB LIBTEST_SOURCES CONFIGURE_DEPENDS "../../Libraries/LibTest/*.cpp")
    list(FILTER LIBTEST_SOURCES EXCLUDE REGEX ".*Main.cpp$")
    add_library(
        LibTest
        ${LIBTEST_SOURCES}
    )
    target_link_libraries(LibTest PRIVATE AK LibCore LibFileSystem)
    set_target_properties(LibTest PROPERTIES OUTPUT_NAME lagom-test)
    add_library(
        LibTestMain
        OBJECT
        "${SERENITY_PROJECT_ROOT}/Libraries/LibTest/TestMain.cpp"
    )
    target_link_libraries(LibTest PUBLIC GenericClangPlugin)
    target_link_libraries(LibTestMain PUBLIC GenericClangPlugin)

    # LibTest tests from Tests/
    set(TEST_DIRECTORIES
        AK
        LibCrypto
        LibCompress
        LibDNS
        LibGC
        LibTest
        LibTextCodec
        LibThreading
        LibUnicode
        LibURL
        LibXML
    )

    if (ENABLE_GUI_TARGETS)
        list(APPEND TEST_DIRECTORIES
            LibGfx
            LibMedia
            LibWeb
            LibWebView
        )
    endif()

    if (ENABLE_CLANG_PLUGINS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang$")
        list(APPEND TEST_DIRECTORIES ClangPlugins)
    endif()

    foreach (dir IN LISTS TEST_DIRECTORIES)
        add_serenity_subdirectory("Tests/${dir}")
    endforeach()

    # LibTLS needs a special working directory to find cacert.pem
    lagom_test(../../Tests/LibTLS/TestTLSHandshake.cpp LibTLS LIBS LibTLS LibCrypto)
    lagom_test(../../Tests/LibTLS/TestTLSCertificateParser.cpp LibTLS LIBS LibTLS LibCrypto)

    # LibCore
    lagom_test(../../Tests/LibCore/TestLibCoreArgsParser.cpp)

    if ((LINUX OR APPLE) AND NOT EMSCRIPTEN)
        lagom_test(../../Tests/LibCore/TestLibCoreFileWatcher.cpp)
        lagom_test(../../Tests/LibCore/TestLibCorePromise.cpp LIBS LibThreading)
    endif()

    lagom_test(../../Tests/LibCore/TestLibCoreDateTime.cpp LIBS LibUnicode)

    if (ENABLE_SWIFT)
        find_package(SwiftTesting REQUIRED)

        add_executable(TestCoreSwift
            ../../Tests/LibCore/TestEventLoopActor.swift
            ../../Tests/LibCore/TestEventLoop.cpp
        )

        # FIXME: Swift doesn't seem to like object libraries for @main
        target_sources(TestCoreSwift PRIVATE ../../Tests/Resources/SwiftTestMain.swift)

        set_target_properties(TestCoreSwift PROPERTIES SUFFIX .swift-testing)
        target_include_directories(TestCoreSwift PRIVATE ../../Tests/LibCore)
        target_link_libraries(TestCoreSwift PRIVATE AK LibCore SwiftTesting::SwiftTesting)
        add_test(NAME TestCoreSwift COMMAND TestCoreSwift)
    endif()


    # RegexLibC test POSIX <regex.h> and contains many Serenity extensions
    # It is therefore not reasonable to run it on Lagom, and we only run the Regex test
    lagom_test(../../Tests/LibRegex/Regex.cpp LIBS LibRegex WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../Tests/LibRegex)

    # JavaScriptTestRunner + LibTest tests
    # test-js
    add_executable(test-js
        ../../Tests/LibJS/test-js.cpp
        ../../Libraries/LibTest/JavaScriptTestRunnerMain.cpp)
    target_link_libraries(test-js AK LibCore LibFileSystem LibTest LibJS LibGC)
    add_test(
        NAME JS
        COMMAND test-js --show-progress=false
    )
    set_tests_properties(JS PROPERTIES ENVIRONMENT LADYBIRD_SOURCE_DIR=${SERENITY_PROJECT_ROOT})

    # Extra tests from Tests/LibJS
    lagom_test(../../Tests/LibJS/test-invalid-unicode-js.cpp LIBS LibJS)
    lagom_test(../../Tests/LibJS/test-value-js.cpp LIBS LibJS)

    # test-wasm
    add_executable(test-wasm
        ../../Tests/LibWasm/test-wasm.cpp
        ../../Libraries/LibTest/JavaScriptTestRunnerMain.cpp)
    target_link_libraries(test-wasm AK LibCore LibFileSystem LibTest LibWasm LibJS LibCrypto LibGC)
    set(wasm_test_root "${SERENITY_PROJECT_ROOT}")
    if (INCLUDE_WASM_SPEC_TESTS)
       set(wasm_test_root "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    add_test(
        NAME Wasm
        COMMAND test-wasm --show-progress=false "${wasm_test_root}/Libraries/LibWasm/Tests"
    )
endif()

install(TARGETS js COMPONENT js)

set(CPACK_GENERATOR "TGZ")
set(CPACK_STRIP_FILES TRUE)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL js)
if (APPLE)
    if("arm64" IN_LIST CMAKE_OSX_ARCHITECTURES AND "x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES)
        set(CPACK_SYSTEM_NAME "macOS-universal2")
    else()
        set(CPACK_SYSTEM_NAME "macOS-${CMAKE_SYSTEM_PROCESSOR}")
    endif()
else()
    set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(CPACK_ARCHIVE_JS_FILE_NAME "ladybird-js-${CPACK_SYSTEM_NAME}")
set(CPACK_PACKAGE_FILE_NAME "ladybird-js-${CPACK_SYSTEM_NAME}")
include(CPack)
