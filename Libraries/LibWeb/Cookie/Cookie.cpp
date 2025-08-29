/*
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Cookie.h"
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

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

// https://www.ietf.org/archive/id/draft-ietf-httpbis-rfc6265bis-15.html#section-5.1.2
Optional<String> canonicalize_domain(URL::URL const& url)
{
    if (!url.host().has_value())
        return {};

    // 1. Convert the host name to a sequence of individual domain name labels.
    // 2. Convert each label that is not a Non-Reserved LDH (NR-LDH) label, to an A-label (see Section 2.3.2.1 of
    //    [RFC5890] for the former and latter), or to a "punycode label" (a label resulting from the "ToASCII" conversion
    //    in Section 4 of [RFC3490]), as appropriate (see Section 6.3 of this specification).
    // 3. Concatenate the resulting labels, separated by a %x2E (".") character.
    // FIXME: Implement the above conversions.

    return MUST(url.serialized_host().to_lowercase());
}

// https://www.ietf.org/archive/id/draft-ietf-httpbis-rfc6265bis-15.html#section-5.1.4
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

    return Web::Cookie::Cookie { move(name), move(value), same_site, move(creation_time), move(last_access_time), move(expiry_time), move(domain), move(path), secure, http_only, host_only, persistent };
}
