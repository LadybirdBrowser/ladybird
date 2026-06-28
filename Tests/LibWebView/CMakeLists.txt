set(TEST_SOURCES
    TestCookieJar.cpp
    TestHistoryStore.cpp
    TestHSTSStore.cpp
    TestSessionHistory.cpp
    TestStorageJar.cpp
    TestWebViewURL.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibWebView LIBS LibDatabase LibWebView LibURL)
endforeach()

if (NOT WIN32)
    add_executable(TestAutocomplete TestAutocomplete.cpp)
    add_dependencies(TestAutocomplete ${ladybird_helper_processes})
    target_link_libraries(TestAutocomplete PRIVATE AK LibCore LibDiff LibFileSystem LibGfx LibImageDecoders LibImageDecoderClient LibIPC LibJS LibMain LibRequests LibURL LibWeb LibWebView)
    if (APPLE)
        target_compile_definitions(TestAutocomplete PRIVATE LADYBIRD_BINARY_PATH="$<TARGET_FILE_DIR:ladybird>")
    endif()
endif()

if (BUILD_TESTING AND NOT WIN32)
    add_test(
        NAME TestWebDriverSessionHistory
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-session-history.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverSessionHistory PROPERTIES TIMEOUT 120)

    add_test(NAME TestAutocomplete COMMAND $<TARGET_FILE:TestAutocomplete>)
    set_tests_properties(TestAutocomplete PROPERTIES TIMEOUT 60 ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})
endif()
