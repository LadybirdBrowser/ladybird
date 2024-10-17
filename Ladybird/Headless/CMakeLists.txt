set(SOURCES
    ${LADYBIRD_SOURCES}
    Application.cpp
    HeadlessWebView.cpp
    Test.cpp
    main.cpp
)

add_executable(headless-browser ${SOURCES})
target_include_directories(headless-browser PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(headless-browser PRIVATE ${LADYBIRD_SOURCE_DIR}/Userland/)
target_link_libraries(headless-browser PRIVATE ${LADYBIRD_LIBS} LibDiff)

if (BUILD_TESTING)

    # Find Python
    find_package(Python3 REQUIRED)

    # Fixture for starting the HTTP test server
    add_test(
        NAME SetupHTTPTestServer
        COMMAND ${Python3_EXECUTABLE} ${LADYBIRD_SOURCE_DIR}/Meta/http-test-server.py start -b -d ${LADYBIRD_SOURCE_DIR}/Tests
    )
    set_tests_properties(SetupHTTPTestServer PROPERTIES FIXTURES_SETUP HTTPTestServer)

    # Fixture for stopping the HTTP test server
    add_test(
        NAME CleanupHTTPTestServer
        COMMAND ${Python3_EXECUTABLE} ${LADYBIRD_SOURCE_DIR}/Meta/http-test-server.py stop
    )
    set_tests_properties(CleanupHTTPTestServer PROPERTIES FIXTURES_CLEANUP HTTPTestServer)

    # LibWeb test using the HTTPTestServer fixture
    add_test(
        NAME LibWeb
        COMMAND $<TARGET_FILE:headless-browser> --run-tests ${LADYBIRD_SOURCE_DIR}/Tests/LibWeb --dump-failed-ref-tests --force-fontconfig
    )
    set_tests_properties(LibWeb PROPERTIES FIXTURES_REQUIRED HTTPTestServer)

endif()
