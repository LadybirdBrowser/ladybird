set(TEST_SOURCES
    TestLibCoreArgsParser.cpp
    TestLibCoreDeferredInvoke.cpp
    TestLibCoreEventLoop.cpp
    TestLibCoreMappedFile.cpp
    TestLibCoreMimeType.cpp
    TestLibCorePromise.cpp
    TestLibCoreSharedSingleProducerCircularQueue.cpp
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

target_link_libraries(TestLibCorePromise PRIVATE LibThreading LibSync)
target_link_libraries(TestLibCoreStream PRIVATE LibThreading LibSync)

if(NOT WIN32)
    # These tests use the .txt files in the current directory
    set_tests_properties(TestLibCoreMappedFile TestLibCoreStream PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

target_link_libraries(TestLibCoreSharedSingleProducerCircularQueue PRIVATE LibThreading)
