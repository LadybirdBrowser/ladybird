/*
 * Copyright (c) 2026, kunlinglio <lkl13263311018@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "../Proxy.h"
#include <AK/Function.h>
#include <AK/Optional.h>

namespace Proxy {

struct ProxyTable {
    AK::Optional<ProxyData> http_proxy;  // Proxy for HTTP requests.
    AK::Optional<ProxyData> https_proxy; // Proxy for HTTPS requests.
    AK::Optional<ProxyData> all_proxy;   // Proxy for remaining requests.
    Vector<String> no_proxy_list;        // ignore patterns
};

Vector<ProxyTable> get_system_proxy_table();
void watch_system_proxy_changes(Function<void()> callback);

// Platform-specific implementations
ProxyTable get_proxy_table_from_environment();
// Assume that environment variables do not change during the lifetime of the process,
// so we don't watch for changes in environment variables
#if defined(AK_OS_MACOS)
ProxyTable get_proxy_table_from_mac();
void watch_system_proxy_changes_mac(Function<void()> callback);
#elif defined(AK_OS_WINDOWS)
// FIXME: Implement this function for Windows.
#elif defined(AK_OS_LINUX)
// FIXME: Implement this function for Linux.
#elif defined(AK_ANDROID)
// FIXME: Implement this function for Android.
#else
// FIXME: Support more platforms.
#endif

// FIXME: Add support to Proxy auto-configuration (PAC) js script here.

}
