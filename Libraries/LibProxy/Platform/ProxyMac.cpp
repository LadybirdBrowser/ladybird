/*
 * Copyright (c) 2026, kunlinglio <lkl13263311018@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../Proxy.h"
#include "ProxyPlatform.h"
#include <AK/ByteString.h>
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <dispatch/dispatch.h>

namespace Proxy {

static String cfstring_to_string(CFStringRef cf_string)
{
    if (!cf_string)
        return {};
    CFIndex length = CFStringGetLength(cf_string);
    CFIndex max_size = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    auto* buffer = static_cast<char*>(malloc(max_size));
    if (CFStringGetCString(cf_string, buffer, max_size, kCFStringEncodingUTF8)) {
        auto result = String::from_utf8(StringView { buffer, strlen(buffer) }).release_value_but_fixme_should_propagate_errors();
        free(buffer);
        return result;
    }
    free(buffer);
    return {};
}

ProxyTable get_proxy_table_from_mac()
{
    ProxyTable table = {};
    CFDictionaryRef proxies = SCDynamicStoreCopyProxies(nullptr);
    if (!proxies) {
        warnln("Failed to get system proxy configuration from macOS");
        return table;
    }

    // get ignore list
    CFArrayRef no_proxy_list = (CFArrayRef)CFDictionaryGetValue(proxies, kSCPropNetProxiesExceptionsList);
    if (no_proxy_list) {
        CFIndex count = CFArrayGetCount(no_proxy_list);
        for (CFIndex i = 0; i < count; i++) {
            CFStringRef entry = (CFStringRef)CFArrayGetValueAtIndex(no_proxy_list, i);
            auto trimmed_entry = cfstring_to_string(entry).trim_whitespace();
            if (!trimmed_entry.is_error() && !trimmed_entry.value().is_empty()) {
                table.no_proxy_list.append(move(trimmed_entry).release_value());
            }
        }
    }
    CFNumberRef exclude_simple_hostname_ref = (CFNumberRef)CFDictionaryGetValue(proxies, kSCPropNetProxiesExcludeSimpleHostnames);
    if (exclude_simple_hostname_ref) {
        i64 exclude_simple_hostname = 0;
        CFNumberGetValue(exclude_simple_hostname_ref, kCFNumberSInt64Type, &exclude_simple_hostname);
        if (exclude_simple_hostname) {
            table.no_proxy_list.append("127.0.0.1"_string);
        }
    }

    auto get_proxy = [&](CFStringRef enable_key, CFStringRef host_key, CFStringRef port_key, ProxyData::Type type) -> Optional<ProxyData> {
        CFNumberRef enabled = (CFNumberRef)CFDictionaryGetValue(proxies, enable_key);
        if (enabled) {
            int enabled_val = 0;
            CFNumberGetValue(enabled, kCFNumberIntType, &enabled_val);
            if (enabled_val) {
                CFStringRef host = (CFStringRef)CFDictionaryGetValue(proxies, host_key);
                CFNumberRef port = (CFNumberRef)CFDictionaryGetValue(proxies, port_key);
                if (host && port) {
                    int port_val = 0;
                    CFNumberGetValue(port, kCFNumberIntType, &port_val);
                    return ProxyData {
                        .type = type,
                        .host = cfstring_to_string(host),
                        .port = static_cast<u16>(port_val),
                    };
                }
            }
        }
        return {};
    };

    table.http_proxy = get_proxy(kSCPropNetProxiesHTTPEnable, kSCPropNetProxiesHTTPProxy, kSCPropNetProxiesHTTPPort, ProxyData::Type::HTTP);
    // Note: "ProxiesHTTPS" indicates that it's a proxy for HTTPS requests, but not the proxy type.
    // Since there's no explicit proxy server type in macOS system proxy configuration, we treat it as an HTTP proxy.
    table.https_proxy = get_proxy(kSCPropNetProxiesHTTPSEnable, kSCPropNetProxiesHTTPSProxy, kSCPropNetProxiesHTTPSPort, ProxyData::Type::HTTP);

    table.all_proxy = get_proxy(kSCPropNetProxiesSOCKSEnable, kSCPropNetProxiesSOCKSProxy, kSCPropNetProxiesSOCKSPort, ProxyData::Type::SOCKS5);

    CFRelease(proxies);
    return table;
}

void watch_system_proxy_changes_mac(Function<void()> callback)
{
    auto* cb = new Function<void()>(move(callback));

    SCDynamicStoreContext context = {
        .version = 0,
        .info = cb,
        .retain = nullptr,
        .release = [](void const* info) { delete static_cast<Function<void()> const*>(info); },
        .copyDescription = nullptr,
    };

    auto const* store = SCDynamicStoreCreate(nullptr, CFSTR("LadybirdProxy"), [](SCDynamicStoreRef, CFArrayRef, void* info) { (*static_cast<Function<void()>*>(info))(); }, &context);

    CFStringRef watched_keys[] = { CFSTR("State:/Network/Global/Proxies") };
    auto const* keys = CFArrayCreate(nullptr, (void const**)watched_keys, 1, &kCFTypeArrayCallBacks);
    SCDynamicStoreSetNotificationKeys(store, keys, nullptr);
    CFRelease(keys);

    static dispatch_queue_t queue = dispatch_queue_create("org.ladybird.system-proxy", DISPATCH_QUEUE_SERIAL);
    SCDynamicStoreSetDispatchQueue(store, queue);

    // store and cb are intentionally leaked — they must live for the lifetime of the process
}

}
