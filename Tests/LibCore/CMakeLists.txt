ladybird_test_suite(LibCore)

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
        TestLibCoreStandardPaths.cpp
    )
endif()

target_sources(TestLibCore PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibCore PRIVATE LibThreading)

if (NOT WIN32)
    # These tests use the .txt files in the current directory.
    set_tests_properties(TestLibCore PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
