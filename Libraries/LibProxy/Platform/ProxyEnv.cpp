/*
 * Copyright (c) 2026, kunlinglio <lkl13263311018@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../Proxy.h"
#include "ProxyPlatform.h"
#include <AK/ByteString.h>
#include <CoreFoundation/CoreFoundation.h>
#include <LibCore/Environment.h>
#include <LibURL/Parser.h>
#include <SystemConfiguration/SystemConfiguration.h>

namespace Proxy {

ProxyTable get_proxy_table_from_environment()
{
    auto get_env_var = [](StringView name) -> Optional<String> {
        auto value = Core::Environment::get(name);
        if (!value.has_value())
            return {};
        // copy to avoid thread safety issues
        return String::from_utf8(value.value()).release_value_but_fixme_should_propagate_errors();
    };
    auto proxy_from_url_sv = [](StringView url_sv) -> Optional<ProxyData> {
        auto url = URL::Parser::basic_parse(url_sv);
        if (!url.has_value())
            return {};
        auto proxy_data_or_error = ProxyData::from_url(url.value());
        if (proxy_data_or_error.is_error())
            return {};
        return proxy_data_or_error.release_value();
    };

    ProxyTable result = {};

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
                if (!trimmed_entry.is_empty())
                    result.no_proxy_list.append(move(trimmed_entry));
            }
        }
    }
    proxy_env = get_env_var("http_proxy"sv);
    if (!proxy_env.has_value())
        proxy_env = get_env_var("HTTP_PROXY"sv);
    if (proxy_env.has_value()) {
        auto parsed_proxy = proxy_from_url_sv(proxy_env.value());
        if (parsed_proxy.has_value())
            result.http_proxy = parsed_proxy.value();
    }
    proxy_env = get_env_var("https_proxy"sv);
    if (!proxy_env.has_value())
        proxy_env = get_env_var("HTTPS_PROXY"sv);
    if (proxy_env.has_value()) {
        auto parsed_proxy = proxy_from_url_sv(proxy_env.value());
        if (parsed_proxy.has_value())
            result.https_proxy = parsed_proxy.value();
    }
    proxy_env = get_env_var("all_proxy"sv);
    if (!proxy_env.has_value())
        proxy_env = get_env_var("ALL_PROXY"sv);
    if (proxy_env.has_value()) {
        auto parsed_proxy = proxy_from_url_sv(proxy_env.value());
        if (parsed_proxy.has_value())
            result.all_proxy = parsed_proxy.value();
    }

    return result;
}

}
