add_executable(ladybird MACOSX_BUNDLE
    ${LADYBIRD_SOURCES}
    main.mm
    Application/Application.mm
    Application/ApplicationBridge.cpp
    Application/ApplicationDelegate.mm
    Application/EventLoopImplementation.mm
    UI/Event.mm
    UI/Inspector.mm
    UI/InspectorController.mm
    UI/LadybirdWebView.mm
    UI/LadybirdWebViewBridge.cpp
    UI/Palette.mm
    UI/SearchPanel.mm
    UI/Tab.mm
    UI/TabController.mm
    UI/TaskManager.mm
    UI/TaskManagerController.mm
    Utilities/Conversions.mm
)
target_include_directories(ladybird PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ladybird PRIVATE "-framework Cocoa -framework UniformTypeIdentifiers" LibUnicode)
target_compile_options(ladybird PRIVATE
        -fobjc-arc
        -Wno-deprecated-anon-enum-enum-conversion # Required for CGImageCreate
)
create_ladybird_bundle(ladybird)
