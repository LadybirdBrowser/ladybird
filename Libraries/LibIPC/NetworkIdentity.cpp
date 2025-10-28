/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Hex.h>
#include <AK/Random.h>
#include <LibCore/Socket.h>
#include <LibCrypto/Curves/EdwardsCurve.h>
#include <LibIPC/NetworkIdentity.h>

namespace IPC {

NetworkIdentity::NetworkIdentity(u64 page_id)
    : m_page_id(page_id)
    , m_created_at(MonotonicTime::now())
{
    // Generate unique identity ID: "page-{page_id}-{random_hex}"
    u32 random_value = get_random<u32>();
    m_identity_id = ByteString::formatted("page-{}-{}", page_id, encode_hex({ &random_value, sizeof(random_value) }));
}

ErrorOr<NonnullRefPtr<NetworkIdentity>> NetworkIdentity::create_for_page(u64 page_id)
{
    auto identity = adopt_ref(*new NetworkIdentity(page_id));

    // Generate Ed25519 keypair for P2P protocol support
    TRY(identity->generate_cryptographic_identity());

    return identity;
}

ErrorOr<NonnullRefPtr<NetworkIdentity>> NetworkIdentity::create_with_tor(u64 page_id, ByteString circuit_id)
{
    auto identity = TRY(create_for_page(page_id));
    TRY(identity->initialize_tor_circuit(move(circuit_id)));
    return identity;
}

ErrorOr<NonnullRefPtr<NetworkIdentity>> NetworkIdentity::create_with_proxy(u64 page_id, ProxyConfig proxy_config)
{
    auto identity = TRY(create_for_page(page_id));
    identity->set_proxy_config(move(proxy_config));
    return identity;
}

ErrorOr<void> NetworkIdentity::generate_cryptographic_identity()
{
    using namespace Crypto::Curves;

    // Generate Ed25519 keypair for P2P protocol support (IPFS, Hypercore, etc.)
    Ed25519 ed25519;

    // Generate private key (32 bytes)
    auto private_key_buffer = TRY(ed25519.generate_private_key());

    // Derive public key from private key (32 bytes)
    auto public_key_buffer = TRY(ed25519.generate_public_key(private_key_buffer));

    // Store keys as ByteStrings
    m_public_key = ByteString::copy(public_key_buffer);
    m_private_key = ByteString::copy(private_key_buffer);

    dbgln("NetworkIdentity: Generated Ed25519 keypair for page_id {}", m_page_id);
    dbgln("  Public key fingerprint: {}", encode_hex(m_public_key->bytes().slice(0, 16)));

    return {};
}

ErrorOr<void> NetworkIdentity::initialize_tor_circuit(ByteString circuit_id)
{
    // If no circuit ID provided, use the identity ID for stream isolation
    if (circuit_id.is_empty())
        circuit_id = m_identity_id;

    m_tor_circuit_id = circuit_id;
    m_proxy_config = ProxyConfig::tor_proxy(circuit_id);

    return {};
}

ErrorOr<void> NetworkIdentity::rotate_tor_circuit()
{
    if (!has_tor_circuit())
        return Error::from_string_literal("No Tor circuit configured");

    // Generate new circuit ID
    u32 random_value = get_random<u32>();
    ByteString new_circuit_id = ByteString::formatted("circuit-{}-{}", m_page_id, encode_hex({ &random_value, sizeof(random_value) }));

    return initialize_tor_circuit(move(new_circuit_id));
}

void NetworkIdentity::log_request(URL::URL const& url, ByteString method)
{
    // Prevent audit log from growing unbounded
    if (m_audit_log.size() >= MaxAuditEntries) {
        // Remove oldest entry
        m_audit_log.remove(0);
    }

    m_audit_log.empend(url, move(method));
}

void NetworkIdentity::log_response(URL::URL const& url, u16 response_code, size_t bytes_sent, size_t bytes_received)
{
    // Find the most recent request matching this URL
    for (auto it = m_audit_log.rbegin(); it != m_audit_log.rend(); ++it) {
        if (it->url == url && !it->response_code.has_value()) {
            it->response_code = response_code;
            it->bytes_sent = bytes_sent;
            it->bytes_received = bytes_received;
            return;
        }
    }

    // If no matching request found, create a new entry
    NetworkAuditEntry entry(url, "UNKNOWN"sv);
    entry.response_code = response_code;
    entry.bytes_sent = bytes_sent;
    entry.bytes_received = bytes_received;

    if (m_audit_log.size() >= MaxAuditEntries)
        m_audit_log.remove(0);

    m_audit_log.append(move(entry));
}

size_t NetworkIdentity::total_bytes_sent() const
{
    size_t total = 0;
    for (auto const& entry : m_audit_log)
        total += entry.bytes_sent;
    return total;
}

size_t NetworkIdentity::total_bytes_received() const
{
    size_t total = 0;
    for (auto const& entry : m_audit_log)
        total += entry.bytes_received;
    return total;
}

// TorAvailability implementation

ErrorOr<void> TorAvailability::check_socks5_available(ByteString host, u16 port)
{
    // Try to connect to Tor SOCKS5 proxy using LibCore::TCPSocket
    // This will attempt to connect and return an error if Tor is not running
    auto socket_result = Core::TCPSocket::connect(host, port);

    if (socket_result.is_error()) {
        // Connection failed - Tor is not available
        dbgln("TorAvailability: Cannot connect to Tor SOCKS5 proxy at {}:{} - {}",
            host, port, socket_result.error());
        return Error::from_string_literal("Cannot connect to Tor SOCKS5 proxy. Is Tor running?");
    }

    // Successfully connected - Tor is available
    dbgln("TorAvailability: Tor SOCKS5 proxy is available at {}:{}", host, port);
    return {};
}

bool TorAvailability::is_tor_running()
{
    auto result = check_socks5_available();
    return !result.is_error();
}

}
