/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>

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

inline ByteString alpn_http_version_to_byte_string(ALPNHttpVersion version)
{
    switch (version) {
    case ALPNHttpVersion::None:
        return ""sv;
    case ALPNHttpVersion::Http1_0:
        return "http/1.0"sv;
    case ALPNHttpVersion::Http1_1:
        return "http/1.1"sv;
    case ALPNHttpVersion::Http2_TLS:
        return "h2"sv;
    case ALPNHttpVersion::Http2_TCP:
        return "h2c"sv;
    case ALPNHttpVersion::Http3:
        return "h3"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

}
