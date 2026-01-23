set(MEDIASERVER_SOURCES
    ConnectionFromClient.cpp
)

add_library(mediaserverservice STATIC ${MEDIASERVER_SOURCES})
set_target_properties(mediaserverservice PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

target_include_directories(mediaserverservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(mediaserverservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(mediaserverservice PUBLIC LibCore LibIPC LibMain LibMediaServerClient)

add_executable(MediaServer main.cpp)

# FIXME: Add Android service

target_include_directories(MediaServer PRIVATE ${LADYBIRD_SOURCE_DIR})
target_link_libraries(MediaServer PRIVATE mediaserverservice)

if(WIN32)
    lagom_windows_bin(MediaServer CONSOLE)
endif()
