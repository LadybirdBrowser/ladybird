if (NOT APPLE)
    return()
endif()

add_library(AppKitEventLoopTestMain OBJECT
    AppKitEventLoopTestMain.mm
    "${LADYBIRD_SOURCE_DIR}/Libraries/LibTest/AssertionHandler.cpp"
)
target_include_directories(AppKitEventLoopTestMain PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/AppKit")
target_link_libraries(AppKitEventLoopTestMain PRIVATE AK LibCore LibTest "-framework Cocoa")

ladybird_test("TestEventLoopImplementation.cpp" UI/AppKit
    CUSTOM_MAIN "$<TARGET_OBJECTS:AppKitEventLoopTestMain>"
    NAME TestAppKitEventLoopImplementation
    LIBS LibSync LibThreading
)
target_sources(TestAppKitEventLoopImplementation PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/AppKit/Application/EventLoopImplementationMacOS.mm")
target_include_directories(TestAppKitEventLoopImplementation PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/AppKit")
target_link_libraries(TestAppKitEventLoopImplementation PRIVATE "-framework Cocoa")

ladybird_test("TestClipboardTypes.mm" UI/AppKit NAME TestAppKitClipboardTypes LIBS LibWebView)
target_sources(TestAppKitClipboardTypes PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/AppKit/Application/ClipboardTypes.mm")
target_include_directories(TestAppKitClipboardTypes PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/AppKit")
target_link_libraries(TestAppKitClipboardTypes PRIVATE "-framework Cocoa")
