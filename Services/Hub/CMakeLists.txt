set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

set(SOURCES
    ActionMirror.cpp
    Application.cpp
    ConnectionFromUI.cpp
    Proxies.cpp
    View.cpp
)

compile_ipc(HubClient.ipc HubClientEndpoint.h)
compile_ipc(HubServer.ipc HubServerEndpoint.h)

add_library(hubservice STATIC ${SOURCES})

target_include_directories(hubservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(hubservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(hubservice PRIVATE LibCore LibGfx LibIPC LibURL LibWebView)
