qt_add_executable(ladybird main.cpp)
target_sources(ladybird PRIVATE
    Application.cpp
    Autocomplete.cpp
    BrowserWindow.cpp
    FindInPageWidget.cpp
    Icon.cpp
    LocationEdit.cpp
    Settings.cpp
    Tab.cpp
    TabBar.cpp
    TVGIconEngine.cpp
    StringUtils.cpp
    WebContentView.cpp
    ladybird.qrc
)
target_link_libraries(ladybird PRIVATE Qt::Core Qt::Gui Qt::Widgets)
create_ladybird_bundle(ladybird)

if (WIN32)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS ${Qt6_DIR}/../../../bin)

    add_custom_command(TARGET ladybird POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -E copy_if_different
            $<TARGET_RUNTIME_DLLS:ladybird>
            $<TARGET_FILE_DIR:ladybird>
        COMMAND ${WINDEPLOYQT_EXECUTABLE} $<TARGET_FILE:ladybird>
        COMMAND_EXPAND_LISTS
    )
endif()
