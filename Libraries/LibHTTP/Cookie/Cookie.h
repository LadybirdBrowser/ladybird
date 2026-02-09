/*
 * Copyright (c) 2021-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <LibCore/SharedVersion.h>
#include <LibHTTP/Forward.h>
#include <LibIPC/Forward.h>
#include <LibURL/Forward.h>

namespace HTTP::Cookie {

enum class SameSite : u8 {
    Default,
    None,
    Strict,
    Lax,
};

enum class Source : u8 {
    NonHttp,
    Http,
};

struct Cookie {
    String creation_time_to_string() const;
    String last_access_time_to_string() const;
    String expiry_time_to_string() const;

    String name;
    String value;
    SameSite same_site { SameSite::Default };
    UnixDateTime creation_time {};
    UnixDateTime last_access_time {};
    UnixDateTime expiry_time {};
    String domain {};
    String path {};
    bool secure { false };
    bool http_only { false };
    bool host_only { false };
    bool persistent { false };
};

struct VersionedCookie {
    Optional<Core::SharedVersion> cookie_version;
    String cookie;
};

StringView same_site_to_string(SameSite same_site_mode);
SameSite same_site_from_string(StringView same_site_mode);

Optional<String> canonicalize_domain(URL::URL const& url);
bool domain_matches(StringView string, StringView domain_string);
bool path_matches(StringView request_path, StringView cookie_path);
String default_path(URL::URL const&);

bool cookie_matches_url(Cookie const&, URL::URL const&, String const& retrieval_host_canonical, Optional<Source> = {});

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, HTTP::Cookie::Cookie const&);

template<>
ErrorOr<HTTP::Cookie::Cookie> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, HTTP::Cookie::VersionedCookie const&);

template<>
ErrorOr<HTTP::Cookie::VersionedCookie> decode(Decoder&);

}
