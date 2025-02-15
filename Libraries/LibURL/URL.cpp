/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/CharacterTypes.h>
#include <AK/Debug.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>

#if defined(ENABLE_PUBLIC_SUFFIX)
#    include <LibURL/PublicSuffixData.h>
#endif

namespace URL {

// FIXME: It could make sense to force users of URL to use URL::Parser::basic_parse() explicitly instead of using a constructor.
URL::URL(StringView string)
    : URL(Parser::basic_parse(string).value_or(URL {}))
{
    if constexpr (URL_PARSER_DEBUG) {
        if (m_data->valid)
            dbgln("URL constructor: Parsed URL to be '{}'.", serialize());
        else
            dbgln("URL constructor: Parsed URL to be invalid.");
    }
}

Optional<URL> URL::complete_url(StringView relative_url) const
{
    if (!is_valid())
        return {};

    return Parser::basic_parse(relative_url, *this);
}

ByteString URL::path_segment_at_index(size_t index) const
{
    VERIFY(index < path_segment_count());
    return percent_decode(m_data->paths[index]);
}

ByteString URL::basename() const
{
    if (!m_data->valid)
        return {};
    if (m_data->paths.is_empty())
        return {};
    auto& last_segment = m_data->paths.last();
    return percent_decode(last_segment);
}

void URL::set_scheme(String scheme)
{
    m_data->scheme = move(scheme);
    m_data->valid = compute_validity();
}

// https://url.spec.whatwg.org/#set-the-username
void URL::set_username(StringView username)
{
    // To set the username given a url and username, set url’s username to the result of running UTF-8 percent-encode on username using the userinfo percent-encode set.
    m_data->username = percent_encode(username, PercentEncodeSet::Userinfo);
    m_data->valid = compute_validity();
}

// https://url.spec.whatwg.org/#set-the-password
void URL::set_password(StringView password)
{
    // To set the password given a url and password, set url’s password to the result of running UTF-8 percent-encode on password using the userinfo percent-encode set.
    m_data->password = percent_encode(password, PercentEncodeSet::Userinfo);
    m_data->valid = compute_validity();
}

void URL::set_host(Host host)
{
    m_data->host = move(host);
    m_data->valid = compute_validity();
}

// https://url.spec.whatwg.org/#concept-host-serializer
String URL::serialized_host() const
{
    return m_data->host->serialize();
}

void URL::set_port(Optional<u16> port)
{
    if (port == default_port_for_scheme(m_data->scheme)) {
        m_data->port = {};
        return;
    }
    m_data->port = move(port);
    m_data->valid = compute_validity();
}

void URL::set_paths(Vector<ByteString> const& paths)
{
    m_data->paths.clear_with_capacity();
    m_data->paths.ensure_capacity(paths.size());
    for (auto const& segment : paths)
        m_data->paths.unchecked_append(percent_encode(segment, PercentEncodeSet::Path));
    m_data->valid = compute_validity();
}

void URL::append_path(StringView path)
{
    m_data->paths.append(percent_encode(path, PercentEncodeSet::Path));
}

// https://url.spec.whatwg.org/#cannot-have-a-username-password-port
bool URL::cannot_have_a_username_or_password_or_port() const
{
    // A URL cannot have a username/password/port if its host is null or the empty string, or its scheme is "file".

    return !m_data->host.has_value() || m_data->host->is_empty_host() || m_data->scheme == "file"sv;
}

// FIXME: This is by no means complete.
// NOTE: This relies on some assumptions about how the spec-defined URL parser works that may turn out to be wrong.
bool URL::compute_validity() const
{
    if (m_data->scheme.is_empty())
        return false;

    if (m_data->cannot_be_a_base_url) {
        if (m_data->paths.size() != 1)
            return false;
        if (m_data->paths[0].is_empty())
            return false;
    } else {
        if (m_data->scheme.is_one_of("about", "mailto"))
            return false;
        // NOTE: Maybe it is allowed to have a zero-segment path.
        if (m_data->paths.size() == 0)
            return false;
    }

    // FIXME: A file URL's host should be the empty string for localhost, not null.
    if (m_data->scheme == "file" && !m_data->host.has_value())
        return false;

    return true;
}

// https://url.spec.whatwg.org/#default-port
Optional<u16> default_port_for_scheme(StringView scheme)
{
    // Spec defined mappings with port:
    if (scheme == "ftp")
        return 21;
    if (scheme == "http")
        return 80;
    if (scheme == "https")
        return 443;
    if (scheme == "ws")
        return 80;
    if (scheme == "wss")
        return 443;

    // NOTE: not in spec, but we support these too
    if (scheme == "irc")
        return 6667;
    if (scheme == "ircs")
        return 6697;

    return {};
}

URL create_with_file_scheme(ByteString const& path, ByteString const& fragment, ByteString const& hostname)
{
    LexicalPath lexical_path(path);
    if (!lexical_path.is_absolute())
        return {};

    URL url;
    url.set_scheme("file"_string);
    url.set_host(hostname == "localhost" ? String {} : String::from_byte_string(hostname).release_value_but_fixme_should_propagate_errors());
    url.set_paths(lexical_path.parts());
    if (path.ends_with('/'))
        url.append_slash();
    if (!fragment.is_empty())
        url.set_fragment(String::from_byte_string(fragment).release_value_but_fixme_should_propagate_errors());
    return url;
}

URL create_with_url_or_path(ByteString const& url_or_path)
{
    URL url = url_or_path;
    if (url.is_valid())
        return url;

    ByteString path = LexicalPath::canonicalized_path(url_or_path);
    return create_with_file_scheme(path);
}

URL create_with_data(StringView mime_type, StringView payload, bool is_base64)
{
    URL url;
    url.set_cannot_be_a_base_url(true);
    url.set_scheme("data"_string);

    StringBuilder builder;
    builder.append(mime_type);
    if (is_base64)
        builder.append(";base64"sv);
    builder.append(',');
    builder.append(payload);
    url.set_paths({ builder.to_byte_string() });
    return url;
}

// https://url.spec.whatwg.org/#special-scheme
bool is_special_scheme(StringView scheme)
{
    return scheme.is_one_of("ftp", "file", "http", "https", "ws", "wss");
}

// https://url.spec.whatwg.org/#url-path-serializer
String URL::serialize_path() const
{
    // 1. If url has an opaque path, then return url’s path.
    // FIXME: Reimplement this step once we modernize the URL implementation to meet the spec.
    if (cannot_be_a_base_url())
        return m_data->paths[0];

    // 2. Let output be the empty string.
    StringBuilder output;

    // 3. For each segment of url’s path: append U+002F (/) followed by segment to output.
    for (auto const& segment : m_data->paths) {
        output.append('/');
        output.append(segment);
    }

    // 4. Return output.
    return output.to_string_without_validation();
}

// https://url.spec.whatwg.org/#concept-url-serializer
String URL::serialize(ExcludeFragment exclude_fragment) const
{
    // 1. Let output be url’s scheme and U+003A (:) concatenated.
    StringBuilder output;
    output.append(m_data->scheme);
    output.append(':');

    // 2. If url’s host is non-null:
    if (m_data->host.has_value()) {
        // 1. Append "//" to output.
        output.append("//"sv);

        // 2. If url includes credentials, then:
        if (includes_credentials()) {
            // 1. Append url’s username to output.
            output.append(m_data->username);

            // 2. If url’s password is not the empty string, then append U+003A (:), followed by url’s password, to output.
            if (!m_data->password.is_empty()) {
                output.append(':');
                output.append(m_data->password);
            }

            // 3. Append U+0040 (@) to output.
            output.append('@');
        }

        // 3. Append url’s host, serialized, to output.
        output.append(serialized_host());

        // 4. If url’s port is non-null, append U+003A (:) followed by url’s port, serialized, to output.
        if (m_data->port.has_value())
            output.appendff(":{}", *m_data->port);
    }

    // 3. If url’s host is null, url does not have an opaque path, url’s path’s size is greater than 1, and url’s path[0] is the empty string, then append U+002F (/) followed by U+002E (.) to output.
    // 4. Append the result of URL path serializing url to output.
    // FIXME: Implement this closer to spec steps.
    if (cannot_be_a_base_url()) {
        output.append(m_data->paths[0]);
    } else {
        if (!m_data->host.has_value() && m_data->paths.size() > 1 && m_data->paths[0].is_empty())
            output.append("/."sv);
        for (auto& segment : m_data->paths) {
            output.append('/');
            output.append(segment);
        }
    }

    // 5. If url’s query is non-null, append U+003F (?), followed by url’s query, to output.
    if (m_data->query.has_value()) {
        output.append('?');
        output.append(*m_data->query);
    }

    // 6. If exclude fragment is false and url’s fragment is non-null, then append U+0023 (#), followed by url’s fragment, to output.
    if (exclude_fragment == ExcludeFragment::No && m_data->fragment.has_value()) {
        output.append('#');
        output.append(*m_data->fragment);
    }

    // 7. Return output.
    return output.to_string_without_validation();
}

// https://url.spec.whatwg.org/#url-rendering
// NOTE: This does e.g. not display credentials.
// FIXME: Parts of the URL other than the host should have their sequences of percent-encoded bytes replaced with code points
//        resulting from percent-decoding those sequences converted to bytes, unless that renders those sequences invisible.
ByteString URL::serialize_for_display() const
{
    VERIFY(m_data->valid);

    StringBuilder builder;
    builder.append(m_data->scheme);
    builder.append(':');

    if (m_data->host.has_value()) {
        builder.append("//"sv);
        builder.append(serialized_host());
        if (m_data->port.has_value())
            builder.appendff(":{}", *m_data->port);
    }

    if (cannot_be_a_base_url()) {
        builder.append(m_data->paths[0]);
    } else {
        if (!m_data->host.has_value() && m_data->paths.size() > 1 && m_data->paths[0].is_empty())
            builder.append("/."sv);
        for (auto& segment : m_data->paths) {
            builder.append('/');
            builder.append(segment);
        }
    }

    if (m_data->query.has_value()) {
        builder.append('?');
        builder.append(*m_data->query);
    }

    if (m_data->fragment.has_value()) {
        builder.append('#');
        builder.append(*m_data->fragment);
    }

    return builder.to_byte_string();
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
            return Origin {};

        // 4. If pathURL’s scheme is "http", "https", or "file", then return pathURL’s origin.
        if (path_url->scheme().is_one_of("http"sv, "https"sv, "file"sv))
            return path_url->origin();

        // 5. Return a new opaque origin.
        return Origin {};
    }

    // -> "ftp"
    // -> "http"
    // -> "https"
    // -> "ws"
    // -> "wss"
    if (scheme().is_one_of("ftp"sv, "http"sv, "https"sv, "ws"sv, "wss"sv)) {
        // Return the tuple origin (url’s scheme, url’s host, url’s port, null).
        return Origin(scheme(), host().value(), port());
    }

    // -> "file"
    // AD-HOC: Our resource:// is basically an alias to file://
    if (scheme() == "file"sv || scheme() == "resource"sv) {
        // Unfortunate as it is, this is left as an exercise to the reader. When in doubt, return a new opaque origin.
        // Note: We must return an origin with the `file://' protocol for `file://' iframes to work from `file://' pages.
        return Origin(scheme(), String {}, {});
    }

    // -> Otherwise
    // Return a new opaque origin.
    return Origin {};
}

bool URL::equals(URL const& other, ExcludeFragment exclude_fragments) const
{
    if (this == &other)
        return true;
    if (!m_data->valid || !other.m_data->valid)
        return false;
    return serialize(exclude_fragments) == other.serialize(exclude_fragments);
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
        return code_point_is_in_percent_encode_set(code_point, PercentEncodeSet::Query) || "?`{}"sv.contains(static_cast<char>(code_point));
    case PercentEncodeSet::Userinfo:
        return code_point_is_in_percent_encode_set(code_point, PercentEncodeSet::Path) || "/:;=@[\\]^|"sv.contains(static_cast<char>(code_point));
    case PercentEncodeSet::Component:
        return code_point_is_in_percent_encode_set(code_point, PercentEncodeSet::Userinfo) || "$%&+,"sv.contains(static_cast<char>(code_point));
    case PercentEncodeSet::ApplicationXWWWFormUrlencoded:
        return code_point_is_in_percent_encode_set(code_point, PercentEncodeSet::Component) || "!'()~"sv.contains(static_cast<char>(code_point));
    case PercentEncodeSet::EncodeURI:
        // NOTE: This is the same percent encode set that JS encodeURI() uses.
        // https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/encodeURI
        return code_point > 0x7E || (!is_ascii_alphanumeric(code_point) && !";,/?:@&=+$-_.!~*'()#"sv.contains(static_cast<char>(code_point)));
    default:
        VERIFY_NOT_REACHED();
    }
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

URL URL::about(String path)
{
    URL url;
    url.m_data->valid = true;
    url.m_data->scheme = "about"_string;
    url.m_data->paths = { move(path) };
    url.m_data->cannot_be_a_base_url = true;
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

bool is_public_suffix([[maybe_unused]] StringView host)
{
#if defined(ENABLE_PUBLIC_SUFFIX)
    return PublicSuffixData::the()->is_public_suffix(host);
#else
    return false;
#endif
}

Optional<String> get_public_suffix([[maybe_unused]] StringView host)
{
#if defined(ENABLE_PUBLIC_SUFFIX)
    return MUST(PublicSuffixData::the()->get_public_suffix(host));
#else
    return {};
#endif
}

}
