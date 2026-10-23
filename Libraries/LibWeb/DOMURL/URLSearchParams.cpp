/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2023-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/RustFFI.h>
#include <LibURL/RustIntegration.h>
#include <LibWeb/DOMURL/URLSearchParams.h>

namespace Web::DOMURL {

// https://url.spec.whatwg.org/#concept-urlencoded-serializer
String url_encode(Vector<QueryParam> const& tuples, StringView encoding)
{
    Vector<URL::FFI::FfiFormTuple> ffi_tuples;
    ffi_tuples.ensure_capacity(tuples.size());
    for (auto const& tuple : tuples)
        ffi_tuples.unchecked_append({ URL::RustIntegration::rust_url_input(tuple.name.utf16_view()), URL::RustIntegration::rust_url_input(tuple.value.utf16_view()) });

    String output;
    URL::FFI::rust_url_form_urlencoded_serialize(ffi_tuples.data(), ffi_tuples.size(), { reinterpret_cast<u8 const*>(encoding.characters_without_null_termination()), encoding.length() }, &output, [](void* ctx, URL::FFI::RustUrlByteSlice serialization) {
        *static_cast<String*>(ctx) = String::from_ascii_without_validation({ serialization.data, serialization.length });
    });
    return output;
}

// https://url.spec.whatwg.org/#concept-urlencoded-parser
Vector<QueryParam> url_decode(StringView input)
{
    Vector<QueryParam> output;
    URL::FFI::rust_url_form_urlencoded_parse({ reinterpret_cast<u8 const*>(input.characters_without_null_termination()), input.length() }, &output, [](void* ctx, URL::FFI::RustUrlByteSlice name, URL::FFI::RustUrlByteSlice value) {
        auto utf16_from_ffi = [](URL::FFI::RustUrlByteSlice string) {
            return Utf16String::from_utf8({ reinterpret_cast<char const*>(string.data), string.length });
        };
        static_cast<Vector<QueryParam>*>(ctx)->empend(utf16_from_ffi(name), utf16_from_ffi(value));
    });
    return output;
}

extern "C" {

// Defined in Bindings/Rust/src/domurl/exports.rs.
void web_url_search_params_serialize(void* cell, void* ctx, void (*on_complete)(void*, u8 const*, size_t));
}

String URLSearchParams::serialize_to_byte_string() const
{
    String serialization;
    web_url_search_params_serialize(const_cast<URLSearchParams*>(this), &serialization, [](void* ctx, u8 const* data, size_t length) {
        *static_cast<String*>(ctx) = String::from_ascii_without_validation({ data, length });
    });
    return serialization;
}

}
