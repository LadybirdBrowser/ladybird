set(SOURCES
    Client.cpp
    Session.cpp
    WebContentConnection.cpp
    main.cpp
)

add_executable(WebDriver ${SOURCES})

target_include_directories(WebDriver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_include_directories(WebDriver PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(WebDriver PRIVATE ${LADYBIRD_SOURCE_DIR})
target_include_directories(WebDriver PRIVATE ${LADYBIRD_SOURCE_DIR}/Services)

target_link_libraries(WebDriver PRIVATE LibCore LibFileSystem LibGfx LibIPC LibJS LibMain LibWeb LibWebSocket LibWebView)
target_link_libraries(WebDriver PRIVATE OpenSSL::Crypto OpenSSL::SSL)

if (WIN32)
    lagom_subsystem_windows(WebDriver)
    add_custom_command(TARGET WebDriver POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -E copy_if_different
            $<TARGET_RUNTIME_DLLS:WebDriver>
            $<TARGET_FILE_DIR:WebDriver>
        COMMAND_EXPAND_LISTS
    )
endif()
