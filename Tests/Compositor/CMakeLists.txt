set(TEST_SOURCES
    TestAsyncScrollTree.cpp
    TestContextState.cpp
    TestScrollbarController.cpp
    TestVSyncScheduler.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" Compositor LIBS compositorservice LibCompositing LibGfx LibWeb)
endforeach()

if (NOT WIN32)
    ladybird_test("TestTransportPlatform.cpp" Compositor LIBS compositorservice LibCompositing LibGfx LibWeb LibWebView)
endif()
