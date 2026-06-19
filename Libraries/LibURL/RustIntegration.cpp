/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/IPv4Address.h>
#include <AK/IPv6Address.h>
#include <AK/OwnPtr.h>
#include <AK/StringUtils.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>
#include <LibURL/Parser.h>
#include <LibURL/RustFFI.h>
#include <LibURL/RustIntegration.h>
#include <LibURL/URL.h>

namespace URL::RustIntegration {

static String string_from_ffi(FFI::RustUrlByteSlice slice)
{
    return String::from_ascii_without_validation({ reinterpret_cast<char const*>(slice.data), slice.length });
}

static FFI::RustUrlByteSlice string_to_ffi(StringView string)
{
    return {
        .data = reinterpret_cast<u8 const*>(string.characters_without_null_termination()),
        .length = string.length(),
    };
}

static FFI::RustUrlByteSlice string_to_ffi(String const& string)
{
    return string_to_ffi(string.bytes_as_string_view());
}

static FFI::RustUrlByteSlice optional_string_to_ffi(Optional<String> const& string)
{
    if (!string.has_value())
        return string_to_ffi(""sv);
    return string_to_ffi(*string);
}

static FFI::RustUrlPatternInit init_to_ffi(URLPattern::Init const& init)
{
    return {
        .has_protocol = init.protocol.has_value(),
        .protocol = optional_string_to_ffi(init.protocol),
        .has_username = init.username.has_value(),
        .username = optional_string_to_ffi(init.username),
        .has_password = init.password.has_value(),
        .password = optional_string_to_ffi(init.password),
        .has_hostname = init.hostname.has_value(),
        .hostname = optional_string_to_ffi(init.hostname),
        .has_port = init.port.has_value(),
        .port = optional_string_to_ffi(init.port),
        .has_pathname = init.pathname.has_value(),
        .pathname = optional_string_to_ffi(init.pathname),
        .has_search = init.search.has_value(),
        .search = optional_string_to_ffi(init.search),
        .has_hash = init.hash.has_value(),
        .hash = optional_string_to_ffi(init.hash),
        .has_base_url = init.base_url.has_value(),
        .base_url = optional_string_to_ffi(init.base_url),
    };
}

static URLPattern::Init init_from_ffi(FFI::RustUrlPatternInit const& init)
{
    URLPattern::Init result;
    if (init.has_protocol)
        result.protocol = string_from_ffi(init.protocol);
    if (init.has_username)
        result.username = string_from_ffi(init.username);
    if (init.has_password)
        result.password = string_from_ffi(init.password);
    if (init.has_hostname)
        result.hostname = string_from_ffi(init.hostname);
    if (init.has_port)
        result.port = string_from_ffi(init.port);
    if (init.has_pathname)
        result.pathname = string_from_ffi(init.pathname);
    if (init.has_search)
        result.search = string_from_ffi(init.search);
    if (init.has_hash)
        result.hash = string_from_ffi(init.hash);
    if (init.has_base_url)
        result.base_url = string_from_ffi(init.base_url);
    return result;
}

static URLPattern::Component::Result component_result_from_ffi(FFI::RustUrlPatternComponentResult const& result)
{
    URLPattern::Component::Result component_result;
    component_result.input = string_from_ffi(result.input);

    for (size_t i = 0; i < result.group_count; ++i) {
        auto const& group = result.groups[i];
        auto name = string_from_ffi(group.name);
        if (group.has_value)
            component_result.groups.set(move(name), string_from_ffi(group.value));
        else
            component_result.groups.set(move(name), Empty {});
    }

    return component_result;
}

static URLPattern::Result result_from_ffi(FFI::RustUrlPatternExecResult const& result)
{
    URLPattern::Result converted;

    converted.inputs.ensure_capacity(result.input_count);
    for (size_t i = 0; i < result.input_count; ++i) {
        auto const& input = result.inputs[i];
        if (input.is_string)
            converted.inputs.unchecked_append(string_from_ffi(input.string));
        else
            converted.inputs.unchecked_append(init_from_ffi(input.init));
    }

    converted.protocol = component_result_from_ffi(result.protocol);
    converted.username = component_result_from_ffi(result.username);
    converted.password = component_result_from_ffi(result.password);
    converted.hostname = component_result_from_ffi(result.hostname);
    converted.port = component_result_from_ffi(result.port);
    converted.pathname = component_result_from_ffi(result.pathname);
    converted.search = component_result_from_ffi(result.search);
    converted.hash = component_result_from_ffi(result.hash);

    return converted;
}

static String take_error(unsigned char const* error_ptr, size_t error_len)
{
    auto error = MUST(String::from_utf8({ reinterpret_cast<char const*>(error_ptr), error_len }));
    FFI::rust_url_pattern_free_error(const_cast<unsigned char*>(error_ptr), error_len);
    return error;
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
            result.string_data = reinterpret_cast<u8 const*>(str.bytes().data());
            result.string_length = str.bytes().size();
        });
    return result;
}

static Optional<Host> host_from_ffi(FFI::FfiUrlHost const& ffi)
{
    if (!ffi.has_host)
        return {};

    switch (ffi.kind) {
    case FFI::RustUrlHostKind::String:
        return Host(string_from_ffi({ ffi.string_data, ffi.string_length }));
    case FFI::RustUrlHostKind::Ipv4: {
        u32 const n = (static_cast<u32>(ffi.ipv4[0]) << 24)
            | (static_cast<u32>(ffi.ipv4[1]) << 16)
            | (static_cast<u32>(ffi.ipv4[2]) << 8)
            | static_cast<u32>(ffi.ipv4[3]);
        return Host(IPv4Address(NetworkOrdered<u32>(n)));
    }
    case FFI::RustUrlHostKind::Ipv6: {
        Array<u16, 8> pieces;
        for (size_t i = 0; i < 8; i++)
            pieces[i] = (static_cast<u16>(ffi.ipv6[i * 2]) << 8) | ffi.ipv6[(i * 2) + 1];
        return Host(IPv6Address(pieces));
    }
    }

    VERIFY_NOT_REACHED();
}

static Optional<URL> url_from_ffi(FFI::RustFfiUrl const& ffi)
{
    URL url;
    url.set_scheme(string_from_ffi(ffi.scheme));
    url.set_username(string_from_ffi(ffi.username));
    url.set_password(string_from_ffi(ffi.password));

    if (auto host = host_from_ffi(ffi.host); host.has_value())
        url.set_host(host.release_value());

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

URLPattern::Impl::~Impl()
{
    if (rust_url_pattern)
        rust_url_pattern_free(rust_url_pattern);
}

struct ExecCallbackContext {
    Optional<URLPattern::Result>* result;
};

static void on_exec_complete(void* ctx_ptr, FFI::RustUrlPatternExecResult const* ffi_result)
{
    auto* ctx = static_cast<ExecCallbackContext*>(ctx_ptr);
    if (!ffi_result)
        return;
    *ctx->result = result_from_ffi(*ffi_result);
}

URLPattern::ErrorOr<URLPattern> URLPattern::create(Input const& input, Optional<String> const& base_url, FFI::IgnoreCase ignore_case)
{
    unsigned char const* error_ptr = nullptr;
    size_t error_len = 0;
    FFI::RustUrlPattern* rust_url_pattern = nullptr;

    auto ffi_ignore_case = ignore_case == FFI::IgnoreCase::Yes ? FFI::IgnoreCase::Yes : FFI::IgnoreCase::No;
    auto base_url_slice = optional_string_to_ffi(base_url);
    if (auto const* input_string = input.get_pointer<String>()) {
        auto input_string_view = input_string->bytes_as_string_view();
        rust_url_pattern = rust_url_pattern_create_from_string(
            reinterpret_cast<u8 const*>(input_string_view.characters_without_null_termination()),
            input_string_view.length(),
            base_url.has_value() ? base_url_slice.data : nullptr,
            base_url_slice.length,
            ffi_ignore_case,
            &error_ptr,
            &error_len);
    } else {
        VERIFY(input.has<Init>());
        if (base_url.has_value())
            return ErrorInfo { "Constructor with URLPatternInit should provide no baseURL"_string };

        auto ffi_init = init_to_ffi(input.get<Init>());
        rust_url_pattern = rust_url_pattern_create_from_init(&ffi_init, ffi_ignore_case, &error_ptr, &error_len);
    }

    if (!rust_url_pattern) {
        auto error = error_ptr ? take_error(error_ptr, error_len) : "Failed to create URLPattern"_string;
        return ErrorInfo { move(error) };
    }

    bool has_regexp_groups = rust_url_pattern_has_regexp_groups(rust_url_pattern);

    auto impl = make<Impl>();
    impl->rust_url_pattern = rust_url_pattern;
    URLPattern pattern;
    pattern.m_impl = move(impl);
    pattern.m_has_regexp_groups = has_regexp_groups;
    pattern.m_protocol_component.pattern_string = string_from_ffi(rust_url_pattern_component_pattern_string(rust_url_pattern, FFI::RustUrlPatternComponent::Protocol));
    pattern.m_username_component.pattern_string = string_from_ffi(rust_url_pattern_component_pattern_string(rust_url_pattern, FFI::RustUrlPatternComponent::Username));
    pattern.m_password_component.pattern_string = string_from_ffi(rust_url_pattern_component_pattern_string(rust_url_pattern, FFI::RustUrlPatternComponent::Password));
    pattern.m_hostname_component.pattern_string = string_from_ffi(rust_url_pattern_component_pattern_string(rust_url_pattern, FFI::RustUrlPatternComponent::Hostname));
    pattern.m_port_component.pattern_string = string_from_ffi(rust_url_pattern_component_pattern_string(rust_url_pattern, FFI::RustUrlPatternComponent::Port));
    pattern.m_pathname_component.pattern_string = string_from_ffi(rust_url_pattern_component_pattern_string(rust_url_pattern, FFI::RustUrlPatternComponent::Pathname));
    pattern.m_search_component.pattern_string = string_from_ffi(rust_url_pattern_component_pattern_string(rust_url_pattern, FFI::RustUrlPatternComponent::Search));
    pattern.m_hash_component.pattern_string = string_from_ffi(rust_url_pattern_component_pattern_string(rust_url_pattern, FFI::RustUrlPatternComponent::Hash));
    return pattern;
}

URLPattern::ErrorOr<Optional<URLPattern::Result>> URLPattern::match(Input const& input, Optional<String> const& base_url_string) const
{
    Optional<Result> result;
    ExecCallbackContext callback_context { .result = &result };
    unsigned char const* error_ptr = nullptr;
    size_t error_len = 0;

    bool did_succeed = false;
    auto base_url_slice = optional_string_to_ffi(base_url_string);
    if (auto const* input_string = input.get_pointer<String>()) {
        auto input_string_view = input_string->bytes_as_string_view();
        did_succeed = rust_url_pattern_exec_string(
            m_impl->rust_url_pattern,
            reinterpret_cast<u8 const*>(input_string_view.characters_without_null_termination()),
            input_string_view.length(),
            base_url_string.has_value() ? base_url_slice.data : nullptr,
            base_url_slice.length,
            &error_ptr,
            &error_len,
            &callback_context,
            on_exec_complete);
    } else {
        VERIFY(input.has<Init>());
        if (base_url_string.has_value())
            return ErrorInfo { "Base URL cannot be provided when URLPatternInput is provided"_string };

        auto ffi_init = init_to_ffi(input.get<Init>());
        did_succeed = rust_url_pattern_exec_init(m_impl->rust_url_pattern, &ffi_init, &error_ptr, &error_len, &callback_context, on_exec_complete);
    }

    if (!did_succeed) {
        auto error = error_ptr ? take_error(error_ptr, error_len) : "Failed to execute URLPattern"_string;
        return ErrorInfo { move(error) };
    }

    return result;
}

bool URLPattern::has_regexp_groups() const
{
    return m_has_regexp_groups;
}

struct UrlFfiStorage {
    Vector<FFI::RustUrlByteSlice> path_segments;
    FFI::RustFfiUrl ffi_url {};
};

static UrlFfiStorage url_to_ffi(URL const& url)
{
    UrlFfiStorage storage;

    storage.ffi_url.scheme = { reinterpret_cast<u8 const*>(url.scheme().bytes().data()), url.scheme().bytes().size() };
    storage.ffi_url.username = { reinterpret_cast<u8 const*>(url.username().bytes().data()), url.username().bytes().size() };
    storage.ffi_url.password = { reinterpret_cast<u8 const*>(url.password().bytes().data()), url.password().bytes().size() };

    storage.ffi_url.host = host_to_ffi(url.host());

    storage.ffi_url.has_port = url.port().has_value();
    storage.ffi_url.port = url.port().value_or(0);

    storage.path_segments.ensure_capacity(url.paths().size());
    for (auto const& segment : url.paths()) {
        storage.path_segments.unchecked_append({
            reinterpret_cast<u8 const*>(segment.bytes().data()),
            segment.bytes().size(),
        });
    }
    storage.ffi_url.path_segments = storage.path_segments.data();
    storage.ffi_url.path_segment_count = storage.path_segments.size();

    storage.ffi_url.has_opaque_path = url.has_an_opaque_path();

    storage.ffi_url.has_query = url.query().has_value();
    if (url.query().has_value())
        storage.ffi_url.query = { reinterpret_cast<u8 const*>(url.query()->bytes().data()), url.query()->bytes().size() };

    storage.ffi_url.has_fragment = url.fragment().has_value();
    if (url.fragment().has_value())
        storage.ffi_url.fragment = { reinterpret_cast<u8 const*>(url.fragment()->bytes().data()), url.fragment()->bytes().size() };

    return storage;
}

struct ParseCallbackCtx {
    Optional<URL>* result;
    URL* url_inout;
};

struct HostParseCallbackCtx {
    Optional<Host>* result;
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

static void on_parse_host_complete(void* ctx_ptr, FFI::FfiUrlHost const* ffi_result)
{
    auto* ctx = static_cast<HostParseCallbackCtx*>(ctx_ptr);
    if (!ffi_result)
        return;
    *ctx->result = host_from_ffi(*ffi_result);
}

Optional<Host> parse_host(StringView input, bool is_opaque)
{
    // URL parsing expects a scalar-value UTF-8 string, but WTF-8 can be provided.
    auto processed_input = String::from_utf8_with_replacement_character(input, String::WithBOMHandling::No);
    auto processed_input_view = processed_input.bytes_as_string_view();

    Optional<Host> result;
    HostParseCallbackCtx ctx { .result = &result };
    bool const did_succeed = FFI::rust_url_parse_host(
        reinterpret_cast<u8 const*>(processed_input_view.characters_without_null_termination()),
        processed_input_view.length(),
        is_opaque,
        &ctx,
        on_parse_host_complete);
    if (!did_succeed)
        return {};
    return result;
}

Optional<URL> parse_basic_url(StringView input, Optional<URL const&> base_url, URL* url, Optional<Parser::State> state_override, Optional<StringView> encoding)
{
    auto const state_override_from_cpp = [](Parser::State state) {
        return static_cast<FFI::State>(to_underlying(state));
    };

    // URL parsing expects a scalar-value UTF-8 string, but WTF-8 can be provided.
    auto processed_input = String::from_utf8_with_replacement_character(input, String::WithBOMHandling::No);

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
            reinterpret_cast<u8 const*>(encoding.has_value() ? encoding->characters_without_null_termination() : nullptr),
            encoding.value_or(""sv).length(),
        },
    };

    Optional<URL> result;
    ParseCallbackCtx ctx { .result = &result, .url_inout = url };
    auto processed_input_view = processed_input.bytes_as_string_view();
    bool const did_succeed = rust_url_basic_parse(
        reinterpret_cast<u8 const*>(processed_input_view.characters_without_null_termination()),
        processed_input_view.length(),
        &options,
        &ctx,
        on_basic_parse_complete);
    if (!did_succeed)
        return {};
    return result;
}

}
