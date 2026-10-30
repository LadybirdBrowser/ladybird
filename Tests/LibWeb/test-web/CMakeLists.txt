set(SOURCES
    Application.cpp
    CaptureFile.cpp
    Collection.cpp
    Debug.cpp
    Display.cpp
    Fixture.cpp
    Fuzzy.cpp
    TestRunCapture.cpp
    TestWebView.cpp
    Variants.cpp
    main.cpp
)

add_executable(test-web ${SOURCES})
add_dependencies(test-web ladybird_build_resource_files ${ladybird_helper_processes})
target_link_libraries(test-web PRIVATE AK LibCore LibDiff LibFileSystem LibGfx LibImageDecoders LibImageDecoderClient LibIPC LibJS LibMain LibRequests LibURL LibWeb LibWebView)

if (APPLE)
    target_compile_definitions(test-web PRIVATE LADYBIRD_BINARY_PATH="$<TARGET_FILE_DIR:ladybird>")
elseif (WIN32)
    target_link_libraries(test-web PRIVATE LibDevTools)
    target_include_directories(test-web PRIVATE $<BUILD_INTERFACE:${PTHREAD_INCLUDE_DIR}>)
    ladybird_windows_bin(test-web CONSOLE)
endif()

if (BUILD_TESTING)
    add_test(
        NAME LibWebManualRenderingReset
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-manual-rendering-reset.py
            $<TARGET_FILE:test-web> ${LADYBIRD_SOURCE_DIR}
    )
    set_tests_properties(LibWebManualRenderingReset PROPERTIES TIMEOUT 240)

    add_test(
        NAME LibWebHistoryRateLimitReset
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-history-rate-limit-reset.py
            $<TARGET_FILE:test-web> ${LADYBIRD_SOURCE_DIR}
    )
    set_tests_properties(LibWebHistoryRateLimitReset PROPERTIES TIMEOUT 120)

    add_test(
        NAME LibWebPreferredColorSchemeReset
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-preferred-color-scheme-reset.py
            $<TARGET_FILE:test-web> ${LADYBIRD_SOURCE_DIR}
    )
    set_tests_properties(LibWebPreferredColorSchemeReset PROPERTIES TIMEOUT 120)

    add_test(
        NAME LibWeb
        COMMAND $<TARGET_FILE:test-web> --python-executable ${Python3_EXECUTABLE} --per-test-timeout 120 -v -v
    )

    add_test(
        NAME LibWebUIProcessSessionHistory
        COMMAND $<TARGET_FILE:test-web> --python-executable ${Python3_EXECUTABLE} --per-test-timeout 120
            --force-new-process --run-ui-process-session-history-tests -f "Text/input/navigation/*"
            --results-dir test-dumps/ui-process-session-history-results
    )

    set_tests_properties(LibWeb LibWebUIProcessSessionHistory PROPERTIES
        ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR}
        RUN_SERIAL TRUE)
    # CTest refuses to run tests with a timeout signal on Windows.
    if (NOT WIN32)
        set_tests_properties(LibWeb LibWebUIProcessSessionHistory PROPERTIES TIMEOUT_SIGNAL_NAME SIGTERM)
    endif()
endif()
