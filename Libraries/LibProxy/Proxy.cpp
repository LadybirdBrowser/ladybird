/*
 * Copyright (c) 2026, kunlinglio <lkl13263311018@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Proxy.h"
#include "Platform/ProxyPlatform.h"
#include "ProxyManager.h"
#include <AK/String.h>
#include <AK/StringUtils.h>
#include <AK/StringView.h>
#include <LibCore/Environment.h>
#include <LibURL/Parser.h>

namespace Proxy {

static Atomic<ProxyMode> g_proxy_mode { ProxyMode::System };

void set_proxy_mode(ProxyMode const& new_mode)
{
    g_proxy_mode.store(new_mode);
}

ProxyMode proxy_mode()
{
    return g_proxy_mode.load();
}

bool use_system_proxy()
{
    return proxy_mode() == ProxyMode::System;
}

ErrorOr<ProxyData> ProxyData::from_url(URL::URL const& proxy_url)
{
    ProxyData proxy_data;
    if (proxy_url.scheme() == "direct") {
        proxy_data.type = ProxyData::Type::Direct;
        return proxy_data;
    }
    if (proxy_url.scheme() == "socks5") {
        proxy_data.type = ProxyData::Type::SOCKS5;
    } else if (proxy_url.scheme() == "http") {
        proxy_data.type = ProxyData::Type::HTTP;
    } else if (proxy_url.scheme() == "https") {
        proxy_data.type = ProxyData::Type::HTTPS;
    } else {
        return Error::from_string_literal("Unsupported proxy type");
    }

    auto host = proxy_url.host();
    if (!host.has_value())
        return Error::from_string_literal("Invalid proxy URL, missing host");
    proxy_data.host = host->serialize();
    auto port = proxy_url.port();
    if (!port.has_value())
        return Error::from_string_literal("Invalid proxy, must have a port");
    proxy_data.port = *port;

    return proxy_data;
}

Vector<ProxyData> get_proxies_for_url(URL::URL const& url)
{
    return ProxyManager::the().match_proxies_for_url(url);
}

}
