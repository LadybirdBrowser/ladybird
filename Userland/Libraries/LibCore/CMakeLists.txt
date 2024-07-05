include(vulkan)

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
    System.cpp
    Version.cpp
)

serenity_lib(LibCoreMinimal coreminimal)

if (LAGOM_TOOLS_ONLY)
    return()
endif()

set(SOURCES
    AnonymousBuffer.cpp
    Command.cpp
    ConfigFile.cpp
    DateTime.cpp
    ElapsedTimer.cpp
    Event.cpp
    EventLoop.cpp
    EventLoopImplementation.cpp
    EventLoopImplementationUnix.cpp
    EventReceiver.cpp
    LockFile.cpp
    MappedFile.cpp
    MimeData.cpp
    NetworkJob.cpp
    Notifier.cpp
    Process.cpp
    ProcessStatisticsReader.cpp
    Resource.cpp
    ResourceImplementation.cpp
    ResourceImplementationFile.cpp
    SecretString.cpp
    SessionManagement.cpp
    Socket.cpp
    SOCKSProxyClient.cpp
    SystemServerTakeover.cpp
    TCPServer.cpp
    ThreadEventQueue.cpp
    Timer.cpp
    UDPServer.cpp
)
if (NOT ANDROID AND NOT WIN32 AND NOT EMSCRIPTEN)
    list(APPEND SOURCES
        Account.cpp
        FilePermissionsMask.cpp
        GetPassword.cpp
        Group.cpp
    )
endif()
if (NOT WIN32 AND NOT EMSCRIPTEN)
    list(APPEND SOURCES LocalServer.cpp)
endif()

# FIXME: Implement Core::FileWatcher for *BSD and Windows.
if (LINUX AND NOT EMSCRIPTEN)
    list(APPEND SOURCES
        FileWatcherLinux.cpp
        Platform/ProcessStatisticsLinux.cpp
    )
elseif (APPLE AND NOT IOS)
    list(APPEND SOURCES
        FileWatcherMacOS.mm
        Platform/ProcessStatisticsMach.cpp
    )
else()
    list(APPEND SOURCES
        FileWatcherUnimplemented.cpp
        Platform/ProcessStatisticsUnimplemented.cpp
    )
endif()

if (HAS_VULKAN)
    list(APPEND SOURCES VulkanContext.cpp)
endif()

if (APPLE OR CMAKE_SYSTEM_NAME STREQUAL "GNU")
    list(APPEND SOURCES MachPort.cpp)
endif()

if (APPLE)
    list(APPEND SOURCES IOSurface.cpp)
    list(APPEND SOURCES MetalContext.mm)
endif()

serenity_lib(LibCore core)
target_link_libraries(LibCore PRIVATE LibCrypt LibUnicode LibURL)
target_link_libraries(LibCore PUBLIC LibCoreMinimal)

if (APPLE)
    target_link_libraries(LibCore PUBLIC "-framework CoreFoundation")
    target_link_libraries(LibCore PUBLIC "-framework CoreServices")
    target_link_libraries(LibCore PUBLIC "-framework Foundation")
    target_link_libraries(LibCore PUBLIC "-framework IOSurface")
    target_link_libraries(LibCore PUBLIC "-framework Metal")
endif()

if (ANDROID)
    target_link_libraries(LibCore PRIVATE log)
endif()

if (HAS_VULKAN)
    target_link_libraries(LibCore PUBLIC Vulkan::Vulkan Vulkan::Headers)
endif()
