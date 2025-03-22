qt_add_executable(ladybird ${LADYBIRD_SOURCES})
target_sources(ladybird PRIVATE
    Application.cpp
    AutoComplete.cpp
    BrowserWindow.cpp
    FindInPageWidget.cpp
    Icon.cpp
    LocationEdit.cpp
    Settings.cpp
    SettingsDialog.cpp
    Tab.cpp
    TabBar.cpp
    TVGIconEngine.cpp
    StringUtils.cpp
    WebContentView.cpp
    ladybird.qrc
    main.cpp
)
target_link_libraries(ladybird PRIVATE Qt::Core Qt::Gui Qt::Network Qt::Widgets)
create_ladybird_bundle(ladybird)
