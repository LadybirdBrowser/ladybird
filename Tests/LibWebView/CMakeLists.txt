set(TEST_SOURCES
    TestHistoryStore.cpp
    TestHSTSStore.cpp
    TestSessionHistory.cpp
    TestWebViewURL.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibWebView LIBS LibDatabase LibWebView LibURL)
endforeach()

if (BUILD_TESTING AND NOT WIN32)
    add_test(
        NAME TestWebDriverSessionHistory
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-session-history.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverSessionHistory PROPERTIES TIMEOUT 120)
endif()
