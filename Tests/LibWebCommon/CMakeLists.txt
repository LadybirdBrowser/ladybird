set(TEST_SOURCES
    TestCSSPixels.cpp
    TestGamepadSharedState.cpp
    TestGamepadStateBuffer.cpp
    TestSiteCompatibility.cpp
    TestStrings.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibWebCommon LIBS LibWebCommon)
endforeach()

target_link_libraries(TestSiteCompatibility PRIVATE LibURL)
target_link_libraries(TestGamepadStateBuffer PRIVATE LibCore LibIPC)
