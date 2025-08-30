qt_add_executable(ladybird main.cpp)
target_sources(ladybird PRIVATE
    Application.cpp
    Autocomplete.cpp
    BrowserWindow.cpp
    FindInPageWidget.cpp
    Icon.cpp
    LocationEdit.cpp
    Menu.cpp
    Settings.cpp
    StringUtils.cpp
    Tab.cpp
    TabBar.cpp
    TVGIconEngine.cpp
    WebContentView.cpp
    ladybird.qrc
)
target_link_libraries(ladybird PRIVATE Qt::Core Qt::Gui Qt::Widgets)
lagom_copy_runtime_dlls(ladybird)
create_ladybird_bundle(ladybird)

if (WIN32)
    qt_generate_deploy_script(
        TARGET ladybird
        OUTPUT_SCRIPT ladybird_deploy_script
        CONTENT "
qt_deploy_runtime_dependencies(
    EXECUTABLE $<TARGET_FILE:ladybird>
    PLUGINS_DIR $<TARGET_FILE_DIR:ladybird>
)
")

    add_custom_command(TARGET ladybird POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -P ${ladybird_deploy_script}
        COMMAND_EXPAND_LISTS
    )
endif()
