set(SOURCES
    ControlConnection.cpp
    Tabs.cpp
)

add_library(audioserverservice STATIC ${SOURCES})
add_executable(AudioServer main.cpp)

if (LINUX)
    target_sources(AudioServer PRIVATE SandboxLinux.cpp)
elseif (APPLE)
    target_sources(AudioServer PRIVATE SandboxMacOS.cpp)
else()
    target_sources(AudioServer PRIVATE SandboxUnimplemented.cpp)
endif()

add_dependencies(audioserverservice
    generate_AudioServerControlClientEndpoint.h
    generate_AudioServerControlEndpoint.h
)

target_include_directories(audioserverservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(audioserverservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(AudioServer PRIVATE audioserverservice LibCore LibMain LibMedia LibSandbox)
target_link_libraries(audioserverservice PRIVATE LibCore LibIPC LibMedia LibThreading)
target_link_libraries(AudioServer PRIVATE LibThreading)

if (WIN32)
    ladybird_windows_bin(AudioServer CONSOLE)
endif()

ladybird_sign_helper_process(AudioServer)
