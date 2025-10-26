/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <LibIPC/ProxyConfig.h>

namespace IPC {

// Proxy connectivity validator
// Tests if a proxy is reachable and accepting connections before applying configuration
class ProxyValidator {
public:
    // Test if proxy is reachable and accepting connections
    [[nodiscard]] static ErrorOr<void> test_proxy(ProxyConfig const& config);

    // Convenience wrapper - returns true if proxy is working
    [[nodiscard]] static bool is_proxy_working(ProxyConfig const& config);

private:
    static ErrorOr<void> test_http_proxy(ByteString const& host, u16 port);
    static ErrorOr<void> test_socks5_proxy(ByteString const& host, u16 port);
};

}
