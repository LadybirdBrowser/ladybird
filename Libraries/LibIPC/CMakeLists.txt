set(SOURCES
    Connection.cpp
    Decoder.cpp
    Encoder.cpp
    Message.cpp
)

if (UNIX)
    list(APPEND SOURCES TransportSocket.cpp)
endif()

serenity_lib(LibIPC ipc)
target_link_libraries(LibIPC PRIVATE LibCore LibURL LibThreading)

if (WIN32)
    find_package(pthread REQUIRED)
    target_include_directories(LibIPC PRIVATE ${PTHREAD_INCLUDE_DIR})
endif()
