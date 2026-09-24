set(SOURCES
    ConnectionFromClient.cpp
)

add_library(mediaserverservice STATIC ${SOURCES})
add_executable(MediaServer main.cpp)

if (LINUX)
    target_sources(MediaServer PRIVATE SandboxLinux.cpp)
elseif (APPLE)
    target_sources(MediaServer PRIVATE SandboxMacOS.cpp)
else()
    target_sources(MediaServer PRIVATE SandboxUnimplemented.cpp)
endif()

add_dependencies(mediaserverservice
    generate_MediaClientEndpoint.h
    generate_MediaServerEndpoint.h
)

target_include_directories(mediaserverservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(mediaserverservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(MediaServer PRIVATE mediaserverservice LibCore LibMain LibMedia LibSandbox)
target_link_libraries(mediaserverservice PRIVATE LibCore LibIPC)

if (WIN32)
    ladybird_windows_bin(MediaServer CONSOLE)
endif()

ladybird_sign_helper_process(MediaServer)
