include(python_environment)

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
target_compile_definitions(test-web PRIVATE HAS_PDFJS=$<BOOL:${PDFJS_FOUND}>)
target_compile_definitions(test-web PRIVATE LADYBIRD_PYTHON_EXECUTABLE="${LADYBIRD_TEST_PYTHON}")
add_dependencies(test-web ladybird_build_resource_files ladybird_python_environment ${ladybird_helper_processes})
target_link_libraries(test-web PRIVATE AK LibCore LibDiff LibFileSystem LibGfx LibImageDecoders LibImageDecoderClient LibIPC LibJS LibMain LibRequests LibURL LibWeb LibWebView)

if (APPLE)
    target_compile_definitions(test-web PRIVATE LADYBIRD_BINARY_PATH="$<TARGET_FILE_DIR:ladybird>")
elseif (WIN32)
    target_link_libraries(test-web PRIVATE LibDevTools)
    target_include_directories(test-web PRIVATE $<BUILD_INTERFACE:${PTHREAD_INCLUDE_DIR}>)
    ladybird_windows_bin(test-web CONSOLE)
endif()

if (BUILD_TESTING)
    ladybird_python_test(test-history-rate-limit-reset.py
        PYTHON_EXECUTABLE "${LADYBIRD_TEST_PYTHON}" ARGS $<TARGET_FILE:test-web>)
    ladybird_python_test(test-manual-rendering-reset.py
        PYTHON_EXECUTABLE "${LADYBIRD_TEST_PYTHON}" ARGS $<TARGET_FILE:test-web> TIMEOUT 240)

    add_test(
        NAME LibWeb
        COMMAND $<TARGET_FILE:test-web> --per-test-timeout 120 -v -v
    )

    add_test(
        NAME LibWebUIProcessSessionHistory
        COMMAND $<TARGET_FILE:test-web> --per-test-timeout 120
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
