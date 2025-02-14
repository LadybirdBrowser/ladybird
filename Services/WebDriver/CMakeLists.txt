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
