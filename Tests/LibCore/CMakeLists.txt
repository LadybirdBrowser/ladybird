set(TEST_SOURCES
    TestLibCoreAnonymousBuffer.cpp
    TestLibCoreArgsParser.cpp
    TestLibCoreDeferredInvoke.cpp
    TestLibCoreDirectory.cpp
    TestLibCoreEventLoop.cpp
    TestLibCoreGeolocationAuthorization.cpp
    TestLibCoreImmutableBytes.cpp
    TestLibCoreMappedFile.cpp
    TestLibCoreMimeType.cpp
    TestLibCorePromise.cpp
    TestLibCoreStream.cpp
)

# FIXME: Change these tests to use a portable tempfile directory
if (LINUX)
    list(APPEND TEST_SOURCES
        TestLibCoreFileWatcher.cpp
    )
endif()

if (APPLE OR LINUX)
    list(APPEND TEST_SOURCES
        TestLibCoreFutex.cpp
        TestLibCoreProcess.cpp
    )
endif()

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibCore)
endforeach()

target_link_libraries(TestLibCoreDirectory PRIVATE LibFileSystem)
target_link_libraries(TestLibCoreImmutableBytes PRIVATE LibFileSystem)
target_link_libraries(TestLibCorePromise PRIVATE LibThreading)
target_link_libraries(TestLibCoreStream PRIVATE LibFileSystem LibThreading)

if(NOT WIN32)
    # These tests use the .txt files in the current directory
    set_tests_properties(TestLibCoreMappedFile TestLibCoreStream PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

target_link_libraries(TestLibCoreEventLoop PRIVATE LibThreading)

if (APPLE OR LINUX)
    target_link_libraries(TestLibCoreFutex PRIVATE LibThreading)
endif()
