set(SOURCES
    Connection.cpp
    Decoder.cpp
    Encoder.cpp
    File.cpp
    Message.cpp
)

if (UNIX)
    list(APPEND SOURCES TransportSocket.cpp)
else()
    list(APPEND SOURCES TransportSocketWindows.cpp)
endif()

serenity_lib(LibIPC ipc)
target_link_libraries(LibIPC PRIVATE LibCore LibThreading LibURL)

if (WIN32)
    find_package(pthread REQUIRED)
    target_include_directories(LibIPC PRIVATE ${PTHREAD_INCLUDE_DIR})
endif()
