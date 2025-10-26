/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Socket.h>
#include <LibIPC/ProxyValidator.h>

namespace IPC {

ErrorOr<void> ProxyValidator::test_proxy(ProxyConfig const& config)
{
    if (!config.is_configured())
        return Error::from_string_literal("Proxy not configured");

    switch (config.type) {
    case ProxyType::HTTP:
    case ProxyType::HTTPS:
        return test_http_proxy(config.host, config.port);
    case ProxyType::SOCKS5:
    case ProxyType::SOCKS5H:
        return test_socks5_proxy(config.host, config.port);
    case ProxyType::None:
        return Error::from_string_literal("No proxy type specified");
    }

    VERIFY_NOT_REACHED();
}

ErrorOr<void> ProxyValidator::test_socks5_proxy(ByteString const& host, u16 port)
{
    // Connect to SOCKS5 proxy
    auto socket = TRY(Core::TCPSocket::connect(host, port));

    // Send SOCKS5 handshake: [version=5, nmethods=1, method=no_auth]
    u8 handshake[] = { 0x05, 0x01, 0x00 };
    TRY(socket->write_until_depleted({ handshake, sizeof(handshake) }));

    // Read response: [version=5, selected_method]
    u8 response[2];
    TRY(socket->read_until_filled({ response, sizeof(response) }));

    // Validate SOCKS5 version
    if (response[0] != 0x05)
        return Error::from_string_literal("Invalid SOCKS5 response version");

    // Check if no-auth method was accepted (0x00) or authentication required (0x02)
    if (response[1] != 0x00 && response[1] != 0x02)
        return Error::from_string_literal("SOCKS5 proxy rejected connection");

    return {};
}

ErrorOr<void> ProxyValidator::test_http_proxy(ByteString const& host, u16 port)
{
    // For HTTP/HTTPS proxies, just verify TCP connectivity
    // Full HTTP CONNECT validation would require more complex logic
    auto socket = TRY(Core::TCPSocket::connect(host, port));
    return {};
}

bool ProxyValidator::is_proxy_working(ProxyConfig const& config)
{
    auto result = test_proxy(config);
    return !result.is_error();
}

}
