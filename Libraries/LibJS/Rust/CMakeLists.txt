set(LIBJS_RUST_FEATURES allocator)
if (ENABLE_ADDRESS_SANITIZER)
    list(APPEND LIBJS_RUST_FEATURES address-sanitizer)
endif()
if (ENABLE_THREAD_SANITIZER)
    list(APPEND LIBJS_RUST_FEATURES thread-sanitizer)
endif()

import_rust_crate(
    MANIFEST_PATH Cargo.toml
    CRATE_NAME libjs_rust
    FEATURES ${LIBJS_RUST_FEATURES}
    FFI_OUTPUT_DIR ${CMAKE_BINARY_DIR}/Libraries
    FFI_HEADERS
        LibJS/Embedding/ABI.h
        LibJS/Embedding/Layout.h
)
# The headers land in the build tree's Libraries directory, which every target already includes. CMake would otherwise
# make the include directory of an imported target a system one for its consumers, hiding the warnings of every header
# generated there.
set_target_properties(libjs_rust PROPERTIES SYSTEM OFF)

# The runtime allocates its cells from LibGC, and its regex engine reads Unicode properties from LibUnicode.
target_link_libraries(libjs_rust INTERFACE LibGC LibUnicode)
if ((LINUX OR BSD) AND NOT BUILD_SHARED_LIBS)
    # The staticlib carries its own copy of the Rust standard library and of the Rust crates it shares with
    # LibUnicode, whose exported symbols then appear twice in static ELF links.
    target_link_options(libjs_rust INTERFACE LINKER:--allow-multiple-definition)
endif()
