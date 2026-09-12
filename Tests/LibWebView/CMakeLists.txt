set(TEST_SOURCES
    TestApplyHistoryStep.cpp
    TestBackgroundNetworking.cpp
    TestAutocompleteMuxer.cpp
    TestAutocompleteRanker.cpp
    TestCanonicalBrowsingContextGroup.cpp
    TestCookieJar.cpp
    TestDownloadSegmentation.cpp
    TestDownloadStore.cpp
    TestExternalURLHandler.cpp
    TestFaviconStore.cpp
    TestHistoryStore.cpp
    TestHSTSStore.cpp
    TestNavigationLoader.cpp
    TestOmnibox.cpp
    TestPausedDebuggerOverlay.cpp
    TestProfile.cpp
    TestSessionHistory.cpp
    TestSessionHistorySnapshotStorage.cpp
    TestSessionHistoryTraversalQueue.cpp
    TestSessionStore.cpp
    TestSiteCompatibilityDataLoading.cpp
    TestStorageJar.cpp
    TestTabPerformanceStats.cpp
    TestWebViewURL.cpp
)

if (APPLE OR LINUX)
    list(APPEND TEST_SOURCES TestCrashReport.cpp)
endif()

foreach(source IN LISTS TEST_SOURCES)
    set(test_libraries LibDatabase LibWebView LibURL)
    if (source STREQUAL "TestProfile.cpp" OR source STREQUAL "TestDownloadSegmentation.cpp")
        list(APPEND test_libraries LibHTTP)
    endif()
    ladybird_test("${source}" LibWebView LIBS ${test_libraries})
endforeach()

if (TARGET TestCrashReport AND cpptrace_FOUND AND LADYBIRD_ENABLE_CPPTRACE)
    target_compile_definitions(TestCrashReport PRIVATE TEST_HAS_CPPTRACE=1)
endif()

if (APPLE OR LINUX)
    foreach(panic_strategy abort unwind)
        set(rust_test_library "${CMAKE_CURRENT_BINARY_DIR}/librust_crash_report_${panic_strategy}.a")
        set(ARG_CRATE_NAME "rust_crash_report_${panic_strategy}")
        configure_file("${LADYBIRD_SOURCE_DIR}/Meta/CMake/RustPanicInit.cpp.in"
            "${CMAKE_CURRENT_BINARY_DIR}/${ARG_CRATE_NAME}_init.cpp" @ONLY)
        add_custom_command(OUTPUT "${rust_test_library}"
            COMMAND ${CMAKE_COMMAND} -E env "CARGO_PKG_NAME=${ARG_CRATE_NAME}"
                ${RUST_RUSTC} --edition=2024 --crate-type=staticlib --crate-name=${ARG_CRATE_NAME}
                -Cpanic=${panic_strategy} -Cdebuginfo=1
                "${CMAKE_CURRENT_SOURCE_DIR}/RustCrashReport.rs" -o "${rust_test_library}"
            DEPENDS RustCrashReport.rs "${LADYBIRD_SOURCE_DIR}/Libraries/RustPanic.rs"
            VERBATIM)
        add_custom_target(${ARG_CRATE_NAME}_build DEPENDS "${rust_test_library}")
        ladybird_test(TestRustCrashReport.cpp LibWebView NAME TestRustCrashReport_${panic_strategy}
            LIBS LibWebView "${rust_test_library}")
        set_tests_properties(TestRustCrashReport_${panic_strategy} PROPERTIES TIMEOUT 60)
        add_dependencies(TestRustCrashReport_${panic_strategy} ${ARG_CRATE_NAME}_build)
        target_sources(TestRustCrashReport_${panic_strategy} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/${ARG_CRATE_NAME}_init.cpp")
        if (panic_strategy STREQUAL "unwind")
            target_compile_definitions(TestRustCrashReport_${panic_strategy} PRIVATE TEST_RUST_UNWIND=1)
        endif()
    endforeach()
endif()

target_sources(TestNavigationLoader PRIVATE ${LADYBIRD_SOURCE_DIR}/Libraries/LibWebView/NavigationLoader.cpp)
target_link_libraries(TestNavigationLoader PRIVATE LibRequests LibWeb)

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

    add_executable(TestCrashOverlay TestCrashOverlay.cpp)
    add_dependencies(TestCrashOverlay ladybird_build_resource_files ${ladybird_helper_processes})
    target_link_libraries(TestCrashOverlay PRIVATE AK LibCore LibDiff LibFileSystem LibGfx LibImageDecoders LibImageDecoderClient LibIPC LibJS LibMain LibRequests LibURL LibWeb LibWebView)
    if (APPLE)
        target_compile_definitions(TestCrashOverlay PRIVATE LADYBIRD_BINARY_PATH="$<TARGET_FILE_DIR:ladybird>")
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
    add_executable(TestTabPerformanceMonitor TestTabPerformanceMonitor.cpp)
    add_dependencies(TestTabPerformanceMonitor ladybird_build_resource_files ${ladybird_helper_processes})
    target_link_libraries(TestTabPerformanceMonitor PRIVATE AK LibCore LibDiff LibFileSystem LibGfx LibImageDecoders LibImageDecoderClient LibIPC LibJS LibMain LibRequests LibURL LibWeb LibWebView)
    if (APPLE)
        target_compile_definitions(TestTabPerformanceMonitor PRIVATE LADYBIRD_BINARY_PATH="$<TARGET_FILE_DIR:ladybird>")
    endif()
endif()

if (BUILD_TESTING AND NOT WIN32)
    add_test(NAME TestBuildInformation COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-build-information.py)
    if (APPLE OR LINUX)
        add_test(
            NAME TestWebDriverCrashReport
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-crash-report.py $<TARGET_FILE:WebDriver>
        )
        set_tests_properties(TestWebDriverCrashReport PROPERTIES TIMEOUT 120)
        set(crash_report_helpers WebWorker RequestServer ImageDecoder Compositor)
        if (ENABLE_CRANELIFT_JIT)
            list(APPEND crash_report_helpers WasmCompiler)
        endif()
        foreach(helper IN LISTS crash_report_helpers)
            add_test(
                NAME Test${helper}CrashReport
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-crash-report.py $<TARGET_FILE:WebDriver> --process ${helper}
            )
            set_tests_properties(Test${helper}CrashReport PROPERTIES TIMEOUT 120)
        endforeach()
    endif()

    add_test(
        NAME TestWebDriverDeleteSession
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-delete-session.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverDeleteSession PROPERTIES TIMEOUT 120)

    add_test(
        NAME TestWebDriverContentBlockers
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-content-blockers.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverContentBlockers PROPERTIES TIMEOUT 180)

    add_test(
        NAME TestWebDriverSessionHistory
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-session-history.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverSessionHistory PROPERTIES TIMEOUT 120 ENVIRONMENT "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/session-history-data")

    add_test(
        NAME TestWebDriverSiteZoom
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-site-zoom.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverSiteZoom PROPERTIES TIMEOUT 120)

    add_test(
        NAME TestWebDriverSiteZoomWithSiteIsolation
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-site-zoom.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverSiteZoomWithSiteIsolation PROPERTIES TIMEOUT 120 ENVIRONMENT "LADYBIRD_WEBDRIVER_ENABLE_SITE_ISOLATION=1")

    add_test(
        NAME TestWebDriverErrorKeepAlive
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-error-keep-alive.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverErrorKeepAlive PROPERTIES TIMEOUT 120)

    add_test(NAME TestAutocomplete COMMAND $<TARGET_FILE:TestAutocomplete>)
    set_tests_properties(TestAutocomplete PROPERTIES TIMEOUT 60 ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})

    add_test(NAME TestFileDownloader COMMAND $<TARGET_FILE:TestFileDownloader>)
    set_tests_properties(TestFileDownloader PROPERTIES TIMEOUT 120 ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})

    add_test(NAME TestCrashOverlay COMMAND $<TARGET_FILE:TestCrashOverlay>)
    set_tests_properties(TestCrashOverlay PROPERTIES TIMEOUT 60 ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})

    add_test(NAME TestBrowserHistoryTraversal COMMAND $<TARGET_FILE:TestBrowserHistoryTraversal>)
    set_tests_properties(TestBrowserHistoryTraversal PROPERTIES TIMEOUT 60 ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})

    add_test(NAME TestInputMethodState COMMAND $<TARGET_FILE:TestInputMethodState>)
    set_tests_properties(TestInputMethodState PROPERTIES TIMEOUT 60 ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})
    add_test(NAME TestTabPerformanceMonitor COMMAND $<TARGET_FILE:TestTabPerformanceMonitor>)
    set_tests_properties(TestTabPerformanceMonitor PROPERTIES TIMEOUT 60 ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})
endif()
