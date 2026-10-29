if (BUILD_TESTING AND NOT WIN32)
    if (APPLE OR LINUX)
        add_test(
            NAME TestWebDriverCrashReport
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-crash-report.py $<TARGET_FILE:WebDriver>
        )
        set_tests_properties(TestWebDriverCrashReport PROPERTIES TIMEOUT 120)
        add_test(
            NAME TestWebDriverBrowserCrashRecovery
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-browser-crash-recovery.py
                $<TARGET_FILE:WebDriver>
        )
        set_tests_properties(TestWebDriverBrowserCrashRecovery PROPERTIES TIMEOUT 120)
        set(crash_report_helpers WebWorker RequestServer ImageDecoder Compositor)
        foreach(helper IN LISTS crash_report_helpers)
            add_test(
                NAME Test${helper}CrashReport
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-crash-report.py $<TARGET_FILE:WebDriver> --process ${helper}
            )
            set_tests_properties(Test${helper}CrashReport PROPERTIES TIMEOUT 120)
        endforeach()
        add_test(
            NAME TestWebDriverWorkerTerminatedWhileStarting
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-worker-terminated-while-starting.py $<TARGET_FILE:WebDriver>
        )
        set_tests_properties(TestWebDriverWorkerTerminatedWhileStarting PROPERTIES TIMEOUT 120)
        add_test(
            NAME TestWebDriverCompositorReconnect
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-compositor-reconnect.py $<TARGET_FILE:WebDriver>
        )
        set_tests_properties(TestWebDriverCompositorReconnect PROPERTIES TIMEOUT 120)
        add_test(
            NAME TestDevToolsClientFollowsTheTab
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-devtools-client-follows-the-tab.py $<TARGET_FILE:ladybird>
        )
        set_tests_properties(TestDevToolsClientFollowsTheTab PROPERTIES TIMEOUT 120)
        add_test(
            NAME TestDevToolsCatchUpLayout
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-devtools-catch-up-layout.py $<TARGET_FILE:ladybird>
        )
        set_tests_properties(TestDevToolsCatchUpLayout PROPERTIES TIMEOUT 120)
        add_test(
            NAME TestWorkerErrorConsole
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-worker-error-console.py $<TARGET_FILE:ladybird>
        )
        set_tests_properties(TestWorkerErrorConsole PROPERTIES TIMEOUT 120)
        add_test(
            NAME TestWindowSizeReachesEveryPage
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-window-size-reaches-every-page.py $<TARGET_FILE:ladybird>
        )
        set_tests_properties(TestWindowSizeReachesEveryPage PROPERTIES TIMEOUT 120)
        add_test(
            NAME TestUserAgentPresetReachesChildFrames
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-user-agent-preset-reaches-child-frames.py $<TARGET_FILE:ladybird>
        )
        set_tests_properties(TestUserAgentPresetReachesChildFrames PROPERTIES TIMEOUT 120)
        add_test(
            NAME TestMessagePortCollection
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-message-port-collection.py $<TARGET_FILE:WebDriver>
        )
        set_tests_properties(TestMessagePortCollection PROPERTIES TIMEOUT 120)
        add_test(
            NAME TestSharedStyleSheetMemory
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-shared-stylesheet-memory.py $<TARGET_FILE:WebDriver>
        )
        set_tests_properties(TestSharedStyleSheetMemory PROPERTIES TIMEOUT 180)
        add_test(
            NAME TestBytecodeCacheBlobIsFileBacked
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-bytecode-cache-blob-is-file-backed.py $<TARGET_FILE:WebDriver>
        )
        set_tests_properties(TestBytecodeCacheBlobIsFileBacked PROPERTIES TIMEOUT 120)
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
        NAME TestWebDriverLocationPolicy
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-location-policy.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverLocationPolicy PROPERTIES TIMEOUT 120)

    add_test(
        NAME TestWebDriverPermissionsSettings
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-permissions-settings.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverPermissionsSettings PROPERTIES TIMEOUT 120)

    add_test(
        NAME TestWebDriverSetPermission
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-set-permission.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverSetPermission PROPERTIES TIMEOUT 120)

    add_test(
        NAME TestWebDriverSearchEngineSettings
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-search-engine-settings.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverSearchEngineSettings PROPERTIES TIMEOUT 120)

    add_test(
        NAME TestWebDriverSettingsChanges
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-settings-changes.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverSettingsChanges PROPERTIES TIMEOUT 120)

    add_test(
        NAME TestWebDriverWebUIPagesShareAProcess
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-web-ui-pages-share-a-process.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverWebUIPagesShareAProcess PROPERTIES TIMEOUT 120)

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
        NAME TestWebDriverForceDark
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-force-dark.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverForceDark PROPERTIES TIMEOUT 120)

    add_test(
        NAME TestWebDriverBiDi
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-bidi.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverBiDi PROPERTIES TIMEOUT 120)

    add_test(
        NAME TestWebDriverErrorKeepAlive
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-error-keep-alive.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverErrorKeepAlive PROPERTIES TIMEOUT 120)
endif()

if (BUILD_BROWSER_TESTS_ONLY)
    return()
endif()

ladybird_test_suite(LibWebView)

set(TEST_SOURCES
    TestAccessibilityTreeManager.cpp
    TestAudibilityHold.cpp
    TestAudioServerAudibility.cpp
    TestAutocompleteMuxer.cpp
    TestAutocompleteRanker.cpp
    TestBackgroundNetworking.cpp
    TestCanonicalBrowsingContextGroup.cpp
    TestCookieJar.cpp
    TestCrashReportDiagnostics.cpp
    TestDownloadSegmentation.cpp
    TestDownloadStore.cpp
    TestExternalURLHandler.cpp
    TestFaviconStore.cpp
    TestHelperProcessEnvironment.cpp
    TestHistoryStore.cpp
    TestHSTSStore.cpp
    TestNavigationLoader.cpp
    TestOmnibox.cpp
    TestPermissionManager.cpp
    TestPictureInPictureWindow.cpp
    TestProfile.cpp
    TestSessionHistory.cpp
    TestSessionHistorySnapshotStorage.cpp
    TestSessionHistoryTraversalQueue.cpp
    TestSessionStore.cpp
    TestSiteCompatibilityDataLoading.cpp
    TestStartupSettings.cpp
    TestStorageAreaRegistry.cpp
    TestStorageJar.cpp
    TestTabPerformanceStats.cpp
    TestWebDriverBiDiNetwork.cpp
    TestWebViewURL.cpp
)

if (APPLE OR LINUX)
    list(APPEND TEST_SOURCES
        TestCrashReport.cpp
        TestCrashReportReview.cpp
        TestCrashReportStore.cpp
        TestFontService.cpp
        TestRendererFontService.cpp
    )
endif()

if (APPLE)
    list(APPEND TEST_SOURCES TestProcessReaper.cpp)
endif()

if (LINUX)
    list(APPEND TEST_SOURCES TestAudioDaemonSocket.cpp)
endif()

if (APPLE AND NOT IOS)
    target_link_libraries(TestLibWebView PRIVATE LibWebSocket OpenSSL::Crypto OpenSSL::SSL)
    target_sources(TestLibWebView PRIVATE
        TestWebDriverMachBootstrap.cpp
        "${LADYBIRD_SOURCE_DIR}/Services/WebDriver/BiDiSession.cpp"
        "${LADYBIRD_SOURCE_DIR}/Services/WebDriver/BrowserConnection.cpp"
        "${LADYBIRD_SOURCE_DIR}/Services/WebDriver/Client.cpp"
        "${LADYBIRD_SOURCE_DIR}/Services/WebDriver/Session.cpp"
        "${LADYBIRD_SOURCE_DIR}/Services/WebDriver/WebSocket.cpp"
    )
    target_include_directories(TestLibWebView PRIVATE "${LADYBIRD_SOURCE_DIR}/Services" "${CMAKE_BINARY_DIR}/Services")
    set_tests_properties(TestLibWebView PROPERTIES TIMEOUT 120)
endif()

if (NOT WIN32)
    set(HEADLESS_TEST_SOURCES
        TestApplicationDestruction.cpp
        TestAutocomplete.cpp
        TestBeforeUnload.cpp
        TestBrowserHistoryTraversal.cpp
        TestCompositorInputRouting.cpp
        TestCrashOverlay.cpp
        TestCrashReportSubmission.cpp
        TestFavicon.cpp
        TestFileDownloader.cpp
        TestImageDecoderIsolation.cpp
        TestInputMethodState.cpp
        TestLinkHoverStatus.cpp
        TestNavigationDownloads.cpp
        TestPictureInPictureUserActivation.cpp
        TestPictureInPictureWindowControls.cpp
        TestPictureInPictureWindowLifetime.cpp
        TestRendererCookieAccess.cpp
        TestRendererOriginClaims.cpp
        TestRendererViolationReports.cpp
        TestRequestServerClientSessions.cpp
        TestShadowTreeTooltipsAndLinks.cpp
        TestTabPerformanceMonitor.cpp
        TestUserActivation.cpp
        TestWebCompatReport.cpp
        TestWebCompatReportPage.cpp
    )
    list(APPEND TEST_SOURCES ${HEADLESS_TEST_SOURCES})
endif()

target_sources(TestLibWebView PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibWebView PRIVATE LibAudio LibCompositing LibDatabase LibGfx LibHTTP LibIPC LibRequests LibThreading LibURL LibWeb LibWebCommon LibWebView)

if (APPLE)
    add_dependencies(TestLibWebView ProcessReaper)
endif()

if (LINUX)
    include(audio)
    if (LADYBIRD_AUDIO_BACKEND STREQUAL "PULSE")
        set_property(SOURCE TestAudioDaemonSocket.cpp APPEND PROPERTY COMPILE_DEFINITIONS HAVE_PULSEAUDIO=1)
    endif()
endif()

if ((APPLE OR LINUX) AND cpptrace_FOUND AND LADYBIRD_ENABLE_CPPTRACE)
    set_property(SOURCE TestCrashReport.cpp APPEND PROPERTY COMPILE_DEFINITIONS TEST_HAS_CPPTRACE=1)
endif()

if (LINUX AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    add_library(CrashReportLateLibrary SHARED CrashReportLateLibrary.cpp)
    add_dependencies(TestLibWebView CrashReportLateLibrary)
    set_property(SOURCE TestCrashReport.cpp APPEND PROPERTY COMPILE_DEFINITIONS TEST_CRASH_REPORT_LATE_LIBRARY="$<TARGET_FILE:CrashReportLateLibrary>")
    target_link_libraries(TestLibWebView PRIVATE ${CMAKE_DL_LIBS})
endif()

if (APPLE OR LINUX)
    foreach(panic_strategy abort unwind)
        set(rust_test_library "${CMAKE_CURRENT_BINARY_DIR}/librust_crash_report_${panic_strategy}.a")
        set(ARG_CRATE_NAME "rust_crash_report_${panic_strategy}")
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
        if (LINUX AND panic_strategy STREQUAL "unwind")
            # Keep the fixture's Rust standard library private to the test executable. Exported, it would take the
            # place of the copies in Ladybird's shared libraries, which then install their panic hooks into it.
            get_filename_component(rust_test_library_name "${rust_test_library}" NAME)
            target_link_options(TestRustCrashReport_${panic_strategy} PRIVATE "LINKER:--exclude-libs,${rust_test_library_name}")
        endif()
        set_tests_properties(TestRustCrashReport_${panic_strategy} PROPERTIES TIMEOUT 60)
        add_dependencies(TestRustCrashReport_${panic_strategy} ${ARG_CRATE_NAME}_build)
        if (panic_strategy STREQUAL "unwind")
            target_compile_definitions(TestRustCrashReport_${panic_strategy} PRIVATE TEST_RUST_UNWIND=1)
        endif()
        if (cpptrace_FOUND AND LADYBIRD_ENABLE_CPPTRACE)
            target_compile_definitions(TestRustCrashReport_${panic_strategy} PRIVATE TEST_HAS_CPPTRACE=1)
        endif()
    endforeach()
endif()


if (NOT WIN32)
    target_sources(TestLibWebView PRIVATE HeadlessTest.cpp)
    target_link_libraries(TestLibWebView PRIVATE LibCrypto LibDiff LibImageDecoders LibImageDecoderClient LibJS)
    add_dependencies(TestLibWebView ladybird_build_resource_files ${ladybird_helper_processes})
    set_tests_properties(TestLibWebView PROPERTIES TIMEOUT 1800 ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})
    if (APPLE)
        target_compile_definitions(TestLibWebView PRIVATE LADYBIRD_BINARY_PATH="$<TARGET_FILE_DIR:ladybird>")
    endif()
endif()

if (BUILD_TESTING AND NOT WIN32)
    add_test(NAME TestBuildInformation COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-build-information.py)
    add_test(
        NAME TestWebDriverBiDiCleanup
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-bidi-cleanup.py
    )
    set_tests_properties(TestWebDriverBiDiCleanup PROPERTIES TIMEOUT 30)
endif()
