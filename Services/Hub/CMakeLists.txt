set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

set(SOURCES
    View.cpp
)

add_library(hubservice STATIC ${SOURCES})

target_include_directories(hubservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(hubservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(hubservice PRIVATE LibCore LibGfx LibIPC LibURL LibWebView)
