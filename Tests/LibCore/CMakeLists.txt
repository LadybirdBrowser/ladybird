set(TEST_SOURCES
    TestLibCoreAnonymousBuffer.cpp
    TestLibCoreArgsParser.cpp
    TestLibCoreDeferredInvoke.cpp
    TestLibCoreEventLoop.cpp
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

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibCore)
endforeach()

target_link_libraries(TestLibCorePromise PRIVATE LibSync LibThreading)
target_link_libraries(TestLibCoreStream PRIVATE LibSync LibThreading)

if(NOT WIN32)
    # These tests use the .txt files in the current directory
    set_tests_properties(TestLibCoreMappedFile TestLibCoreStream PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

target_link_libraries(TestLibCoreEventLoop PRIVATE LibSync LibThreading)
