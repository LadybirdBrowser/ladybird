if (ENABLE_SWIFT)
    find_package(SwiftTesting REQUIRED)

    add_executable(TestGCSwift
        TestGCBindings.swift
        TestHeap.cpp
        TestInterop.cpp
    )

    # FIXME: Swift doesn't seem to like object libraries for @main
    target_sources(TestGCSwift PRIVATE ../Resources/SwiftTestMain.swift)

    set_target_properties(TestGCSwift PROPERTIES SUFFIX .swift-testing)
    target_include_directories(TestGCSwift PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(TestGCSwift PRIVATE AK LibGC SwiftTesting::SwiftTesting)
    add_test(NAME TestGCSwift COMMAND TestGCSwift)
endif()
