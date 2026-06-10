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

# FIXME: Add support for running test-web on Windows
if (WIN32)
    return()
endif()

if (BUILD_TESTING)
    add_test(
        NAME LibWeb
        COMMAND $<TARGET_FILE:test-web> --python-executable ${Python3_EXECUTABLE} --per-test-timeout 120 -v -v
    )

    set_tests_properties(LibWeb PROPERTIES
        ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR}
        TIMEOUT_SIGNAL_NAME SIGTERM)
endif()
