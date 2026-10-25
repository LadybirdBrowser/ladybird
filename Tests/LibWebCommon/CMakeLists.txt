set(TEST_SOURCES
    TestCSSPixels.cpp
    TestGamepadSharedState.cpp
    TestGamepadStateBuffer.cpp
    TestKeyEvent.cpp
    TestPermissionsPolicy.cpp
    TestSiteCompatibility.cpp
    TestStorageEndpoint.cpp
    TestStorageKey.cpp
    TestStrings.cpp
    TestWebDriverInputResponse.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibWebCommon LIBS LibWebCommon)
endforeach()

target_link_libraries(TestSiteCompatibility PRIVATE LibURL)
target_link_libraries(TestGamepadStateBuffer PRIVATE LibCore LibIPC)
target_link_libraries(TestKeyEvent PRIVATE LibIPC)
target_link_libraries(TestPermissionsPolicy PRIVATE LibHTTP LibIPC LibURL)
target_link_libraries(TestStorageEndpoint PRIVATE LibIPC)
target_link_libraries(TestStorageKey PRIVATE LibURL)
