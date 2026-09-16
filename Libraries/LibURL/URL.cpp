/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2023-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/CharacterTypes.h>
#include <AK/Debug.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16View.h>
#include <AK/Utf8View.h>
#include <LibURL/Parser.h>
#include <LibURL/PublicSuffixData.h>
#include <LibURL/RustIntegration.h>

namespace URL {

static bool s_file_scheme_urls_have_tuple_origins = false;

Optional<URL> URL::complete_url(StringView relative_url) const
{
    return Parser::basic_parse(relative_url, *this);
}

URL::PathSegments::Iterator& URL::PathSegments::Iterator::operator++()
{
    m_start = segment_end() + 1;
    return *this;
}

size_t URL::PathSegments::Iterator::segment_end() const
{
    if (m_is_opaque)
        return m_path.length();
    return m_path.find('/', m_start).value_or(m_path.length());
}

size_t URL::PathSegments::size() const
{
    if (m_is_opaque)
        return 1;
    return m_path.count("/"sv);
}

StringView URL::PathSegments::first() const
{
    VERIFY(!is_empty());
    return *begin();
}

StringView URL::PathSegments::last() const
{
    VERIFY(!is_empty());
    if (m_is_opaque)
        return m_path;
    return m_path.substring_view(*m_path.find_last('/') + 1);
}

StringView URL::username() const
{
    if (m_components.host_kind == FFI::HostKind::Null)
        return {};
    return view(m_components.scheme_end + 3, m_components.username_end);
}

StringView URL::password() const
{
    if (m_components.host_kind == FFI::HostKind::Null || m_components.username_end + 1 >= m_components.host_start)
        return {};
    return view(m_components.username_end + 1, m_components.host_start - 1);
}

// https://url.spec.whatwg.org/#include-credentials
bool URL::includes_credentials() const
{
    // A URL includes credentials if its username or password is not the empty string.
    return m_components.host_kind != FFI::HostKind::Null && m_components.host_start > m_components.scheme_end + 3;
}

Optional<Host> URL::host() const
{
    auto rust_url = to_rust();
    return RustIntegration::host_from_ffi(FFI::rust_url_host(&rust_url));
}

// https://url.spec.whatwg.org/#concept-host-serializer
StringView URL::serialized_host() const
{
    VERIFY(m_components.host_kind != FFI::HostKind::Null);
    return view(m_components.host_start, m_components.host_end);
}

Optional<u16> URL::port() const
{
    if (!m_components.has_port)
        return {};
    return m_components.port;
}

u32 URL::path_end() const
{
    if (m_components.query_start != FFI::URL_OFFSET_NONE)
        return m_components.query_start;
    if (m_components.fragment_start != FFI::URL_OFFSET_NONE)
        return m_components.fragment_start;
    return m_serialization.bytes().size();
}

Optional<StringView> URL::query() const
{
    if (m_components.query_start == FFI::URL_OFFSET_NONE)
        return {};
    auto query_end = m_components.fragment_start != FFI::URL_OFFSET_NONE ? m_components.fragment_start : m_serialization.bytes().size();
    return view(m_components.query_start + 1, query_end);
}

Optional<StringView> URL::fragment() const
{
    if (m_components.fragment_start == FFI::URL_OFFSET_NONE)
        return {};
    return view(m_components.fragment_start + 1, m_serialization.bytes().size());
}

ByteString URL::path_segment_at_index(size_t index) const
{
    for (auto segment : path_segments()) {
        if (index-- == 0)
            return percent_decode(segment);
    }
    VERIFY_NOT_REACHED();
}

ByteString URL::basename() const
{
    auto segments = path_segments();
    if (segments.is_empty())
        return {};
    return percent_decode(segments.last());
}

FFI::RustUrl URL::to_rust() const
{
    return {
        .serialization = { m_serialization.bytes().data(), m_serialization.bytes().size() },
        .components = m_components,
    };
}

void URL::set_from_rust(FFI::RustUrl const& rust_url)
{
    m_serialization = String::from_ascii_without_validation({ rust_url.serialization.data, rust_url.serialization.length });
    m_components = rust_url.components;
}

void URL::set_from_rust_callback(void* url, FFI::RustUrl const* rust_url)
{
    VERIFY(rust_url);
    static_cast<URL*>(url)->set_from_rust(*rust_url);
}

static FFI::RustUrlInput rust_url_input(StringView input)
{
    return { reinterpret_cast<u8 const*>(input.characters_without_null_termination()), nullptr, input.length() };
}

static FFI::RustUrlByteSlice rust_byte_slice(Optional<StringView> string)
{
    if (!string.has_value())
        return { nullptr, 0 };
    // NB: A null data pointer means null, so an empty string needs a non-null one.
    return { string->is_empty() ? reinterpret_cast<u8 const*>("") : reinterpret_cast<u8 const*>(string->characters_without_null_termination()), string->length() };
}

void URL::set_scheme(StringView scheme)
{
    auto rust_url = to_rust();
    FFI::rust_url_set_scheme(&rust_url, rust_byte_slice(scheme), this, set_from_rust_callback);
}

// https://url.spec.whatwg.org/#set-the-username
void URL::set_username(StringView username)
{
    auto rust_url = to_rust();
    auto processed_username = String::from_utf8_with_replacement_character(username, String::WithBOMHandling::No);
    FFI::rust_url_set_username(&rust_url, rust_url_input(processed_username.bytes_as_string_view()), this, set_from_rust_callback);
}

// https://url.spec.whatwg.org/#set-the-username
void URL::set_username(Utf16View username)
{
    auto rust_url = to_rust();
    FFI::rust_url_set_username(&rust_url, RustIntegration::rust_url_input(username), this, set_from_rust_callback);
}

// https://url.spec.whatwg.org/#set-the-password
void URL::set_password(StringView password)
{
    auto rust_url = to_rust();
    auto processed_password = String::from_utf8_with_replacement_character(password, String::WithBOMHandling::No);
    FFI::rust_url_set_password(&rust_url, rust_url_input(processed_password.bytes_as_string_view()), this, set_from_rust_callback);
}

// https://url.spec.whatwg.org/#set-the-password
void URL::set_password(Utf16View password)
{
    auto rust_url = to_rust();
    FFI::rust_url_set_password(&rust_url, RustIntegration::rust_url_input(password), this, set_from_rust_callback);
}

void URL::set_host(Host const& host)
{
    auto kind = host.value().visit(
        [](IPv4Address const&) { return FFI::HostKind::Ipv4; },
        [](IPv6Address const&) { return FFI::HostKind::Ipv6; },
        [](String const&) { return FFI::HostKind::Domain; },
        [](OpaqueHost const&) { return FFI::HostKind::Opaque; });
    auto serialized_host = host.serialize();
    auto rust_url = to_rust();
    FFI::rust_url_set_host(&rust_url, kind, rust_byte_slice(serialized_host.bytes_as_string_view()), this, set_from_rust_callback);
}

void URL::set_path(ReadonlySpan<StringView> percent_encoded_segments)
{
    Vector<FFI::RustUrlByteSlice, 8> segments;
    segments.ensure_capacity(percent_encoded_segments.size());
    for (auto segment : percent_encoded_segments)
        segments.unchecked_append(rust_byte_slice(segment));
    auto rust_url = to_rust();
    FFI::rust_url_set_path(&rust_url, segments.data(), segments.size(), this, set_from_rust_callback);
}

void URL::set_opaque_path(StringView percent_encoded_path)
{
    auto rust_url = to_rust();
    FFI::rust_url_set_opaque_path(&rust_url, rust_byte_slice(percent_encoded_path), this, set_from_rust_callback);
}

void URL::set_port(Optional<u16> port)
{
    auto rust_url = to_rust();
    FFI::rust_url_set_port(&rust_url, port.has_value(), port.value_or(0), this, set_from_rust_callback);
}

void URL::set_query(Optional<StringView> query)
{
    if (!query.has_value() && m_components.query_start == FFI::URL_OFFSET_NONE)
        return;
    auto rust_url = to_rust();
    FFI::rust_url_set_query(&rust_url, rust_byte_slice(query), this, set_from_rust_callback);
}

void URL::set_fragment(Optional<StringView> fragment)
{
    if (!fragment.has_value() && m_components.fragment_start == FFI::URL_OFFSET_NONE)
        return;
    auto rust_url = to_rust();
    FFI::rust_url_set_fragment(&rust_url, rust_byte_slice(fragment), this, set_from_rust_callback);
}

Optional<BlobURLEntry const&> URL::blob_url_entry() const
{
    if (!m_blob_url_entry)
        return {};
    return m_blob_url_entry->entry;
}

void URL::set_blob_url_entry(Optional<BlobURLEntry> entry)
{
    if (!entry.has_value()) {
        m_blob_url_entry = nullptr;
        return;
    }
    m_blob_url_entry = adopt_ref(*new BlobURLEntryStorage(entry.release_value()));
}

// https://url.spec.whatwg.org/#cannot-have-a-username-password-port
bool URL::cannot_have_a_username_or_password_or_port() const
{
    // A URL cannot have a username/password/port if its host is null or the empty string, or its scheme is "file".
    return m_components.host_kind == FFI::HostKind::Null
        || m_components.host_start == m_components.host_end
        || m_components.scheme_type == FFI::SchemeType::File;
}

// https://url.spec.whatwg.org/#default-port
Optional<u16> default_port_for_scheme(StringView scheme)
{
    if (scheme == "ftp"sv)
        return 21;
    if (scheme == "http"sv)
        return 80;
    if (scheme == "https"sv)
        return 443;
    if (scheme == "ws"sv)
        return 80;
    if (scheme == "wss"sv)
        return 443;
    return {};
}

Optional<URL> create_with_file_scheme(ByteString const& path, ByteString const& fragment, ByteString const& hostname)
{
    LexicalPath lexical_path(path);
    if (!lexical_path.is_absolute())
        return {};

    StringBuilder url_builder;
    url_builder.append("file://"sv);
    url_builder.append(hostname);
    url_builder.append(lexical_path.string());
    if (path.ends_with('/'))
        url_builder.append('/');
    if (!fragment.is_empty()) {
        url_builder.append('#');
        url_builder.append(fragment);
    }

    return Parser::basic_parse(url_builder.string_view());
}

Optional<URL> create_with_url_or_path(ByteString const& url_or_path)
{
    auto url = Parser::basic_parse(url_or_path);
    if (url.has_value())
        return url.release_value();

    ByteString path = LexicalPath::canonicalized_path(url_or_path);
    return create_with_file_scheme(path);
}

URL create_with_data(StringView mime_type, StringView payload, bool is_base64)
{
    StringBuilder builder;
    builder.append(mime_type);
    if (is_base64)
        builder.append(";base64"sv);
    builder.append(',');
    builder.append(payload);
    auto path = percent_encode(builder.string_view(), PercentEncodeSet::Path);
    return Parser::basic_parse(MUST(String::formatted("data:{}", path))).release_value();
}

// https://url.spec.whatwg.org/#special-scheme
ReadonlySpan<StringView> special_schemes()
{
    static auto const schemes = to_array<StringView>({
        "ftp"sv,
        "file"sv,
        "http"sv,
        "https"sv,
        "ws"sv,
        "wss"sv,
    });
    return schemes;
}

// https://url.spec.whatwg.org/#is-special
bool is_special_scheme(StringView scheme)
{
    return special_schemes().contains_slow(scheme);
}

// https://url.spec.whatwg.org/#url-path-serializer
StringView URL::serialize_path() const
{
    return view(m_components.path_start, path_end());
}

// This function is used whenever a path is needed to access the actual file on disk.
// On Windows serialize_path can produce a path like /C:/path/to/tst.htm, so the leading slash needs to be removed to obtain a valid path.
ByteString URL::file_path() const
{
    ByteString path = percent_decode(serialize_path());
#ifdef AK_OS_WINDOWS
    if (path.starts_with('/'))
        path = path.substring(1);
#endif
    return path;
}

// https://url.spec.whatwg.org/#concept-url-serializer
String URL::serialize(ExcludeFragment exclude_fragment) const
{
    if (exclude_fragment == ExcludeFragment::No || m_components.fragment_start == FFI::URL_OFFSET_NONE)
        return m_serialization;
    return MUST(m_serialization.substring_from_byte_offset_with_shared_superstring(0, m_components.fragment_start));
}

// https://url.spec.whatwg.org/#url-rendering
// NOTE: This does e.g. not display credentials.
// FIXME: Parts of the URL other than the host should have their sequences of percent-encoded bytes replaced with code points
//        resulting from percent-decoding those sequences converted to bytes, unless that renders those sequences invisible.
ByteString URL::serialize_for_display() const
{
    if (!includes_credentials())
        return m_serialization.to_byte_string();

    StringBuilder builder;
    builder.append(view(0, m_components.scheme_end + 3));
    builder.append(m_serialization.bytes_as_string_view().substring_view(m_components.host_start));
    return builder.to_byte_string();
}

void set_file_scheme_urls_have_tuple_origins()
{
    VERIFY(!s_file_scheme_urls_have_tuple_origins);
    s_file_scheme_urls_have_tuple_origins = true;
}

bool file_scheme_urls_have_tuple_origins()
{
    return s_file_scheme_urls_have_tuple_origins;
}

// https://url.spec.whatwg.org/#concept-url-origin
Origin URL::origin() const
{
    // The origin of a URL url is the origin returned by running these steps, switching on url’s scheme:
    // -> "blob"
    if (scheme() == "blob"sv) {
        // 1. If url’s blob URL entry is non-null, then return url’s blob URL entry’s environment’s origin.
        if (blob_url_entry().has_value())
            return blob_url_entry()->environment.origin;

        // 2. Let pathURL be the result of parsing the result of URL path serializing url.
        auto path_url = Parser::basic_parse(serialize_path());

        // 3. If pathURL is failure, then return a new opaque origin.
        if (!path_url.has_value())
            return Origin::create_opaque();

        // 4. If pathURL’s scheme is "http", "https", or "file", then return pathURL’s origin.
        if (path_url->scheme().is_one_of("http"sv, "https"sv, "file"sv))
            return path_url->origin();

        // 5. Return a new opaque origin.
        return Origin::create_opaque();
    }

    // -> "ftp"
    // -> "http"
    // -> "https"
    // -> "ws"
    // -> "wss"
    if (scheme().is_one_of("ftp"sv, "http"sv, "https"sv, "ws"sv, "wss"sv)) {
        // Return the tuple origin (url’s scheme, url’s host, url’s port, null).
        return Origin(String::from_ascii_without_validation(scheme().bytes()), host().value(), port());
    }

    // AD-HOC: resource:// URLs are internal browser resources; give them a shared tuple origin
    // so that same-origin checks pass between any two resource:// documents or worker scripts.
    if (scheme() == "resource"sv)
        return Origin("resource"_string, String {}, {});

    // -> "file"
    if (scheme() == "file"sv) {
        // Unfortunate as it is, this is left as an exercise to the reader. When in doubt, return a new opaque origin.

        // Our implementation-defined behavior is to return an opaque origin for file:// URLs,
        // tagged explicitly as a "file" opaque origin rather than a fully anonymous one.
        //
        // This keeps file:// URLs opaque by default while still allowing downstream code to
        // identify and special-case them where needed - for example, to match cases where
        // other browsers treat a file:// origin as if it were a tuple origin.
        //
        // A process-wide flag can opt into tuple origins for file:// URLs instead. This is
        // intended for development/testing scenarios where web features requiring a non-opaque
        // origin (such as localStorage) need to work with file:// pages.
        if (file_scheme_urls_have_tuple_origins())
            return Origin { "file"_string, String {}, {} };

        return Origin::create_opaque(Origin::OpaqueData::Type::File);
    }

    // -> Otherwise
    // Return a new opaque origin.
    return Origin::create_opaque();
}

bool URL::equals(URL const& other, ExcludeFragment exclude_fragments) const
{
    if (exclude_fragments == ExcludeFragment::No)
        return m_serialization == other.m_serialization;

    auto without_fragment = [](URL const& url) {
        auto serialization = url.m_serialization.bytes_as_string_view();
        if (url.m_components.fragment_start == FFI::URL_OFFSET_NONE)
            return serialization;
        return serialization.substring_view(0, url.m_components.fragment_start);
    };
    return without_fragment(*this) == without_fragment(other);
}

void append_percent_encoded(StringBuilder& builder, u32 code_point)
{
    if (code_point <= 0x7f)
        builder.appendff("%{:02X}", code_point);
    else if (code_point <= 0x07ff)
        builder.appendff("%{:02X}%{:02X}", ((code_point >> 6) & 0x1f) | 0xc0, (code_point & 0x3f) | 0x80);
    else if (code_point <= 0xffff)
        builder.appendff("%{:02X}%{:02X}%{:02X}", ((code_point >> 12) & 0x0f) | 0xe0, ((code_point >> 6) & 0x3f) | 0x80, (code_point & 0x3f) | 0x80);
    else if (code_point <= 0x10ffff)
        builder.appendff("%{:02X}%{:02X}%{:02X}%{:02X}", ((code_point >> 18) & 0x07) | 0xf0, ((code_point >> 12) & 0x3f) | 0x80, ((code_point >> 6) & 0x3f) | 0x80, (code_point & 0x3f) | 0x80);
    else
        VERIFY_NOT_REACHED();
}

// https://url.spec.whatwg.org/#c0-control-percent-encode-set
bool code_point_is_in_percent_encode_set(u32 code_point, PercentEncodeSet set)
{
    // NOTE: Once we've checked for presence in the C0Control set, we know that the code point is
    //       a valid ASCII character in the range 0x20..0x7E, so we can safely cast it to char.
    switch (set) {
    case PercentEncodeSet::C0Control:
        return code_point < 0x20 || code_point > 0x7E;
    case PercentEncodeSet::Fragment:
        return code_point_is_in_percent_encode_set(code_point, PercentEncodeSet::C0Control) || " \"<>`"sv.contains(static_cast<char>(code_point));
    case PercentEncodeSet::Query:
        return code_point_is_in_percent_encode_set(code_point, PercentEncodeSet::C0Control) || " \"#<>"sv.contains(static_cast<char>(code_point));
    case PercentEncodeSet::SpecialQuery:
        return code_point_is_in_percent_encode_set(code_point, PercentEncodeSet::Query) || code_point == '\'';
    case PercentEncodeSet::Path:
        return code_point_is_in_percent_encode_set(code_point, PercentEncodeSet::Query) || "?^`{}"sv.contains(static_cast<char>(code_point));
    case PercentEncodeSet::Userinfo:
        return code_point_is_in_percent_encode_set(code_point, PercentEncodeSet::Path) || "/:;=@[\\]|"sv.contains(static_cast<char>(code_point));
    case PercentEncodeSet::Component:
        return code_point_is_in_percent_encode_set(code_point, PercentEncodeSet::Userinfo) || "$%&+,"sv.contains(static_cast<char>(code_point));
    case PercentEncodeSet::ApplicationXWWWFormUrlencoded:
        return code_point_is_in_percent_encode_set(code_point, PercentEncodeSet::Component) || "!'()~"sv.contains(static_cast<char>(code_point));
    }
    VERIFY_NOT_REACHED();
}

void append_percent_encoded_if_necessary(StringBuilder& builder, u32 code_point, PercentEncodeSet set)
{
    if (code_point_is_in_percent_encode_set(code_point, set))
        append_percent_encoded(builder, code_point);
    else
        builder.append_code_point(code_point);
}

String percent_encode(StringView input, PercentEncodeSet set, SpaceAsPlus space_as_plus)
{
    StringBuilder builder;
    for (auto code_point : Utf8View(input)) {
        if (space_as_plus == SpaceAsPlus::Yes && code_point == ' ')
            builder.append('+');
        else
            append_percent_encoded_if_necessary(builder, code_point, set);
    }
    return MUST(builder.to_string());
}

String percent_encode(Utf16View input, PercentEncodeSet set, SpaceAsPlus space_as_plus)
{
    StringBuilder builder;
    for (auto code_point : input) {
        if (space_as_plus == SpaceAsPlus::Yes && code_point == ' ')
            builder.append('+');
        else
            append_percent_encoded_if_necessary(builder, code_point, set);
    }
    return MUST(builder.to_string());
}

URL URL::about(StringView path)
{
    VERIFY(!path.contains('?') && !path.contains('#'));

    URL url;
    url.m_serialization = MUST(String::formatted("about:{}", path));
    url.m_components.scheme_end = "about"sv.length();
    url.m_components.username_end = url.m_components.scheme_end + 1;
    url.m_components.host_start = url.m_components.username_end;
    url.m_components.host_end = url.m_components.username_end;
    url.m_components.path_start = url.m_components.username_end;
    url.m_components.has_opaque_path = true;
    return url;
}

// https://url.spec.whatwg.org/#percent-decode
ByteString percent_decode(StringView input)
{
    if (!input.contains('%'))
        return input;

    // 1. Let output be an empty byte sequence.
    StringBuilder builder;

    // 2. For each byte byte in input:
    for (size_t i = 0; i < input.length(); ++i) {
        // 1. If byte is not 0x25 (%), then append byte to output.
        if (input[i] != '%') {
            builder.append(input[i]);
        }
        // 2. Otherwise, if byte is 0x25 (%) and the next two bytes after byte in input are not in the ranges 0x30 (0)
        //    to 0x39 (9), 0x41 (A) to 0x46 (F), and 0x61 (a) to 0x66 (f), all inclusive, append byte to output.
        else if (i + 2 >= input.length() || !is_ascii_hex_digit(input[i + 1]) || !is_ascii_hex_digit(input[i + 2])) {
            builder.append(input[i]);
        }
        // 3. Otherwise:
        else {
            // 1. Let bytePoint be the two bytes after byte in input, decoded, and then interpreted as hexadecimal number.
            u8 byte_point = (parse_ascii_hex_digit(input[i + 1]) << 4) | parse_ascii_hex_digit(input[i + 2]);

            // 2. Append a byte whose value is bytePoint to output.
            builder.append(byte_point);

            // 3. Skip the next two bytes in input.
            i += 2;
        }
    }
    return builder.to_byte_string();
}

}
