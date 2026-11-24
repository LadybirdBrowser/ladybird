set(SOURCES
    Resolver.cpp
    Types.cpp
)

ladybird_lib(LibDNS dns EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibDNS PRIVATE LibCore LibThreading libdns_rust)

import_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libdns_rust FEATURES allocator FFI_HEADER RustFFI.h)
# The crate calls OpenSSL through libcrypto_rust, which leaves linking it to its users.
target_link_libraries(libdns_rust INTERFACE OpenSSL::Crypto)
test_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libdns_rust)

get_target_property(_libdns_rust_lib libdns_rust IMPORTED_LOCATION)
set_property(SOURCE Resolver.cpp APPEND PROPERTY OBJECT_DEPENDS ${_libdns_rust_lib})
set_property(SOURCE Types.cpp APPEND PROPERTY OBJECT_DEPENDS ${_libdns_rust_lib})
