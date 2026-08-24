set(TEST_SOURCES
    TestContextState.cpp
    TestViewportScrollbarController.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" Compositor LIBS compositorservice LibGfx LibWeb)
endforeach()
