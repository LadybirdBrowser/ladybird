/*
 * Copyright (c) 2021-2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <LibIPC/Forward.h>
#include <LibURL/Forward.h>
#include <LibWeb/Cookie/Cookie.h>

namespace Web::Cookie {

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
String default_path(URL::URL const&);

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::Cookie::ParsedCookie const&);

template<>
ErrorOr<Web::Cookie::ParsedCookie> decode(Decoder&);

}
