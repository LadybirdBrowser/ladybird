/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebSocket/ConnectionInfo.h>

namespace WebSocket {

ConnectionInfo::ConnectionInfo(URL::URL url)
    : m_url(move(url))
{
}

bool ConnectionInfo::is_secure() const
{
    // RFC 6455 Section 3 :
    // The URI is called "secure" if the scheme component matches "wss" case-insensitively.
    return m_url.scheme().bytes_as_string_view().equals_ignoring_ascii_case("wss"sv);
}

ByteString ConnectionInfo::resource_name() const
{
    // RFC 6455 Section 3 :
    // The "resource-name" can be constructed by concatenating the following:
    StringBuilder builder;
    // "/" if the path component is empty
    auto path = URL::percent_decode(m_url.serialize_path());
    if (path.is_empty())
        builder.append('/');
    // The path component
    builder.append(path);
    // "?" if the query component is non-empty
    if (m_url.query().has_value() && !m_url.query()->is_empty()) {
        builder.append('?');
        // the query component
        builder.append(*m_url.query());
    }
    return builder.to_byte_string();
}

}
