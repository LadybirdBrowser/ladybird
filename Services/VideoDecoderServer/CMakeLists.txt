set(SOURCES
    ControlConnection.cpp
)

add_library(videodecoderserverservice STATIC ${SOURCES})
add_executable(VideoDecoderServer main.cpp)

if (LINUX)
    target_sources(VideoDecoderServer PRIVATE SandboxLinux.cpp)
elseif (APPLE)
    target_sources(VideoDecoderServer PRIVATE SandboxMacOS.cpp)
else()
    target_sources(VideoDecoderServer PRIVATE SandboxUnimplemented.cpp)
endif()

add_dependencies(videodecoderserverservice
    generate_VideoDecoderServerControlClientEndpoint.h
    generate_VideoDecoderServerControlEndpoint.h
)

target_include_directories(videodecoderserverservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(videodecoderserverservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(VideoDecoderServer PRIVATE videodecoderserverservice LibCore LibGfx LibMain LibMediaCodecs LibSandbox)
target_link_libraries(videodecoderserverservice PRIVATE LibCore LibIPC LibMediaCodecs)

if (WIN32)
    target_include_directories(VideoDecoderServer PRIVATE $<BUILD_INTERFACE:${PTHREAD_INCLUDE_DIR}>)
    target_include_directories(videodecoderserverservice PRIVATE $<BUILD_INTERFACE:${PTHREAD_INCLUDE_DIR}>)
    ladybird_windows_bin(VideoDecoderServer CONSOLE)
endif()

ladybird_sign_helper_process(VideoDecoderServer)
