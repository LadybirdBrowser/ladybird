/*
 * Copyright (c) 2026, kunlinglio <lkl13263311018@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Platform/ProxyPlatform.h"
#include "Proxy.h"
#include <AK/AtomicRefCounted.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibSync/Mutex.h>

namespace Proxy {

class SystemProxyTable : public AK::AtomicRefCounted<SystemProxyTable> {
public:
    Vector<ProxyTable> tables;
};

class ProxyManager {
public:
    static ProxyManager& the();
    Vector<ProxyData> match_proxies_for_url(URL::URL const& url);

private:
    static ProxyManager* s_the;

    Sync::Mutex m_proxy_table_lock;
    AK::RefPtr<SystemProxyTable> m_proxy_tables;
    AK::NonnullRefPtr<SystemProxyTable> get_proxy_tables();

    void invalidate_cache();

    ProxyManager();
};

}
