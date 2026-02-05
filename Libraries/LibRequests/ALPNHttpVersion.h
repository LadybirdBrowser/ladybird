/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Requests {

// https://www.iana.org/assignments/tls-extensiontype-values/tls-extensiontype-values.xhtml#alpn-protocol-ids
enum class ALPNHttpVersion {
    None,
    Http1_0,
    Http1_1,
    Http2_TLS,
    Http2_TCP,
    Http3,
};

inline FlyString alpn_http_version_to_fly_string(ALPNHttpVersion version)
{
    switch (version) {
    case ALPNHttpVersion::None:
        return ""_fly_string;
    case ALPNHttpVersion::Http1_0:
        return "http/1.0"_fly_string;
    case ALPNHttpVersion::Http1_1:
        return "http/1.1"_fly_string;
    case ALPNHttpVersion::Http2_TLS:
        return "h2"_fly_string;
    case ALPNHttpVersion::Http2_TCP:
        return "h2c"_fly_string;
    case ALPNHttpVersion::Http3:
        return "h3"_fly_string;
    default:
        VERIFY_NOT_REACHED();
    }
}

}
