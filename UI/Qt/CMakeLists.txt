set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

if (APPLE)
    # The macOS QRhiWidget presentation path uses QRhi's private Metal native
    # handle types to import IOSurfaces without CPU readback.
    set(QT_NO_PRIVATE_MODULE_WARNING ON)
    find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
endif()

qt_add_executable(ladybird main.cpp)
target_sources(ladybird PRIVATE
    Application.cpp
    Autocomplete.cpp
    BookmarksBar.cpp
    BrowserWindow.cpp
    ChromeStyle.cpp
    DevToolsBanner.cpp
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
    WebContentView.cpp
    WindowControlButton.cpp
    ladybird.qrc
)
target_link_libraries(ladybird PRIVATE Qt::Core Qt::Gui Qt::Widgets)

if (APPLE)
    target_sources(ladybird PRIVATE
        MacWindow.mm
        WebContentViewMac.mm
    )
    target_link_libraries(ladybird PRIVATE Qt::GuiPrivate "-framework Cocoa" "-framework IOSurface" "-framework Metal" "-framework QuartzCore")
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
