/*
 * Copyright (c) 2026, kunlinglio <lkl13263311018@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <AK/IPv4Address.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCore/Proxy.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>

#ifdef HAS_LIBPROXY
#    include <proxy.h>
#endif

namespace Core {

ErrorOr<ProxyData> ProxyData::parse_url(URL::URL const& url)
{
    ProxyData proxy_data;
    if (url.scheme() == "direct") {
        proxy_data.type = ProxyData::Type::Direct;
        return proxy_data;
    }
    if (url.scheme() == "socks5") {
        proxy_data.type = ProxyData::Type::SOCKS5;
    } else if (url.scheme() == "http") {
        proxy_data.type = ProxyData::Type::HTTP;
    } else if (url.scheme() == "https") {
        proxy_data.type = ProxyData::Type::HTTPS;
    } else {
        return Error::from_string_literal("Unsupported proxy type");
    }

    proxy_data.host = url.host()->serialize();

    auto port = url.port();
    if (!port.has_value())
        return Error::from_string_literal("Invalid proxy, must have a port");
    proxy_data.port = *port;

    return proxy_data;
}

// FIXME: Support config this via settings page.
bool ProxyData::use_system_proxy()
{
#ifdef HAS_LIBPROXY
    return true;
#else
    return false;
#endif
}

Vector<ProxyData> ProxyData::get_proxies_for_url(URL::URL const& url)
{
    Vector<ProxyData> results;
#ifdef HAS_LIBPROXY
    auto* factory = px_proxy_factory_new(); // FIXME: Use a shared factory.
    if (auto* proxies = px_proxy_factory_get_proxies(factory, url.to_byte_string().characters())) {
        for (size_t i = 0; proxies[i]; ++i) {
            auto string_view = StringView(proxies[i], strlen(proxies[i]));
            auto proxy_url = URL::Parser::basic_parse(string_view);
            if (proxy_url.has_value()) {
                auto proxy_data_or_error = parse_url(proxy_url.value());
                if (proxy_data_or_error.is_error()) {
                    dbgln("Failed to parse proxy URL {}: {}, skipping", proxies[i], proxy_data_or_error.error());
                    continue;
                }
                results.append(proxy_data_or_error.release_value());
            }
        }
        px_proxy_factory_free_proxies(proxies);
    }
    px_proxy_factory_free(factory);
#endif
    if (results.is_empty())
        results.append({ .type = Type::Direct, .host = {}, .port = 0 });
    return results;
}

}
