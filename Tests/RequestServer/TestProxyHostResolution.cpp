/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibTest/TestCase.h>
#include <RequestServer/Resolver.h>

static HTTP::Proxy proxy_at(StringView host, u16 port)
{
    HTTP::Proxy proxy;
    proxy.type = HTTP::Proxy::Type::HTTP;
    proxy.host = host;
    proxy.port = port;
    return proxy;
}

TEST_CASE(no_proxy_needs_no_lookup_or_pin)
{
    Core::EventLoop loop;
    bool called_back = false;
    RequestServer::resolve_proxy_host({}, [&](ErrorOr<void> result) {
        EXPECT(!result.is_error());
        called_back = true;
    });
    EXPECT(called_back);
    EXPECT(!RequestServer::curl_resolve_entry_for_proxy({}).has_value());
}

TEST_CASE(proxy_given_as_an_address_needs_no_lookup_or_pin)
{
    Core::EventLoop loop;
    for (auto host : { "192.0.2.1"sv, "::1"sv, "fe80::1%eth0"sv, "fe80::1%25eth0"sv }) {
        auto proxy = proxy_at(host, 3128);
        bool called_back = false;
        RequestServer::resolve_proxy_host(proxy, [&](ErrorOr<void> result) {
            EXPECT(!result.is_error());
            called_back = true;
        });
        EXPECT(called_back);
        EXPECT(!RequestServer::curl_resolve_entry_for_proxy(proxy).has_value());
    }
}

TEST_CASE(proxy_given_by_name_is_pinned_to_the_addresses_found)
{
    Core::EventLoop loop;
    auto proxy = proxy_at("localhost"sv, 3128);
    EXPECT(!RequestServer::curl_resolve_entry_for_proxy(proxy).has_value());

    bool called_back = false;
    bool failed = false;
    RequestServer::resolve_proxy_host(proxy, [&](ErrorOr<void> result) {
        failed = result.is_error();
        called_back = true;
    });
    loop.spin_until([&] { return called_back; });
    EXPECT(!failed);

    auto entry = RequestServer::curl_resolve_entry_for_proxy(proxy);
    EXPECT(entry.has_value());
    EXPECT(entry->starts_with("localhost:3128:"sv));
    EXPECT(entry->contains("127.0.0.1"sv));

    // A pin is for the host and port the proxy was looked up with, and no other.
    EXPECT(!RequestServer::curl_resolve_entry_for_proxy(proxy_at("localhost"sv, 8080)).has_value());
}
