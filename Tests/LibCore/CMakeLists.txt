set(TEST_SOURCES
    TestLibCoreArgsParser.cpp
    TestLibCoreDateTime.cpp
    TestLibCoreDeferredInvoke.cpp
    TestLibCoreFilePermissionsMask.cpp
    TestLibCoreFileWatcher.cpp
    TestLibCoreMappedFile.cpp
    TestLibCorePromise.cpp
    TestLibCoreSharedSingleProducerCircularQueue.cpp
    TestLibCoreStream.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibCore)
endforeach()

target_link_libraries(TestLibCoreDateTime PRIVATE LibUnicode)
target_link_libraries(TestLibCorePromise PRIVATE LibThreading)
# NOTE: Required because of the LocalServer tests
target_link_libraries(TestLibCoreStream PRIVATE LibThreading)
target_link_libraries(TestLibCoreSharedSingleProducerCircularQueue PRIVATE LibThreading)
