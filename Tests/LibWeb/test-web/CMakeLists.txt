set(SOURCES
    Application.cpp
    Fixture.cpp
    Fuzzy.cpp
    TestWebView.cpp
    main.cpp
)

add_executable(test-web ${SOURCES})
add_dependencies(test-web ladybird_build_resource_files ImageDecoder RequestServer WebContent WebWorker)
target_link_libraries(test-web PRIVATE AK LibCore LibDiff LibFileSystem LibGfx LibImageDecoderClient LibIPC LibJS LibMain LibRequests LibURL LibWeb LibWebView)

if (APPLE)
    target_compile_definitions(test-web PRIVATE LADYBIRD_BINARY_PATH="$<TARGET_FILE_DIR:ladybird>")
endif()

if (BUILD_TESTING)
    find_package(Python3 REQUIRED)

    add_test(
        NAME LibWeb
        COMMAND $<TARGET_FILE:test-web> --python-executable ${Python3_EXECUTABLE} --dump-failed-ref-tests --per-test-timeout 120 --verbose
    )

    set_tests_properties(LibWeb PROPERTIES
        ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_PROJECT_ROOT}
        TIMEOUT_SIGNAL_NAME SIGTERM)
endif()
