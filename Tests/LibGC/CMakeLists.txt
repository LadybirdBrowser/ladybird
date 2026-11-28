ladybird_test_suite(LibGC)

set(TEST_SOURCES
    TestBlockAllocator.cpp
    TestCompressedPointer.cpp
    TestExternalEntityTable.cpp
    TestGCContainers.cpp
    TestGCEphemeron.cpp
    TestGCFunction.cpp
    TestGCHeapGroup.cpp
    TestGCIdleCollection.cpp
    TestGCIncrementalSweep.cpp
    TestGCRustAPI.cpp
    TestGCThreads.cpp
    TestGCTypeIsolation.cpp
    TestGCVisitor.cpp
    TestMember.cpp
    TestNanBoxedValue.cpp
    TestPrimitiveStorage.cpp
)

# Rust code that allocates from a heap, which only runs in a process that links LibGC as the C++ build does.
import_rust_crate(MANIFEST_PATH RustAPI/Cargo.toml CRATE_NAME libgc_api_test FEATURES allocator)

target_sources(TestLibGC PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibGC PRIVATE LibGC LibThreading libgc_api_test)
