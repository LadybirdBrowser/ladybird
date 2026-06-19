set(SOURCES
    Decoder.cpp
    Encoder.cpp
)

ladybird_lib(LibTextCodec textcodec EXPLICIT_SYMBOL_EXPORT)

import_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libtextcodec_rust FFI_HEADER RustFFI.h)
target_link_libraries(LibTextCodec PRIVATE libtextcodec_rust)

get_target_property(_libtextcodec_rust_lib libtextcodec_rust IMPORTED_LOCATION)
set_property(SOURCE Decoder.cpp APPEND PROPERTY OBJECT_DEPENDS ${_libtextcodec_rust_lib})
set_property(SOURCE Encoder.cpp APPEND PROPERTY OBJECT_DEPENDS ${_libtextcodec_rust_lib})
