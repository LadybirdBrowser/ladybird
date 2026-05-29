/*
 * Copyright (c) 2026, kunlinglio <lkl13263311018@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ProxyPlatform.h"
#include "../Proxy.h"
#include <AK/Optional.h>
#include <AK/Vector.h>

namespace Proxy {

Vector<ProxyTable> get_system_proxy_table()
{
    Vector<ProxyTable> results = {};
    results.append(get_proxy_table_from_environment());
#if defined(AK_OS_MACOS)
    results.append(get_proxy_table_from_mac());
#endif
    return results;
}

void watch_system_proxy_changes(Function<void()> callback)
{
#if defined(AK_OS_MACOS)
    watch_system_proxy_changes_mac(move(callback));
#else
    // Avoid "unused parameter" error.
    (void)callback;
#endif
}

}
