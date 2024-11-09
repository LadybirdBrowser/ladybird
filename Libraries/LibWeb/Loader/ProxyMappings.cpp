/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ProxyMappings.h"

Web::ProxyMappings& Web::ProxyMappings::the()
{
    static ProxyMappings instance {};
    return instance;
}

Core::ProxyData Web::ProxyMappings::proxy_for_url(URL::URL const& url) const
{
    auto url_string = url.to_byte_string();
    for (auto& it : m_mappings) {
        if (url_string.matches(it.key)) {
            auto result = Core::ProxyData::parse_url(m_proxies[it.value]);
            if (result.is_error()) {
                dbgln("Failed to parse proxy URL: {}", m_proxies[it.value]);
                continue;
            }
            return result.release_value();
        }
    }

    return {};
}

void Web::ProxyMappings::set_mappings(Vector<ByteString> proxies, OrderedHashMap<ByteString, size_t> mappings)
{
    m_proxies = move(proxies);
    m_mappings = move(mappings);

    dbgln("Proxy mappings updated: proxies: {}", m_proxies);
}
