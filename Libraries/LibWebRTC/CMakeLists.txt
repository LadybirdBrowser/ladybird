build_rust_binary(
    MANIFEST_PATH Rust/Cargo.toml
    CRATE_NAME ladybird_webrtc
    BINARY_NAME webrtc-test
    OUTPUT_PATH_VAR LADYBIRD_WEBRTC_TEST_BINARY
    FEATURES test-tools
)
