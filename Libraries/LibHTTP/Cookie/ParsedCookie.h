/*
 * Copyright (c) 2021-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <LibHTTP/Cookie/Cookie.h>
#include <LibHTTP/Forward.h>
#include <LibIPC/Forward.h>
#include <LibURL/Forward.h>

namespace HTTP::Cookie {

struct ParsedCookie {
    String name;
    String value;
    SameSite same_site_attribute { SameSite::Default };
    Optional<UnixDateTime> expiry_time_from_expires_attribute {};
    Optional<UnixDateTime> expiry_time_from_max_age_attribute {};
    Optional<String> domain {};
    Optional<String> path {};
    bool secure_attribute_present { false };
    bool http_only_attribute_present { false };
};

Optional<ParsedCookie> parse_cookie(URL::URL const&, StringView cookie_string);
bool cookie_contains_invalid_control_character(StringView);

constexpr inline auto MAXIMUM_COOKIE_AGE = AK::Duration::from_seconds(400LL * 24 * 60 * 60);

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, HTTP::Cookie::ParsedCookie const&);

template<>
ErrorOr<HTTP::Cookie::ParsedCookie> decode(Decoder&);

}
