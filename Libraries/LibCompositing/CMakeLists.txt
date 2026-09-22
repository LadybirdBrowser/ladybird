import_rust_crate(
    MANIFEST_PATH Rust/Cargo.toml
    CRATE_NAME libcompositing_rust
    FEATURES allocator
    FFI_HEADERS
        RustFFI.h
        TypesRustFFI.h
        DisplayList/DisplayListCommandsGenerated.h
)
target_link_libraries(libcompositing_rust INTERFACE LibGfx)
test_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libcompositing_rust)
