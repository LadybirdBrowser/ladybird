/*
 * Copyright (c) 2021-2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Time.h>
#include <LibIPC/Forward.h>

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

StringView same_site_to_string(SameSite same_site_mode);
SameSite same_site_from_string(StringView same_site_mode);

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::Cookie::Cookie const&);

template<>
ErrorOr<Web::Cookie::Cookie> decode(Decoder&);

}
