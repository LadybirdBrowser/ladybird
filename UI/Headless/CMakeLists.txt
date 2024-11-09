set(SOURCES
    ${LADYBIRD_SOURCES}
    Application.cpp
    Fixture.cpp
    HeadlessWebView.cpp
    Test.cpp
    main.cpp
)

add_executable(headless-browser ${SOURCES})
target_include_directories(headless-browser PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(headless-browser PRIVATE ${LADYBIRD_SOURCE_DIR})
target_link_libraries(headless-browser PRIVATE ${LADYBIRD_LIBS} LibDiff)

if (BUILD_TESTING)
    find_package(Python3 REQUIRED)
    add_test(
        NAME LibWeb
        COMMAND $<TARGET_FILE:headless-browser> --run-tests ${LADYBIRD_SOURCE_DIR}/Tests/LibWeb --python-executable ${Python3_EXECUTABLE} --dump-failed-ref-tests --per-test-timeout 120
    )
endif()
