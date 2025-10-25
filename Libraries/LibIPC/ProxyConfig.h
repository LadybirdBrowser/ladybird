/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/Types.h>

namespace IPC {

// Proxy type for network requests
enum class ProxyType : u8 {
    None,           // Direct connection (no proxy)
    SOCKS5,         // SOCKS5 proxy (local DNS resolution)
    SOCKS5H,        // SOCKS5 with hostname resolution via proxy (DNS leak prevention)
    HTTP,           // HTTP proxy (CONNECT method)
    HTTPS,          // HTTPS proxy
};

// Proxy configuration for per-tab network identity
struct ProxyConfig {
    ProxyType type { ProxyType::None };
    ByteString host;
    u16 port { 0 };
    Optional<ByteString> username;  // For SOCKS5 stream isolation
    Optional<ByteString> password;  // For SOCKS5 authentication

    // Check if proxy is configured
    [[nodiscard]] bool is_configured() const
    {
        return type != ProxyType::None && !host.is_empty() && port > 0;
    }

    // Generate libcurl-compatible proxy URL
    [[nodiscard]] ByteString to_curl_proxy_url() const
    {
        if (!is_configured())
            return {};

        switch (type) {
        case ProxyType::SOCKS5:
            return ByteString::formatted("socks5://{}:{}", host, port);
        case ProxyType::SOCKS5H:
            return ByteString::formatted("socks5h://{}:{}", host, port);
        case ProxyType::HTTP:
            return ByteString::formatted("http://{}:{}", host, port);
        case ProxyType::HTTPS:
            return ByteString::formatted("https://{}:{}", host, port);
        case ProxyType::None:
            return {};
        }
        VERIFY_NOT_REACHED();
    }

    // Generate libcurl-compatible authentication string
    [[nodiscard]] Optional<ByteString> to_curl_auth_string() const
    {
        if (!username.has_value())
            return {};

        if (password.has_value())
            return ByteString::formatted("{}:{}", *username, *password);

        return ByteString::formatted("{}:", *username);
    }

    // Create Tor SOCKS5 proxy configuration
    [[nodiscard]] static ProxyConfig tor_proxy(ByteString circuit_id = {})
    {
        ProxyConfig config;
        config.type = ProxyType::SOCKS5H;  // DNS resolution via Tor (prevents leaks)
        config.host = "localhost"sv;
        config.port = 9050;  // Default Tor SOCKS5 port

        // Use circuit_id as SOCKS5 username for stream isolation
        // Each unique username gets a separate Tor circuit
        if (!circuit_id.is_empty())
            config.username = move(circuit_id);

        return config;
    }

    // Equality comparison
    [[nodiscard]] bool operator==(ProxyConfig const& other) const
    {
        return type == other.type
            && host == other.host
            && port == other.port
            && username == other.username
            && password == other.password;
    }
};

}
