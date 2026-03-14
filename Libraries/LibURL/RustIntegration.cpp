/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/IPv4Address.h>
#include <AK/IPv6Address.h>
#include <AK/StringUtils.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>
#include <LibTextCodec/Encoder.h>
#include <LibURL/Parser.h>
#include <LibURL/RustFFI.h>
#include <LibURL/RustIntegration.h>
#include <LibURL/URL.h>

namespace URL::RustIntegration {

static String string_from_ffi(FFI::RustUrlByteSlice slice)
{
    return String::from_ascii_without_validation({ reinterpret_cast<char const*>(slice.data), slice.length });
}

static FFI::FfiUrlHost host_to_ffi(Optional<Host> const& host)
{
    FFI::FfiUrlHost result {};
    if (!host.has_value()) {
        result.has_host = false;
        return result;
    }
    result.has_host = true;
    host->value().visit(
        [&](IPv4Address const& addr) {
            result.kind = FFI::RustUrlHostKind::Ipv4;
            u32 const n = addr.to_u32();
            result.ipv4[0] = static_cast<u8>(n >> 24);
            result.ipv4[1] = static_cast<u8>(n >> 16);
            result.ipv4[2] = static_cast<u8>(n >> 8);
            result.ipv4[3] = static_cast<u8>(n);
        },
        [&](IPv6Address const& addr) {
            result.kind = FFI::RustUrlHostKind::Ipv6;
            for (size_t i = 0; i < 8; i++) {
                u16 piece = addr[i];
                result.ipv6[i * 2] = static_cast<u8>(piece >> 8);
                result.ipv6[(i * 2) + 1] = static_cast<u8>(piece & 0xff);
            }
        },
        [&](String const& str) {
            result.kind = FFI::RustUrlHostKind::String;
            result.string_data = reinterpret_cast<uint8_t const*>(str.bytes().data());
            result.string_length = str.bytes().size();
        });
    return result;
}

static Optional<URL> url_from_ffi(FFI::RustFfiUrl const& ffi)
{
    URL url;
    url.set_scheme(string_from_ffi(ffi.scheme));
    url.set_username(string_from_ffi(ffi.username));
    url.set_password(string_from_ffi(ffi.password));

    if (ffi.host.has_host) {
        switch (ffi.host.kind) {
        case FFI::RustUrlHostKind::String:
            url.set_host(Host(string_from_ffi({ ffi.host.string_data, ffi.host.string_length })));
            break;
        case FFI::RustUrlHostKind::Ipv4: {
            u32 const n = (static_cast<u32>(ffi.host.ipv4[0]) << 24)
                | (static_cast<u32>(ffi.host.ipv4[1]) << 16)
                | (static_cast<u32>(ffi.host.ipv4[2]) << 8)
                | static_cast<u32>(ffi.host.ipv4[3]);
            url.set_host(Host(IPv4Address(NetworkOrdered<u32>(n))));
            break;
        }
        case FFI::RustUrlHostKind::Ipv6: {
            Array<u16, 8> pieces;
            for (size_t i = 0; i < 8; i++)
                pieces[i] = (static_cast<u16>(ffi.host.ipv6[i * 2]) << 8) | ffi.host.ipv6[(i * 2) + 1];
            url.set_host(Host(IPv6Address(pieces)));
            break;
        }
        }
    }

    if (ffi.has_port)
        url.set_port(ffi.port);
    else
        url.set_port({});

    url.set_has_an_opaque_path(ffi.has_opaque_path);
    Vector<String> paths;
    if (ffi.path_segments) {
        paths.ensure_capacity(ffi.path_segment_count);
        for (size_t i = 0; i < ffi.path_segment_count; i++)
            paths.unchecked_append(string_from_ffi(ffi.path_segments[i]));
    }
    url.set_raw_paths(move(paths));

    if (ffi.has_query)
        url.set_query(string_from_ffi(ffi.query));
    else
        url.set_query({});

    if (ffi.has_fragment)
        url.set_fragment(string_from_ffi(ffi.fragment));
    else
        url.set_fragment({});

    return url;
}

struct UrlFfiStorage {
    Vector<FFI::RustUrlByteSlice> path_segments;
    FFI::RustFfiUrl ffi_url {};
};

static UrlFfiStorage url_to_ffi(URL const& url)
{
    UrlFfiStorage storage;

    storage.ffi_url.scheme = { reinterpret_cast<uint8_t const*>(url.scheme().bytes().data()), url.scheme().bytes().size() };
    storage.ffi_url.username = { reinterpret_cast<uint8_t const*>(url.username().bytes().data()), url.username().bytes().size() };
    storage.ffi_url.password = { reinterpret_cast<uint8_t const*>(url.password().bytes().data()), url.password().bytes().size() };

    storage.ffi_url.host = host_to_ffi(url.host());

    storage.ffi_url.has_port = url.port().has_value();
    storage.ffi_url.port = url.port().value_or(0);

    storage.path_segments.ensure_capacity(url.paths().size());
    for (auto const& segment : url.paths()) {
        storage.path_segments.unchecked_append({
            reinterpret_cast<uint8_t const*>(segment.bytes().data()),
            segment.bytes().size(),
        });
    }
    storage.ffi_url.path_segments = storage.path_segments.data();
    storage.ffi_url.path_segment_count = storage.path_segments.size();

    storage.ffi_url.has_opaque_path = url.has_an_opaque_path();

    storage.ffi_url.has_query = url.query().has_value();
    if (url.query().has_value())
        storage.ffi_url.query = { reinterpret_cast<uint8_t const*>(url.query()->bytes().data()), url.query()->bytes().size() };

    storage.ffi_url.has_fragment = url.fragment().has_value();
    if (url.fragment().has_value())
        storage.ffi_url.fragment = { reinterpret_cast<uint8_t const*>(url.fragment()->bytes().data()), url.fragment()->bytes().size() };

    return storage;
}

struct ParseCallbackCtx {
    Optional<URL>* result;
    URL* url_inout;
};

static void on_basic_parse_complete(void* ctx_ptr, FFI::RustFfiUrl const* ffi_result)
{
    auto* ctx = static_cast<ParseCallbackCtx*>(ctx_ptr);
    if (!ffi_result)
        return;
    *ctx->result = url_from_ffi(*ffi_result);
    if (ctx->url_inout && ctx->result->has_value())
        *ctx->url_inout = **ctx->result;
}

Optional<URL> parse_basic_url(StringView input, Optional<URL const&> base_url, URL* url, Optional<Parser::State> state_override, Optional<StringView> encoding)
{
    auto const state_override_from_cpp = [](Parser::State state) {
        return static_cast<FFI::State>(to_underlying(state));
    };

    Optional<UrlFfiStorage> base_storage;
    if (base_url.has_value())
        base_storage = url_to_ffi(*base_url);

    Optional<UrlFfiStorage> url_storage;
    if (url)
        url_storage = url_to_ffi(*url);

    FFI::RustBasicParseOptions options {
        .has_base_url = base_url.has_value(),
        .has_url = url != nullptr,
        .base_url = base_storage.has_value() ? base_storage->ffi_url : FFI::RustFfiUrl {},
        .url = url_storage.has_value() ? url_storage->ffi_url : FFI::RustFfiUrl {},
        .has_state_override = state_override.has_value(),
        .state_override = state_override.map(state_override_from_cpp).value_or(FFI::State::SchemeStart),
        .encoding = {
            reinterpret_cast<uint8_t const*>(encoding.has_value() ? encoding->characters_without_null_termination() : nullptr),
            encoding.value_or(""sv).length(),
        },
    };

    Optional<URL> result;
    ParseCallbackCtx ctx { .result = &result, .url_inout = url };
    bool const did_succeed = rust_url_basic_parse(
        reinterpret_cast<uint8_t const*>(input.characters_without_null_termination()),
        input.length(),
        &options,
        &ctx,
        on_basic_parse_complete);
    if (!did_succeed)
        return {};
    return result;
}

}

namespace URL::FFI {

extern "C" bool textcodec_rust_encode(uint8_t const* encoding, size_t encoding_length, uint8_t const* input, size_t input_length, void* ctx, FfiByteFn on_byte, FfiCodePointFn on_error)
{
    auto encoder = TextCodec::encoder_for(StringView { encoding, encoding_length });
    if (!encoder.has_value())
        return false;

    auto result = encoder->process(
        Utf8View { StringView { input, input_length } },
        [&](u8 byte) -> ErrorOr<void> {
            on_byte(ctx, byte);
            return {};
        },
        [&](u32 error) -> ErrorOr<void> {
            on_error(ctx, error);
            return {};
        });
    return !result.is_error();
}

}
