/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2026, kunlinglio <lkl13263311018@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>

namespace Proxy {

enum class ProxyMode : u8 {
    System,
    Direct,
};

ALWAYS_INLINE String print_proxy_mode(ProxyMode mode)
{
    switch (mode) {
    case ProxyMode::System:
        return "system"_string;
    case ProxyMode::Direct:
        return "direct"_string;
    }
    VERIFY_NOT_REACHED();
}

void set_proxy_mode(ProxyMode const&);
ProxyMode proxy_mode();
bool use_system_proxy();

struct ProxyData {
    enum class Type : u8 {
        Direct,
        SOCKS5,
        HTTP,
        HTTPS
    } type { Type::Direct };
    String host;
    u16 port { 0 };

    bool is_direct() const { return type == Type::Direct; }

    bool operator==(ProxyData const&) const = default;

    static ErrorOr<ProxyData> from_url(URL::URL const&);
};

Vector<ProxyData> get_proxies_for_url(URL::URL const&);

#if defined(AK_OS_MACOS)
Vector<ProxyData> get_proxies_from_mac_system_configuration(URL::URL const&);
#endif

// FIXME: Add more platform-specific proxy retrieval functions here (e.g. Android, BSD, Windows).

}
