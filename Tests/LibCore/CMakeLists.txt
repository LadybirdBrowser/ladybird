set(TEST_SOURCES
    TestLibCoreArgsParser.cpp
    TestLibCoreDateTime.cpp
    TestLibCoreDeferredInvoke.cpp
    TestLibCoreFileWatcher.cpp
    # FIXME: Identify and address the commit that caused this to start failing at runtime
    #TestLibCoreMappedFile.cpp
    TestLibCoreMimeType.cpp
    TestLibCorePromise.cpp
    TestLibCoreSharedSingleProducerCircularQueue.cpp
    # FIXME: Identify and address the commit that caused this to start failing at runtime
    #TestLibCoreStream.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibCore)
endforeach()

target_link_libraries(TestLibCoreDateTime PRIVATE LibUnicode)
target_link_libraries(TestLibCorePromise PRIVATE LibThreading)
# NOTE: Required because of the LocalServer tests
if (TARGET TestLibCoreStream)
    target_link_libraries(TestLibCoreStream PRIVATE LibThreading)
endif()
target_link_libraries(TestLibCoreSharedSingleProducerCircularQueue PRIVATE LibThreading)

if (ENABLE_SWIFT)
    find_package(SwiftTesting REQUIRED)

    add_executable(TestCoreSwift
        TestEventLoopActor.swift
        TestEventLoop.cpp
    )

    # FIXME: Swift doesn't seem to like object libraries for @main
    target_sources(TestCoreSwift PRIVATE ../Resources/SwiftTestMain.swift)

    set_target_properties(TestCoreSwift PROPERTIES SUFFIX .swift-testing)
    target_include_directories(TestCoreSwift PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(TestCoreSwift PRIVATE AK LibCore SwiftTesting::SwiftTesting)
    add_test(NAME TestCoreSwift COMMAND TestCoreSwift)
endif()
