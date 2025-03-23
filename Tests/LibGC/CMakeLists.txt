if (ENABLE_SWIFT)
    find_package(SwiftTesting REQUIRED)

    add_executable(TestGCSwift
        TestGCBindings.swift
        TestHeap.cpp
        TestInterop.cpp
    )
    # FIXME: Swift doesn't seem to like object libraries for @main
    target_sources(TestGCSwift PRIVATE ../Resources/SwiftTestMain.swift)

    generate_clang_module_map(TestGCSwift)

    set_target_properties(TestGCSwift PROPERTIES SUFFIX .swift-testing)
    target_include_directories(TestGCSwift PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(TestGCSwift PRIVATE AK LibGC SwiftTesting::SwiftTesting)

    get_property(testing_compile_options TARGET SwiftTesting::SwiftTesting PROPERTY INTERFACE_LINK_OPTIONS)

    add_swift_target_properties(TestGCSwift
        LAGOM_LIBRARIES AK LibGC
        COMPILE_DEFINITIONS LIBGC_WORKAROUND_BOOL_BITFIELD
        COMPILE_OPTIONS ${testing_compile_options} -enable-experimental-feature Extern
    )
    add_test(NAME TestGCSwift COMMAND TestGCSwift)
endif()
