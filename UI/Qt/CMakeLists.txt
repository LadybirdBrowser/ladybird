set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

qt_add_executable(ladybird main.cpp)
target_sources(ladybird PRIVATE
    Application.cpp
    Autocomplete.cpp
    BookmarksBar.cpp
    BrowserWindow.cpp
    EventLoopImplementationQt.cpp
    EventLoopImplementationQtEventTarget.cpp
    FindInPageWidget.cpp
    Icon.cpp
    LocationEdit.cpp
    Menu.cpp
    Settings.cpp
    StringUtils.cpp
    Tab.cpp
    TabBar.cpp
    TVGIconEngine.cpp
    WaylandDmaBufPresenter.cpp
    WebContentView.cpp
    ladybird.qrc
)
target_link_libraries(ladybird PRIVATE Qt::Core Qt::Gui Qt::Widgets)

function(find_qt_qpa_include_dir out_var)
    get_target_property(qt_gui_include_dirs Qt::Gui INTERFACE_INCLUDE_DIRECTORIES)

    # FIXME: gross - graphics on Linux is not the most stable, but probably deserves better than this
    foreach(include_dir IN LISTS qt_gui_include_dirs)
        if (EXISTS "${include_dir}/qpa/qplatformnativeinterface.h")
            set(${out_var} "${include_dir}" PARENT_SCOPE)
            return()
        endif()

        file(GLOB qpa_header_candidates LIST_DIRECTORIES FALSE "${include_dir}/*/QtGui/qpa/qplatformnativeinterface.h")
        if (qpa_header_candidates)
            list(GET qpa_header_candidates 0 qpa_header)
            get_filename_component(qpa_dir "${qpa_header}" DIRECTORY)
            get_filename_component(qt_qpa_include_dir "${qpa_dir}" DIRECTORY)
            set(${out_var} "${qt_qpa_include_dir}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    set(${out_var} "" PARENT_SCOPE)
endfunction()

option(LADYBIRD_VCPKG_WAYLAND "Use Wayland/dmabuf dependencies from vcpkg rather than system packages" OFF)

if (LINUX AND NOT ANDROID)
    if (LADYBIRD_VCPKG_WAYLAND)
        find_library(WAYLAND_CLIENT_LIBRARY
            NAMES wayland-client
            PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
            NO_DEFAULT_PATH)

        find_program(WAYLAND_SCANNER
            NAMES wayland-scanner
            PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools"
            PATH_SUFFIXES wayland
            NO_DEFAULT_PATH)

        find_file(WAYLAND_DMABUF_PROTOCOL_XML
            NAMES linux-dmabuf-unstable-v1.xml
            PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/wayland-protocols/unstable/linux-dmabuf"
            NO_DEFAULT_PATH)

        set(WAYLAND_EXTRA_INCLUDE_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
    else()
        find_library(WAYLAND_CLIENT_LIBRARY
            NAMES wayland-client
            NO_PACKAGE_ROOT_PATH
            NO_CMAKE_PATH
            NO_CMAKE_ENVIRONMENT_PATH
            NO_CMAKE_INSTALL_PREFIX)

        find_program(WAYLAND_SCANNER
            NAMES wayland-scanner
            NO_PACKAGE_ROOT_PATH
            NO_CMAKE_PATH
            NO_CMAKE_ENVIRONMENT_PATH
            NO_CMAKE_INSTALL_PREFIX)

        find_file(WAYLAND_DMABUF_PROTOCOL_XML
            NAMES linux-dmabuf-unstable-v1.xml
            PATHS
                /usr/share/wayland-protocols/unstable/linux-dmabuf
                /usr/local/share/wayland-protocols/unstable/linux-dmabuf
            NO_DEFAULT_PATH)

        unset(WAYLAND_EXTRA_INCLUDE_DIR)
    endif()

    if (WAYLAND_CLIENT_LIBRARY AND WAYLAND_SCANNER AND WAYLAND_DMABUF_PROTOCOL_XML)
        set(GENERATED_WAYLAND_FILES
            "${CMAKE_CURRENT_BINARY_DIR}/linux-dmabuf-unstable-v1-client-protocol.h"
            "${CMAKE_CURRENT_BINARY_DIR}/linux-dmabuf-unstable-v1-protocol.c"
        )

        add_custom_command(
            OUTPUT ${GENERATED_WAYLAND_FILES}
            COMMAND ${WAYLAND_SCANNER} client-header
                ${WAYLAND_DMABUF_PROTOCOL_XML}
                "${CMAKE_CURRENT_BINARY_DIR}/linux-dmabuf-unstable-v1-client-protocol.h"
            COMMAND ${WAYLAND_SCANNER} private-code
                ${WAYLAND_DMABUF_PROTOCOL_XML}
                "${CMAKE_CURRENT_BINARY_DIR}/linux-dmabuf-unstable-v1-protocol.c"
            DEPENDS ${WAYLAND_DMABUF_PROTOCOL_XML}
            VERBATIM
        )

        target_sources(ladybird PRIVATE ${GENERATED_WAYLAND_FILES})
        target_include_directories(ladybird PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

        if (WAYLAND_EXTRA_INCLUDE_DIR)
            target_include_directories(ladybird PRIVATE "${WAYLAND_EXTRA_INCLUDE_DIR}")
        endif()

        find_qt_qpa_include_dir(QT_QPA_INCLUDE_DIR)
        if (QT_QPA_INCLUDE_DIR)
            target_include_directories(ladybird PRIVATE "${QT_QPA_INCLUDE_DIR}")
        endif()

        target_link_libraries(ladybird PRIVATE "${WAYLAND_CLIENT_LIBRARY}")
        target_compile_definitions(ladybird PRIVATE LADYBIRD_HAVE_WAYLAND_DMABUF=1)
    endif()
endif()

create_ladybird_bundle(ladybird)

if (WIN32)
    ladybird_windows_bin(ladybird CONSOLE)
    qt_generate_deploy_script(
        TARGET ladybird
        OUTPUT_SCRIPT ladybird_deploy_script
        CONTENT "
qt_deploy_runtime_dependencies(
    EXECUTABLE $<TARGET_FILE:ladybird>
    PLUGINS_DIR $<TARGET_FILE_DIR:ladybird>
    NO_TRANSLATIONS
)
")

    add_custom_command(TARGET ladybird POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -P ${ladybird_deploy_script}
        COMMAND_EXPAND_LISTS
    )
endif()
