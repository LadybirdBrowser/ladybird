/*
 * Copyright (c) 2026, kunlinglio <lkl13263311018@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ProxyManager.h"

namespace Proxy {

ProxyManager* ProxyManager::s_the = nullptr;
Sync::OnceFlag ProxyManager::s_once {};

ProxyManager::ProxyManager()
{
    watch_system_proxy_changes([this] {
        invalidate_cache();
    });
}

ProxyManager& ProxyManager::the()
{
    Sync::call_once(s_once, [] {
        s_the = new ProxyManager();
    });
    return *s_the;
}

NonnullRefPtr<SystemProxyTable> ProxyManager::get_proxy_tables()
{
    Sync::MutexLocker lock(m_proxy_table_lock);
    if (m_proxy_tables) {
        return *m_proxy_tables;
    }
    auto tables = make_ref_counted<SystemProxyTable>();
    tables->tables = get_system_proxy_table();
    m_proxy_tables = *tables;
    return tables;
}

void ProxyManager::invalidate_cache()
{
    Sync::MutexLocker lock(m_proxy_table_lock);
    dbgln("System proxy configuration changed, invalidating proxy cache");
    m_proxy_tables = nullptr;
}

Vector<ProxyData> ProxyManager::match_proxies_for_url(URL::URL const& url)
{
    Vector<ProxyData> result;
    auto proxy_tables = get_proxy_tables();

    auto host = url.host();
    auto host_string = host.has_value() ? host->serialize() : String {};
    auto const& scheme = url.scheme();

    for (auto& table : proxy_tables->tables) {
        bool host_excluded = false;
        for (auto& entry : table.no_proxy_list) {
            if (entry.is_empty())
                continue;
            if (!host_string.is_empty() && AK::StringUtils::matches(host_string, entry, CaseSensitivity::CaseInsensitive)) {
                host_excluded = true;
                break;
            }
        }
        if (host_excluded)
            continue;

        if (table.http_proxy.has_value() && scheme == "http")
            result.append(table.http_proxy.value());
        else if (table.https_proxy.has_value() && scheme == "https")
            result.append(table.https_proxy.value());
        else if (table.all_proxy.has_value())
            result.append(table.all_proxy.value());
    }
    return result;
}

}
