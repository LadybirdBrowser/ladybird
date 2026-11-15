/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibDNS/Resolver.h>
#include <LibWebSocket/ConnectionInfo.h>

namespace WebSocket {

ConnectionInfo::ConnectionInfo(URL::URL url)
    : m_url(move(url))
    , m_headers(HTTP::HeaderList::create())
{
}

ConnectionInfo::ConnectionInfo(ConnectionInfo const&) = default;
ConnectionInfo::ConnectionInfo(ConnectionInfo&&) = default;
ConnectionInfo& ConnectionInfo::operator=(ConnectionInfo const&) = default;
ConnectionInfo& ConnectionInfo::operator=(ConnectionInfo&&) = default;
ConnectionInfo::~ConnectionInfo() = default;

Optional<DNS::LookupResult const&> ConnectionInfo::dns_result() const
{
    if (!m_dns_result)
        return {};
    return *m_dns_result;
}

void ConnectionInfo::set_dns_result(NonnullRefPtr<DNS::LookupResult const> dns_result)
{
    m_dns_result = move(dns_result);
}

}
