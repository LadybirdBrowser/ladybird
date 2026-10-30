ladybird_test_suite(LibWebCommon)

set(TEST_SOURCES
    TestCSSPixels.cpp
    TestGamepadSharedState.cpp
    TestGamepadStateBuffer.cpp
    TestInsecureRequestsPolicy.cpp
    TestKeyEvent.cpp
    TestPermissionSnapshot.cpp
    TestPermissionsPolicy.cpp
    TestProhibitsMixedSecurityContexts.cpp
    TestRemoteViolationDirective.cpp
    TestSiteCompatibility.cpp
    TestStorageEndpoint.cpp
    TestStorageKey.cpp
    TestStrings.cpp
    TestWebDriverInputResponse.cpp
)

target_sources(TestLibWebCommon PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibWebCommon PRIVATE LibHTTP LibIPC LibURL LibWebCommon)
