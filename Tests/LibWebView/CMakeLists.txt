set(TEST_SOURCES
    TestApplyHistoryStep.cpp
    TestAutocompleteMuxer.cpp
    TestAutocompleteRanker.cpp
    TestCookieJar.cpp
    TestDownloadSegmentation.cpp
    TestDownloadStore.cpp
    TestExternalURLHandler.cpp
    TestFaviconStore.cpp
    TestHistoryStore.cpp
    TestHSTSStore.cpp
    TestOmnibox.cpp
    TestProfile.cpp
    TestSessionHistory.cpp
    TestSessionHistorySnapshotStorage.cpp
    TestSessionHistoryTraversalQueue.cpp
    TestSessionStore.cpp
    TestStorageJar.cpp
    TestWebViewURL.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    set(test_libraries LibDatabase LibWebView LibURL)
    if (source STREQUAL "TestProfile.cpp" OR source STREQUAL "TestDownloadSegmentation.cpp")
        list(APPEND test_libraries LibHTTP)
    endif()
    ladybird_test("${source}" LibWebView LIBS ${test_libraries})
endforeach()

if (NOT WIN32)
    add_executable(TestAutocomplete TestAutocomplete.cpp)
    add_dependencies(TestAutocomplete ${ladybird_helper_processes})
    target_link_libraries(TestAutocomplete PRIVATE AK LibCore LibDiff LibFileSystem LibGfx LibImageDecoders LibImageDecoderClient LibIPC LibJS LibMain LibRequests LibURL LibWeb LibWebView)
    if (APPLE)
        target_compile_definitions(TestAutocomplete PRIVATE LADYBIRD_BINARY_PATH="$<TARGET_FILE_DIR:ladybird>")
    endif()

    add_executable(TestFileDownloader TestFileDownloader.cpp)
    add_dependencies(TestFileDownloader ${ladybird_helper_processes})
    target_link_libraries(TestFileDownloader PRIVATE AK LibCore LibDiff LibFileSystem LibGfx LibImageDecoders LibImageDecoderClient LibIPC LibJS LibMain LibRequests LibSync LibThreading LibURL LibWeb LibWebView)
    if (APPLE)
        target_compile_definitions(TestFileDownloader PRIVATE LADYBIRD_BINARY_PATH="$<TARGET_FILE_DIR:ladybird>")
    endif()

    add_executable(TestBrowserHistoryTraversal TestBrowserHistoryTraversal.cpp)
    add_dependencies(TestBrowserHistoryTraversal ladybird_build_resource_files ${ladybird_helper_processes})
    target_link_libraries(TestBrowserHistoryTraversal PRIVATE AK LibCore LibDiff LibFileSystem LibGfx LibImageDecoders LibImageDecoderClient LibIPC LibJS LibMain LibRequests LibURL LibWeb LibWebView)
    if (APPLE)
        target_compile_definitions(TestBrowserHistoryTraversal PRIVATE LADYBIRD_BINARY_PATH="$<TARGET_FILE_DIR:ladybird>")
    endif()

    add_executable(TestInputMethodState TestInputMethodState.cpp)
    add_dependencies(TestInputMethodState ladybird_build_resource_files ${ladybird_helper_processes})
    target_link_libraries(TestInputMethodState PRIVATE AK LibCore LibDiff LibFileSystem LibGfx LibImageDecoders LibImageDecoderClient LibIPC LibJS LibMain LibRequests LibURL LibWeb LibWebView)
    if (APPLE)
        target_compile_definitions(TestInputMethodState PRIVATE LADYBIRD_BINARY_PATH="$<TARGET_FILE_DIR:ladybird>")
    endif()
endif()

if (BUILD_TESTING AND NOT WIN32)
    add_test(
        NAME TestWebDriverDeleteSession
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-delete-session.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverDeleteSession PROPERTIES TIMEOUT 120)

    add_test(
        NAME TestWebDriverSessionHistory
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-session-history.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverSessionHistory PROPERTIES TIMEOUT 120)

    add_test(NAME TestAutocomplete COMMAND $<TARGET_FILE:TestAutocomplete>)
    set_tests_properties(TestAutocomplete PROPERTIES TIMEOUT 60 ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})

    add_test(NAME TestFileDownloader COMMAND $<TARGET_FILE:TestFileDownloader>)
    set_tests_properties(TestFileDownloader PROPERTIES TIMEOUT 120 ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})

    add_test(NAME TestBrowserHistoryTraversal COMMAND $<TARGET_FILE:TestBrowserHistoryTraversal>)
    set_tests_properties(TestBrowserHistoryTraversal PROPERTIES TIMEOUT 60 ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})

    add_test(NAME TestInputMethodState COMMAND $<TARGET_FILE:TestInputMethodState>)
    set_tests_properties(TestInputMethodState PROPERTIES TIMEOUT 60 ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})
endif()
