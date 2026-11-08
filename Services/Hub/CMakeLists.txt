set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

set(SOURCES)
compile_ipc(HubClient.ipc HubClientEndpoint.h)
compile_ipc(HubServer.ipc HubServerEndpoint.h)

# The messages between the Hub and the UI, which both of them need.
add_library(hubipc STATIC ${SOURCES})
target_include_directories(hubipc PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(hubipc PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)
target_link_libraries(hubipc PRIVATE LibCore LibGfx LibHTTP LibIPC LibURL LibWebView)

set(SOURCES
    Application.cpp
    ConnectionFromUI.cpp
    Proxies.cpp
    View.cpp
)

add_library(hubservice STATIC ${SOURCES})
add_dependencies(hubservice hubipc)

target_include_directories(hubservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(hubservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(hubservice PRIVATE LibCore LibGfx LibHTTP LibIPC LibRequests LibURL LibWebView hubipc)

add_executable(Hub main.cpp)
target_include_directories(Hub PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)
target_link_libraries(Hub PRIVATE hubservice hubipc LibCore LibHTTP LibIPC LibMain LibRequests LibURL LibWebView)

ladybird_sign_helper_process(Hub)
