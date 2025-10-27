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
//
// IMPORTANT LIMITATIONS:
// - This validator makes SYNCHRONOUS BLOCKING TCP connections
// - Calling from an event loop will BLOCK until connection succeeds/fails
// - Connection timeout is system-dependent (typically 30-120 seconds)
// - Can cause UI freezes if called from IPC handlers
//
// FUTURE IMPROVEMENTS:
// - Make validation asynchronous (requires event loop integration)
// - Add configurable timeout support
// - Move validation to background thread
// - Add caching to reduce validation frequency
//
// CURRENT WORKAROUND:
// - Validation failures are treated as warnings, not errors
// - Config is applied even if validation fails
// - This prevents falling back to unencrypted connections
class ProxyValidator {
public:
    // Test if proxy is reachable and accepting connections
    // WARNING: This is SYNCHRONOUS and will BLOCK the calling thread
    [[nodiscard]] static ErrorOr<void> test_proxy(ProxyConfig const& config);

    // Convenience wrapper - returns true if proxy is working
    // WARNING: This is SYNCHRONOUS and will BLOCK the calling thread
    [[nodiscard]] static bool is_proxy_working(ProxyConfig const& config);

private:
    static ErrorOr<void> test_http_proxy(ByteString const& host, u16 port);
    static ErrorOr<void> test_socks5_proxy(ByteString const& host, u16 port);
};

}
