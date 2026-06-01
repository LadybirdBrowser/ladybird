set(TEST_SOURCES
    TestLibCoreAnonymousBuffer.cpp
    TestLibCoreArgsParser.cpp
    TestLibCoreDeferredInvoke.cpp
    TestLibCoreEventLoop.cpp
    TestLibCoreMappedFile.cpp
    TestLibCoreMimeType.cpp
    TestLibCorePromise.cpp
)

# FIXME: Change these tests to use a portable tempfile directory
if (LINUX)
    list(APPEND TEST_SOURCES
        TestLibCoreFileWatcher.cpp
    )
endif()

ladybird_test_suite(TestLibCore LibCore SOURCES ${TEST_SOURCES} LIBS LibSync LibThreading)

ladybird_test(TestLibCoreStream.cpp LibCore)
target_link_libraries(TestLibCoreStream PRIVATE LibSync LibThreading)

if(NOT WIN32)
    # These tests use the .txt files in the current directory
    set_tests_properties(TestLibCoreStream PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
