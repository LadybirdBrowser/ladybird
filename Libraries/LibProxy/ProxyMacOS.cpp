/*
 * Copyright (c) 2026, kunlinglio <lkl13263311018@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Proxy.h"
#include <AK/ByteString.h>
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>

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

Vector<ProxyData> get_proxies_from_mac_system_configuration(URL::URL const& url)
{
    Vector<ProxyData> results;
    CFDictionaryRef proxies = SCDynamicStoreCopyProxies(nullptr);
    if (!proxies)
        return results;

    auto check_proxy = [&](CFStringRef enable_key, CFStringRef host_key, CFStringRef port_key, ProxyData::Type type) {
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
                    results.append({
                        .type = type,
                        .host = cfstring_to_string(host),
                        .port = static_cast<u16>(port_val),
                    });
                }
            }
        }
    };

    if (url.scheme() == "http") {
        check_proxy(kSCPropNetProxiesHTTPEnable, kSCPropNetProxiesHTTPProxy, kSCPropNetProxiesHTTPPort, ProxyData::Type::HTTP);
    } else if (url.scheme() == "https") {
        check_proxy(kSCPropNetProxiesHTTPSEnable, kSCPropNetProxiesHTTPSProxy, kSCPropNetProxiesHTTPSPort, ProxyData::Type::HTTP);
    }

    check_proxy(kSCPropNetProxiesSOCKSEnable, kSCPropNetProxiesSOCKSProxy, kSCPropNetProxiesSOCKSPort, ProxyData::Type::SOCKS5);

    CFRelease(proxies);
    return results;
}

}
