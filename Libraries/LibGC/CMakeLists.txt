set(SOURCES
    BlockAllocator.cpp
    CAPI.cpp
    Cell.cpp
    CellAllocator.cpp
    CrossHeapMember.cpp
    Root.cpp
    RootHashMap.cpp
    RootHashTable.cpp
    RootVector.cpp
    Rootable.cpp
    Heap.cpp
    HeapGroup.cpp
    HeapBlock.cpp
    PrimitiveStorage.cpp
    Timer.cpp
    WeakBlock.cpp
    WeakContainer.cpp
)

ladybird_lib(LibGC gc EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibGC PRIVATE LibCore)
target_link_libraries(LibGC PUBLIC LibThreading)

# The collector's core: the descriptors of blocks, and marking.
set(LIBGC_RUST_FEATURES allocator)
if (ENABLE_ADDRESS_SANITIZER)
    list(APPEND LIBGC_RUST_FEATURES address-sanitizer)
endif()
import_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libgc_rust FEATURES ${LIBGC_RUST_FEATURES} FFI_HEADER RustFFI.h)
test_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libgc_rust)
target_link_libraries(LibGC PRIVATE libgc_rust)
get_target_property(_libgc_rust_lib libgc_rust IMPORTED_LOCATION)
set_property(SOURCE BlockAllocator.cpp CAPI.cpp Cell.cpp CellAllocator.cpp Heap.cpp HeapBlock.cpp WeakBlock.cpp APPEND PROPERTY OBJECT_DEPENDS ${_libgc_rust_lib})

# The first word of a GC::ForeignCell belongs to its foreign implementation rather than holding a vtable pointer, so
# UBSan's vptr checks misfire whenever LibGC handles such a cell as a GC::Cell.
if (ENABLE_UNDEFINED_SANITIZER)
    target_compile_options(LibGC PRIVATE -fno-sanitize=vptr)
endif()

if(cpptrace_FOUND AND LADYBIRD_ENABLE_CPPTRACE)
    target_link_libraries(LibGC PRIVATE cpptrace::cpptrace)
    target_compile_definitions(LibGC PRIVATE LIBGC_HAS_CPPTRACE=1)
endif()

# TODO: Use ladybird_generate_export_header and annotate entire LibGC with export macros
include(GenerateExportHeader)
generate_export_header(LibGC EXPORT_MACRO_NAME GC_API EXPORT_FILE_NAME "Export.h")
