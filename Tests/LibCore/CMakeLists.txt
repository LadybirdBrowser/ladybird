set(TEST_SOURCES
    TestLibCoreArgsParser.cpp
    TestLibCoreDeferredInvoke.cpp
    TestLibCoreFileWatcher.cpp
    TestLibCoreMimeType.cpp
    TestLibCorePromise.cpp
    TestLibCoreSharedSingleProducerCircularQueue.cpp
)

# FIXME: Change these tests to use a portable tempfile directory
if (NOT WIN32)
    list(APPEND TEST_SOURCES
        TestLibCoreMappedFile.cpp
        TestLibCoreStream.cpp
    )
endif()

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibCore)
endforeach()

target_link_libraries(TestLibCorePromise PRIVATE LibThreading)
if (NOT WIN32)
    target_link_libraries(TestLibCoreStream PRIVATE LibThreading)
    # These tests use the .txt files in the current directory
    set_tests_properties(TestLibCoreMappedFile TestLibCoreStream PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
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
