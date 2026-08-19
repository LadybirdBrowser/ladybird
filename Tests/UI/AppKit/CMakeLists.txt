if (NOT APPLE)
    return()
endif()

ladybird_test("TestClipboardTypes.mm" UI/AppKit NAME TestAppKitClipboardTypes LIBS LibWebView)
target_sources(TestAppKitClipboardTypes PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/AppKit/Application/ClipboardTypes.mm")
target_include_directories(TestAppKitClipboardTypes PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/AppKit")
target_link_libraries(TestAppKitClipboardTypes PRIVATE "-framework Cocoa")
