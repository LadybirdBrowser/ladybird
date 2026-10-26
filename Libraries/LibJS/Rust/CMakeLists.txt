# The headers of the embedding ABI, LibJS/Embedding/ABI.h and LibJS/Embedding/Layout.h, which LibJS's public headers
# include. A crate of its own writes them, so that the C++ that includes them waits for the headers alone, not for the
# runtime, which takes far longer to build.
generate_rust_ffi_headers(
    MANIFEST_PATH embedding_headers/Cargo.toml
    CRATE_NAME libjs_embedding_headers
    FFI_OUTPUT_DIR ${CMAKE_BINARY_DIR}/Libraries
    FFI_HEADERS
        LibJS/Embedding/ABI.h
        LibJS/Embedding/Layout.h
)

# Build the runtime independently for standalone JavaScript tools. LibWeb's crate also builds it as a dependency,
# with the same features, for LibWeb's library (Libraries/LibWeb/CMakeLists.txt).
import_rust_crate(
    MANIFEST_PATH Cargo.toml
    CRATE_NAME libjs_rust
    NO_DEFAULT_FEATURES
    FEATURES ${LIBJS_RUST_FEATURES}
)

# The runtime allocates its cells from LibGC, and its regex engine reads Unicode properties from LibUnicode.
target_link_libraries(libjs_rust INTERFACE LibGC LibUnicode)
if ((LINUX OR BSD) AND NOT BUILD_SHARED_LIBS)
    # The staticlib carries its own copy of the Rust standard library and of the Rust crates it shares with
    # LibUnicode, whose exported symbols then appear twice in static ELF links.
    target_link_options(libjs_rust INTERFACE LINKER:--allow-multiple-definition)
endif()
