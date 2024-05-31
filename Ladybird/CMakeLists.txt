cmake_minimum_required(VERSION 3.23)

project(ladybird
    VERSION 0.0.1
    LANGUAGES CXX
    DESCRIPTION "Ladybird Web Browser"
)

include(GNUInstallDirs)

if (ANDROID OR IOS)
    set(BUILD_SHARED_LIBS OFF)
endif()

# FIXME: The rpath steps from lagom_install_rules.cmake do not produce a working binary
#        in the build directory on macOS.
#        We need to copy the libs into the build directory bundle as the install rules do.
set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
# See slide 100 of the following ppt :^)
# https://crascit.com/wp-content/uploads/2019/09/Deep-CMake-For-Library-Authors-Craig-Scott-CppCon-2019.pdf
if (NOT APPLE)
    set(CMAKE_INSTALL_RPATH $ORIGIN;$ORIGIN/../${CMAKE_INSTALL_LIBDIR})
endif()
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# Lagom
set(LADYBIRD_CUSTOM_TARGET_SUFFIX "-ladybird")
if (PROJECT_IS_TOP_LEVEL)
    set(LADYBIRD_CUSTOM_TARGET_SUFFIX "")
endif()

if (PROJECT_IS_TOP_LEVEL)
    get_filename_component(
        SERENITY_SOURCE_DIR "${ladybird_SOURCE_DIR}/.."
        ABSOLUTE
    )
    list(APPEND CMAKE_MODULE_PATH "${SERENITY_SOURCE_DIR}/Meta/CMake")
    include(cmake/EnableLagom.cmake)
    include(use_linker)
    include(lagom_compile_options)
    include(lagom_install_options)
else()
    # FIXME: Use SERENITY_SOURCE_DIR in Lagom/CMakeLists.txt
    set(SERENITY_SOURCE_DIR "${SERENITY_PROJECT_ROOT}")
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

if (ENABLE_QT)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)
endif()

set(SOURCES
    HelperProcess.cpp
    Utilities.cpp
)
set(LADYBIRD_HEADERS
    HelperProcess.h
    Types.h
    Utilities.h
)

if (ENABLE_QT)
    qt_add_executable(ladybird ${SOURCES})
    target_sources(ladybird PRIVATE
        Qt/Application.cpp
        Qt/AutoComplete.cpp
        Qt/BrowserWindow.cpp
        Qt/EventLoopImplementationQt.cpp
        Qt/EventLoopImplementationQtEventTarget.cpp
        Qt/FindInPageWidget.cpp
        Qt/Icon.cpp
        Qt/InspectorWidget.cpp
        Qt/LocationEdit.cpp
        Qt/Settings.cpp
        Qt/SettingsDialog.cpp
        Qt/Tab.cpp
        Qt/TabBar.cpp
        Qt/TaskManagerWindow.cpp
        Qt/TVGIconEngine.cpp
        Qt/StringUtils.cpp
        Qt/WebContentView.cpp
        Qt/ladybird.qrc
        Qt/main.cpp
    )
    target_link_libraries(ladybird PRIVATE Qt::Core Qt::Gui Qt::Network Qt::Widgets)
elseif (APPLE)
    add_executable(ladybird MACOSX_BUNDLE
        ${SOURCES}
        AppKit/main.mm
        AppKit/Application/Application.mm
        AppKit/Application/ApplicationBridge.cpp
        AppKit/Application/ApplicationDelegate.mm
        AppKit/Application/EventLoopImplementation.mm
        AppKit/UI/Event.mm
        AppKit/UI/Inspector.mm
        AppKit/UI/InspectorController.mm
        AppKit/UI/LadybirdWebView.mm
        AppKit/UI/LadybirdWebViewBridge.cpp
        AppKit/UI/Palette.mm
        AppKit/UI/SearchPanel.mm
        AppKit/UI/Tab.mm
        AppKit/UI/TabController.mm
        AppKit/UI/TaskManager.mm
        AppKit/UI/TaskManagerController.mm
        AppKit/Utilities/Conversions.mm
    )
    target_include_directories(ladybird PRIVATE AppKit)
    target_link_libraries(ladybird PRIVATE "-framework Cocoa -framework UniformTypeIdentifiers" LibUnicode)
    target_compile_options(ladybird PRIVATE
        -fobjc-arc
        -Wno-deprecated-anon-enum-enum-conversion # Required for CGImageCreate
    )
elseif(ANDROID)
    add_library(ladybird SHARED
        ${SOURCES}
        Android/src/main/cpp/LadybirdActivity.cpp
        Android/src/main/cpp/WebViewImplementationNative.cpp
        Android/src/main/cpp/WebViewImplementationNativeJNI.cpp
        Android/src/main/cpp/ALooperEventLoopImplementation.cpp
        Android/src/main/cpp/TimerExecutorService.cpp
        Android/src/main/cpp/JNIHelpers.cpp
    )
    target_link_libraries(ladybird PRIVATE LibArchive jnigraphics android)
else()
    # TODO: Check for other GUI frameworks here when we move them in-tree
    #       For now, we can export a static library of common files for chromes to link to
    add_library(ladybird STATIC ${SOURCES})
endif()

if (APPLE)
    target_sources(ladybird PRIVATE MachPortServer.cpp)
    target_link_libraries(ladybird PRIVATE LibThreading)
endif()

target_sources(ladybird PUBLIC FILE_SET ladybird TYPE HEADERS
    BASE_DIRS ${SERENITY_SOURCE_DIR}
    FILES ${LADYBIRD_HEADERS}
)
target_link_libraries(ladybird PRIVATE AK LibCore LibFileSystem LibGfx LibImageDecoderClient LibIPC LibJS LibMain LibSQL LibWeb LibWebView LibProtocol LibURL)

target_include_directories(ladybird PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(ladybird PRIVATE ${SERENITY_SOURCE_DIR}/Userland/)
target_include_directories(ladybird PRIVATE ${SERENITY_SOURCE_DIR}/Userland/Services/)

function(set_helper_process_properties)
    set(targets ${ARGV})
    if (APPLE)
        # Store helper processes in the same bundle directory as the main application
        set_target_properties(${targets} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:ladybird>")
    else()
        set_target_properties(${targets} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${IN_BUILD_PREFIX}${CMAKE_INSTALL_LIBEXECDIR}")

        if (NOT CMAKE_INSTALL_LIBEXECDIR STREQUAL "libexec")
            set_source_files_properties(Utilities.cpp PROPERTIES COMPILE_DEFINITIONS LADYBIRD_LIBEXECDIR="${CMAKE_INSTALL_LIBEXECDIR}")
            set_source_files_properties(Utilities.cpp TARGET_DIRECTORY ${targets} PROPERTIES COMPILE_DEFINITIONS LADYBIRD_LIBEXECDIR="${CMAKE_INSTALL_LIBEXECDIR}")
        endif()
    endif()
endfunction()

add_executable(headless-browser
    ${SERENITY_SOURCE_DIR}/Userland/Utilities/headless-browser.cpp
    HelperProcess.cpp
    Utilities.cpp)

target_include_directories(headless-browser PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(headless-browser PRIVATE ${SERENITY_SOURCE_DIR}/Userland/)
target_link_libraries(headless-browser PRIVATE AK LibCore LibWeb LibWebView LibWebSocket LibCrypto LibFileSystem LibGemini LibHTTP LibImageDecoderClient LibJS LibGfx LibMain LibSQL LibTLS LibIPC LibDiff LibProtocol LibURL)

if (ANDROID)
    include(cmake/AndroidExtras.cmake)
endif()

add_custom_target(run${LADYBIRD_CUSTOM_TARGET_SUFFIX}
    COMMAND "${CMAKE_COMMAND}" -E env "SERENITY_SOURCE_DIR=${SERENITY_SOURCE_DIR}" "$<TARGET_FILE:ladybird>" $ENV{LAGOM_ARGS}
    USES_TERMINAL
    VERBATIM
)

add_custom_target(debug${LADYBIRD_CUSTOM_TARGET_SUFFIX}
    COMMAND "${CMAKE_COMMAND}" -E env "SERENITY_SOURCE_DIR=${SERENITY_SOURCE_DIR}" gdb -ex "set follow-fork-mode child" "$<TARGET_FILE:ladybird>"
    USES_TERMINAL
)

add_subdirectory(ImageDecoder)
add_subdirectory(RequestServer)
add_subdirectory(SQLServer)
add_subdirectory(WebContent)
add_subdirectory(WebDriver)
add_subdirectory(WebWorker)

set(ladybird_helper_processes ImageDecoder RequestServer SQLServer WebContent WebWorker)

add_dependencies(ladybird ${ladybird_helper_processes})
add_dependencies(headless-browser ${ladybird_helper_processes})
add_dependencies(WebDriver ladybird headless-browser)

function(create_ladybird_bundle target_name)
    set_target_properties(${target_name} PROPERTIES
        OUTPUT_NAME "Ladybird"
        MACOSX_BUNDLE_GUI_IDENTIFIER org.SerenityOS.Ladybird
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
        MACOSX_BUNDLE_INFO_PLIST "${SERENITY_SOURCE_DIR}/Ladybird/Info.plist"
        MACOSX_BUNDLE TRUE
        WIN32_EXECUTABLE TRUE
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER org.SerenityOS.Ladybird
    )

    if (APPLE)
        set(bundle_dir "$<TARGET_BUNDLE_DIR:${target_name}>")
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${bundle_dir}/Contents/Resources"
            COMMAND "iconutil" --convert icns "${SERENITY_SOURCE_DIR}/Ladybird/Icons/macos/app_icon.iconset" --output "${bundle_dir}/Contents/Resources/app_icon.icns"
        )
    endif()
endfunction()

create_ladybird_bundle(ladybird)
set_helper_process_properties(${ladybird_helper_processes})
if (APPLE)
    set_helper_process_properties(headless-browser WebDriver)
endif()

include(cmake/ResourceFiles.cmake)
set(resource_base_dir "${CMAKE_BINARY_DIR}/${IN_BUILD_PREFIX}${CMAKE_INSTALL_DATADIR}/Lagom")
if (APPLE)
    set(resource_base_dir "$<TARGET_BUNDLE_DIR:ladybird>/Contents/Resources")
endif()
copy_resources_to_build(${resource_base_dir} ladybird)

if(NOT CMAKE_SKIP_INSTALL_RULES)
    include(cmake/InstallRules.cmake)
endif()

include(CTest)
if (BUILD_TESTING)
    add_test(
        NAME LibWeb
        COMMAND $<TARGET_FILE:headless-browser> --resources "${resource_base_dir}" --run-tests ${SERENITY_SOURCE_DIR}/Tests/LibWeb --dump-failed-ref-tests
    )
    add_test(
        NAME WPT
        CONFIGURATIONS Integration
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../Tests/LibWeb/WPT/run.sh
    )
    set_tests_properties(LibWeb WPT PROPERTIES
        DEPENDS ladybird
        ENVIRONMENT QT_QPA_PLATFORM=offscreen
        ENVIRONMENT "SERENITY_SOURCE_DIR=${SERENITY_SOURCE_DIR}"
    )
endif()
