/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <LibWebCommon/WebDriver/Network.h>

namespace WebView {

// NB: The network state belongs to the browser side of a BiDi session, where context IDs can be resolved.
class WebDriverBiDiNetwork {
    AK_ALLOC_WITH_KMALLOC;

public:
    using ValidateContext = Function<ErrorOr<void, Web::WebDriver::Error>(String const&)>;
    ErrorOr<JsonValue, Web::WebDriver::Error> run_command(StringView method, JsonObject const&, ValidateContext const&);
    void context_destroyed(String const&);

private:
    // https://w3c.github.io/webdriver-bidi/#intercept-map
    struct Intercept {
        Vector<Web::WebDriver::Network::UrlPattern> url_patterns;
        HashTable<String> phases;
        Optional<Vector<String>> browsing_contexts;
    };
    HashMap<String, Intercept> m_intercept_map;

    // https://w3c.github.io/webdriver-bidi/#network-collectors
    struct Collector {
        u64 max_encoded_item_size;
        Vector<String> data_types;
        String collector_type;
        Vector<String> contexts;
        Vector<String> user_contexts;
    };
    HashMap<String, Collector> m_network_collectors;

    // https://w3c.github.io/webdriver-bidi/#session-extra-headers
    Vector<Web::WebDriver::Network::Header> m_default_headers;
    HashMap<String, Vector<Web::WebDriver::Network::Header>> m_user_context_headers;
    HashMap<String, Vector<Web::WebDriver::Network::Header>> m_navigable_headers;

    // https://w3c.github.io/webdriver-bidi/#default-cache-behavior
    String m_default_cache_behavior { "default"_string };
    // https://w3c.github.io/webdriver-bidi/#navigable-cache-behavior-map
    HashMap<String, String> m_navigable_cache_behavior_map;
};

}
