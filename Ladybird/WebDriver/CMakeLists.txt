set(WEBDRIVER_SOURCE_DIR ${LADYBIRD_SOURCE_DIR}/Userland/Services/WebDriver)

set(SOURCES
    ${WEBDRIVER_SOURCE_DIR}/Client.cpp
    ${WEBDRIVER_SOURCE_DIR}/Session.cpp
    ${WEBDRIVER_SOURCE_DIR}/WebContentConnection.cpp
    ../Utilities.cpp
    main.cpp
)

add_executable(WebDriver ${SOURCES})

target_include_directories(WebDriver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(WebDriver PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/..)
target_include_directories(WebDriver PRIVATE ${LADYBIRD_SOURCE_DIR}/Userland)
target_include_directories(WebDriver PRIVATE ${LADYBIRD_SOURCE_DIR}/Userland/Services)
target_link_libraries(WebDriver PRIVATE LibCore LibFileSystem LibGfx LibIPC LibJS LibMain LibWeb LibWebSocket LibWebView)
add_dependencies(WebDriver headless-browser)
