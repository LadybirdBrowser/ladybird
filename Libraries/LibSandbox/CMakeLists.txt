set(SOURCES
    ConnectBroker.cpp
    Sandbox.cpp
)

if (LINUX)
    list(APPEND SOURCES
        GpuDriverPaths.cpp
        Seccomp.cpp
    )
endif()

ladybird_lib(LibSandbox sandbox)

if (LINUX OR APPLE)
    target_link_libraries(LibSandbox PRIVATE LibCore)
endif()
