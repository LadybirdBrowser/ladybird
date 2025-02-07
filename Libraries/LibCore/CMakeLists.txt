# These are the minimal set of sources needed to build the code generators. We separate them to allow
# LibCore to depend on generated sources.
set(SOURCES
    ArgsParser.cpp
    Directory.cpp
    DirectoryEntry.cpp
    DirIterator.cpp
    Environment.cpp
    File.cpp
    StandardPaths.cpp
    Version.cpp
)

if (WIN32)
    list(APPEND SOURCES SystemWindows.cpp)
else()
    list(APPEND SOURCES System.cpp)
endif()

serenity_lib(LibCoreMinimal coreminimal)

if (WIN32)
    find_path(DIRENT_INCLUDE_DIR dirent.h REQUIRED)
    target_include_directories(LibCoreMinimal PRIVATE ${DIRENT_INCLUDE_DIR})
endif()

if (LAGOM_TOOLS_ONLY)
    return()
endif()

set(SOURCES
    Command.cpp
    ConfigFile.cpp
    DateTime.cpp
    ElapsedTimer.cpp
    Event.cpp
    EventLoop.cpp
    EventLoopImplementation.cpp
    EventReceiver.cpp
    MappedFile.cpp
    MimeData.cpp
    Notifier.cpp
    Resource.cpp
    ResourceImplementation.cpp
    ResourceImplementationFile.cpp
    SystemServerTakeover.cpp
    TCPServer.cpp
    ThreadEventQueue.cpp
    Timer.cpp
    UDPServer.cpp
)

if (WIN32)
    list(APPEND SOURCES
        ProcessWindows.cpp
        SocketWindows.cpp
        AnonymousBufferWindows.cpp
        EventLoopImplementationWindows.cpp)
else()
    list(APPEND SOURCES
        Process.cpp
        Socket.cpp
        AnonymousBuffer.cpp
        EventLoopImplementationUnix.cpp)
endif()

if (NOT WIN32 AND NOT EMSCRIPTEN)
    list(APPEND SOURCES LocalServer.cpp)
endif()

# FIXME: Implement these for other systems.
if (LINUX AND NOT EMSCRIPTEN)
    list(APPEND SOURCES
        FileWatcherLinux.cpp
        Platform/ProcessStatisticsLinux.cpp
        TimeZoneWatcherLinux.cpp
    )
elseif (APPLE AND NOT IOS)
    list(APPEND SOURCES
        FileWatcherMacOS.mm
        Platform/ProcessStatisticsMach.cpp
        TimeZoneWatcherMacOS.mm
    )
else()
    list(APPEND SOURCES
        FileWatcherUnimplemented.cpp
        Platform/ProcessStatisticsUnimplemented.cpp
        TimeZoneWatcherUnimplemented.cpp
    )
endif()

if (APPLE OR CMAKE_SYSTEM_NAME STREQUAL "GNU")
    list(APPEND SOURCES MachPort.cpp)
endif()

if (APPLE)
    list(APPEND SOURCES IOSurface.cpp)
endif()

serenity_lib(LibCore core)
target_link_libraries(LibCore PRIVATE LibCrypt LibUnicode LibURL)
target_link_libraries(LibCore PUBLIC LibCoreMinimal)

if (APPLE)
    target_link_libraries(LibCore PUBLIC "-framework CoreFoundation")
    target_link_libraries(LibCore PUBLIC "-framework CoreServices")
    target_link_libraries(LibCore PUBLIC "-framework Foundation")
    target_link_libraries(LibCore PUBLIC "-framework IOSurface")
endif()

if (ANDROID)
    target_link_libraries(LibCore PRIVATE log)
endif()
