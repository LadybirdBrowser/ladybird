set(SOURCES
    Sandbox.cpp
)

if (LINUX)
    list(APPEND SOURCES
        Seccomp.cpp
    )
endif()

ladybird_lib(LibSandbox sandbox)
