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
