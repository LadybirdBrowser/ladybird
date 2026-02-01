/*
 * Copyright (c) 2021-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Time.h>
#include <LibCore/SharedVersion.h>
#include <LibIPC/Forward.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>

namespace Web::Cookie {

enum class SameSite {
    Default,
    None,
    Strict,
    Lax
};

enum class Source {
    NonHttp,
    Http,
};

struct WEB_API Cookie {
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

WEB_API StringView same_site_to_string(SameSite same_site_mode);
WEB_API SameSite same_site_from_string(StringView same_site_mode);

WEB_API Optional<String> canonicalize_domain(URL::URL const& url);
WEB_API bool domain_matches(StringView string, StringView domain_string);
WEB_API bool path_matches(StringView request_path, StringView cookie_path);
WEB_API String default_path(URL::URL const&);

WEB_API bool cookie_matches_url(Cookie const&, URL::URL const&, String const& retrieval_host_canonical, Optional<Source> = {});

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Cookie::Cookie const&);

template<>
WEB_API ErrorOr<Web::Cookie::Cookie> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Cookie::VersionedCookie const&);

template<>
WEB_API ErrorOr<Web::Cookie::VersionedCookie> decode(Decoder&);

}
