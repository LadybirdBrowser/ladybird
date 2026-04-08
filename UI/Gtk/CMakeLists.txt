find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(LIBADWAITA REQUIRED IMPORTED_TARGET libadwaita-1>=1.4)

add_executable(ladybird main.cpp)

find_program(GLIB_COMPILE_RESOURCES NAMES glib-compile-resources REQUIRED)

set(GTK_UI_RESOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/browser-window.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/list-popover.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/location-entry.ui"
)

set(GTK_GRESOURCE_XML "${CMAKE_CURRENT_SOURCE_DIR}/Resources/resources.gresource.xml")
set(GTK_GRESOURCE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/gtk-ui-resources.cpp")

add_custom_command(
    OUTPUT "${GTK_GRESOURCE_SOURCE}"
    DEPENDS "${GTK_GRESOURCE_XML}" ${GTK_UI_RESOURCES}
    COMMAND "${GLIB_COMPILE_RESOURCES}"
        "${GTK_GRESOURCE_XML}"
        --sourcedir "${CMAKE_CURRENT_SOURCE_DIR}/Resources"
        --target "${GTK_GRESOURCE_SOURCE}"
        --generate-source
    VERBATIM
)
add_custom_target(gtk_ui_resources DEPENDS "${GTK_GRESOURCE_SOURCE}")

target_sources(ladybird PRIVATE
    Application.cpp
    BrowserWindow.cpp
    Dialogs.cpp
    Events.cpp
    EventLoopImplementationGtk.cpp
    Menu.cpp
    Tab.cpp
    WebContentView.cpp
    Widgets/LadybirdBrowserWindow.cpp
    Widgets/LadybirdLocationEntry.cpp
    Widgets/LadybirdWebView.cpp
    "${GTK_GRESOURCE_SOURCE}"
)
target_link_libraries(ladybird PRIVATE PkgConfig::GTK4 PkgConfig::LIBADWAITA)
add_dependencies(ladybird gtk_ui_resources)
create_ladybird_bundle(ladybird)
