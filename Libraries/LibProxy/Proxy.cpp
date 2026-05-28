/*
 * Copyright (c) 2026, kunlinglio <lkl13263311018@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Proxy.h"
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

    proxy_data.host = proxy_url.host()->serialize();
    auto port = proxy_url.port();
    if (!port.has_value())
        return Error::from_string_literal("Invalid proxy, must have a port");
    proxy_data.port = *port;

    return proxy_data;
}

static Vector<ProxyData> get_proxies_from_environment(URL::URL const& url)
{
    auto get_env_var = [](StringView name) -> Optional<String> {
        auto value = Core::Environment::get(name);
        if (!value.has_value())
            return {};
        // copy to avoid thread safety issues
        return String::from_utf8(value.value()).release_value_but_fixme_should_propagate_errors();
    };
    auto from_url_sv = [](StringView url_sv) -> Optional<ProxyData> {
        auto url = URL::Parser::basic_parse(url_sv);
        if (!url.has_value())
            return {};
        auto proxy_data_or_error = ProxyData::from_url(url.value());
        if (proxy_data_or_error.is_error())
            return {};
        return proxy_data_or_error.release_value();
    };
    Vector<ProxyData> results;

    Optional<String> proxy_env;
    proxy_env = get_env_var("no_proxy"sv);
    if (!proxy_env.has_value()) {
        proxy_env = get_env_var("NO_PROXY"sv);
    }
    if (proxy_env.has_value()) {
        auto no_proxy_list_res = proxy_env.value().split(',');
        if (no_proxy_list_res.is_error()) {
            auto error = no_proxy_list_res.release_error();
            warnln("Failed to parse no_proxy environment variable '{}': {}", proxy_env.value(), error);
        } else {
            auto no_proxy_list = no_proxy_list_res.release_value();

            for (auto& entry : no_proxy_list) {
                auto trimmed_entry = entry.trim_whitespace().value_or(""_string);
                if (trimmed_entry.is_empty())
                    continue;
                if (url.host().has_value() && AK::StringUtils::matches(url.host()->serialize(), trimmed_entry, CaseSensitivity::CaseInsensitive)) {
                    return {};
                }
            }
        }
    }
    if (url.scheme() == "http") {
        proxy_env = get_env_var("http_proxy"sv);
        if (!proxy_env.has_value())
            proxy_env = get_env_var("HTTP_PROXY"sv);
    } else if (url.scheme() == "https") {
        proxy_env = get_env_var("https_proxy"sv);
        if (!proxy_env.has_value())
            proxy_env = get_env_var("HTTPS_PROXY"sv);
    }
    if (proxy_env.has_value()) {
        auto parsed_proxy = from_url_sv(proxy_env.value());
        if (parsed_proxy.has_value())
            results.append(parsed_proxy.value());
    }
    proxy_env = get_env_var("all_proxy"sv);
    if (!proxy_env.has_value())
        proxy_env = get_env_var("ALL_PROXY"sv);
    if (proxy_env.has_value()) {
        auto parsed_proxy = from_url_sv(proxy_env.value());
        if (parsed_proxy.has_value())
            results.append(parsed_proxy.value());
    }

    return results;
}

Vector<ProxyData> get_proxies_for_url(URL::URL const& url)
{
    auto results = get_proxies_from_environment(url);
    if (!results.is_empty())
        return results;

#if defined(AK_OS_MACOS)
    results = get_proxies_from_mac_system_configuration(url);
    if (!results.is_empty())
        return results;
#endif

    results.append({});
    return results;
}

}
