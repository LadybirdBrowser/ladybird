set(SOURCES
    AddressInfoVector.cpp
    ArgsParser.cpp
    ConfigFile.cpp
    DirIterator.cpp
    Directory.cpp
    DirectoryEntry.cpp
    ElapsedTimer.cpp
    Environment.cpp
    EventLoop.cpp
    EventLoopImplementation.cpp
    EventReceiver.cpp
    File.cpp
    MappedFile.cpp
    MimeData.cpp
    Notifier.cpp
    ReportTime.cpp
    Resource.cpp
    ResourceImplementation.cpp
    ResourceImplementationFile.cpp
    SharedVersion.cpp
    SocketAddress.cpp
    StandardPaths.cpp
    SystemServerTakeover.cpp
    ThreadEventQueue.cpp
    Timer.cpp
    Version.cpp
)

if (WIN32)
    list(APPEND SOURCES
        AnonymousBufferWindows.cpp
        EventLoopImplementationWindows.cpp
        LocalServerWindows.cpp
        ProcessWindows.cpp
        SocketWindows.cpp
        SocketpairWindows.cpp
        SystemWindows.cpp
        TCPServerWindows.cpp
        UDPServerWindows.cpp
    )
else()
    list(APPEND SOURCES
        AnonymousBuffer.cpp
        EventLoopImplementationUnix.cpp
        LocalServer.cpp
        Process.cpp
        Socket.cpp
        System.cpp
        TCPServer.cpp
        UDPServer.cpp
    )
endif()

include(CheckIncludeFile)

check_include_file(sys/inotify.h HAVE_INOTIFY)

if (HAVE_INOTIFY)
    list(APPEND SOURCES FileWatcherInotify.cpp)
else()
    list(APPEND SOURCES FileWatcherUnimplemented.cpp)
endif()

# FIXME: Implement ProcessStatistics for Windows and *BSD
if (LINUX)
    list(APPEND SOURCES Platform/ProcessStatisticsLinux.cpp)
elseif (APPLE AND NOT IOS)
    list(APPEND SOURCES Platform/ProcessStatisticsMach.cpp)
else ()
    list(APPEND SOURCES Platform/ProcessStatisticsUnimplemented.cpp)
endif()

if (LINUX OR BSD)
    list(APPEND SOURCES TimeZoneWatcherUnix.cpp)
elseif (APPLE AND NOT IOS)
    list(APPEND SOURCES TimeZoneWatcherMacOS.mm)
elseif (WIN32)
    list(APPEND SOURCES TimeZoneWatcherWindows.cpp)
else()
    list(APPEND SOURCES TimeZoneWatcherUnimplemented.cpp)
endif()

if (APPLE OR CMAKE_SYSTEM_NAME STREQUAL "GNU")
    list(APPEND SOURCES MachPort.cpp)
endif()

if (APPLE)
    list(APPEND SOURCES IOSurface.cpp)
endif()

ladybird_lib(LibCore core)
target_link_libraries(LibCore PRIVATE LibUnicode LibURL Threads::Threads)

if (${CMAKE_SYSTEM_NAME} MATCHES "NetBSD")
    # NetBSD has its shm_open and shm_unlink functions in librt so we need to link that
    target_link_libraries(LibCore PRIVATE rt)
endif()
if (${CMAKE_SYSTEM_NAME} MATCHES "SunOS")
    # Solaris has socket and networking related functions in two extra libraries
    target_link_libraries(LibCore PRIVATE nsl socket)
endif()
if (HAIKU)
    # Haiku has networking related functions in the network library
    target_link_libraries(LibCore PRIVATE network)
endif()

if (APPLE)
    target_link_libraries(LibCore PUBLIC "-framework CoreFoundation")
    target_link_libraries(LibCore PUBLIC "-framework CoreServices")
    target_link_libraries(LibCore PUBLIC "-framework Foundation")
    target_link_libraries(LibCore PUBLIC "-framework IOSurface")
endif()

if (WIN32)
    target_link_libraries(LibCore PRIVATE ntdll.dll)
    find_path(DIRENT_INCLUDE_DIR dirent.h REQUIRED)
    target_include_directories(LibCore PRIVATE ${DIRENT_INCLUDE_DIR})
endif()

if (ANDROID)
    target_link_libraries(LibCore PRIVATE log)
endif()
