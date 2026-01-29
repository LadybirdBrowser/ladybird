/*
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2023-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IPv4Address.h>
#include <AK/IPv6Address.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/Cookie/Cookie.h>

namespace Web::Cookie {

static String time_to_string(UnixDateTime const& time)
{
    return MUST(time.to_string("%Y-%m-%d %H:%M:%S %Z"sv));
}

String Cookie::creation_time_to_string() const
{
    return time_to_string(creation_time);
}

String Cookie::last_access_time_to_string() const
{
    return time_to_string(last_access_time);
}

String Cookie::expiry_time_to_string() const
{
    return time_to_string(expiry_time);
}

StringView same_site_to_string(SameSite same_site)
{
    switch (same_site) {
    case SameSite::Default:
        return "Default"sv;
    case SameSite::None:
        return "None"sv;
    case SameSite::Lax:
        return "Lax"sv;
    case SameSite::Strict:
        return "Strict"sv;
    }
    VERIFY_NOT_REACHED();
}

SameSite same_site_from_string(StringView same_site_mode)
{
    if (same_site_mode.equals_ignoring_ascii_case("None"sv))
        return SameSite::None;
    if (same_site_mode.equals_ignoring_ascii_case("Strict"sv))
        return SameSite::Strict;
    if (same_site_mode.equals_ignoring_ascii_case("Lax"sv))
        return SameSite::Lax;
    return SameSite::Default;
}

// https://datatracker.ietf.org/doc/html/draft-ietf-httpbis-rfc6265bis-22#section-5.1.2
Optional<String> canonicalize_domain(URL::URL const& url)
{
    if (!url.host().has_value())
        return {};

    // 1. Convert the host name to a sequence of individual domain name labels.
    // 2. All labels must be one of U-label, A-label, or Non-Reserved LDH (NR-LDH) label (see Section 2.3.1 of [RFC5890]).
    //    If any label is not one of these then abort this algorithm and fail to canonicalize the host name.
    // 3. Convert each U-label to an A-label (see Section 2.3.2.1 of [RFC5890]).
    // 4. If any label is a Fake A-label then abort this algorithm and fail to canonicalize the host name.
    // 5. Concatenate the resulting labels, separated by a %x2E (".") character.
    // FIXME: Implement the above conversions.

    return MUST(url.serialized_host().to_lowercase());
}

// https://datatracker.ietf.org/doc/html/draft-ietf-httpbis-rfc6265bis-22#section-5.1.3
bool domain_matches(StringView string, StringView domain_string)
{
    // A string domain-matches a given domain string if at least one of the following conditions hold:

    // * The domain string and the string are identical. (Note that both the domain string and the string will have been
    //   canonicalized to lower case at this point.)
    if (string == domain_string)
        return true;

    // * All of the following conditions hold:
    //   - The domain string is a suffix of the string.
    if (!string.ends_with(domain_string))
        return false;
    //   - The last character of the string that is not included in the domain string is a %x2E (".") character.
    if (string[string.length() - domain_string.length() - 1] != '.')
        return false;
    //   - The string is a host name (i.e., not an IP address).
    if (AK::IPv4Address::from_string(string).has_value())
        return false;
    if (AK::IPv6Address::from_string(string).has_value())
        return false;

    return true;
}

// https://datatracker.ietf.org/doc/html/draft-ietf-httpbis-rfc6265bis-22#section-5.1.4-3
bool path_matches(StringView request_path, StringView cookie_path)
{
    // A request-path path-matches a given cookie-path if at least one of the following conditions holds:

    // * The cookie-path and the request-path are identical.
    if (request_path == cookie_path)
        return true;

    if (request_path.starts_with(cookie_path)) {
        // * The cookie-path is a prefix of the request-path, and the last character of the cookie-path is %x2F ("/").
        if (cookie_path.ends_with('/'))
            return true;

        // * The cookie-path is a prefix of the request-path, and the first character of the request-path that is not
        //   included in the cookie-path is a %x2F ("/") character.
        if (request_path[cookie_path.length()] == '/')
            return true;
    }

    return false;
}

// https://datatracker.ietf.org/doc/html/draft-ietf-httpbis-rfc6265bis-22#section-5.1.4-1
String default_path(URL::URL const& url)
{
    // 1. Let uri-path be the path portion of the request-uri if such a portion exists (and empty otherwise).
    auto uri_path = URL::percent_decode(url.serialize_path());

    // 2. If the uri-path is empty or if the first character of the uri-path is not a %x2F ("/") character, output
    //    %x2F ("/") and skip the remaining steps.
    if (uri_path.is_empty() || (uri_path[0] != '/'))
        return "/"_string;

    StringView uri_path_view = uri_path;
    size_t last_separator = uri_path_view.find_last('/').value();

    // 3. If the uri-path contains no more than one %x2F ("/") character, output %x2F ("/") and skip the remaining step.
    if (last_separator == 0)
        return "/"_string;

    // 4. Output the characters of the uri-path from the first character up to, but not including, the right-most
    //    %x2F ("/").
    // FIXME: The path might not be valid UTF-8.
    return MUST(String::from_utf8(uri_path.substring_view(0, last_separator)));
}

}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Cookie::Cookie const& cookie)
{
    TRY(encoder.encode(cookie.name));
    TRY(encoder.encode(cookie.value));
    TRY(encoder.encode(cookie.domain));
    TRY(encoder.encode(cookie.path));
    TRY(encoder.encode(cookie.creation_time));
    TRY(encoder.encode(cookie.expiry_time));
    TRY(encoder.encode(cookie.host_only));
    TRY(encoder.encode(cookie.http_only));
    TRY(encoder.encode(cookie.last_access_time));
    TRY(encoder.encode(cookie.persistent));
    TRY(encoder.encode(cookie.secure));
    TRY(encoder.encode(cookie.same_site));

    return {};
}

template<>
ErrorOr<Web::Cookie::Cookie> IPC::decode(Decoder& decoder)
{
    auto name = TRY(decoder.decode<String>());
    auto value = TRY(decoder.decode<String>());
    auto domain = TRY(decoder.decode<String>());
    auto path = TRY(decoder.decode<String>());
    auto creation_time = TRY(decoder.decode<UnixDateTime>());
    auto expiry_time = TRY(decoder.decode<UnixDateTime>());
    auto host_only = TRY(decoder.decode<bool>());
    auto http_only = TRY(decoder.decode<bool>());
    auto last_access_time = TRY(decoder.decode<UnixDateTime>());
    auto persistent = TRY(decoder.decode<bool>());
    auto secure = TRY(decoder.decode<bool>());
    auto same_site = TRY(decoder.decode<Web::Cookie::SameSite>());

    return Web::Cookie::Cookie { move(name), move(value), same_site, creation_time, last_access_time, expiry_time, move(domain), move(path), secure, http_only, host_only, persistent };
}
