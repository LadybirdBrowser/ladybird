set(SOURCES
    ${LADYBIRD_SOURCES}
    main.cpp
)

add_executable(headless-browser ${SOURCES})
target_include_directories(headless-browser PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(headless-browser PRIVATE ${LADYBIRD_SOURCE_DIR}/Userland/)
target_link_libraries(headless-browser PRIVATE ${LADYBIRD_LIBS} LibDiff)

if (BUILD_TESTING)
    add_test(
        NAME LibWeb
        COMMAND $<TARGET_FILE:headless-browser> --run-tests ${LADYBIRD_SOURCE_DIR}/Tests/LibWeb --dump-failed-ref-tests --force-fontconfig
    )
endif()
