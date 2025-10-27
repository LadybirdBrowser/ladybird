/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCore/Proxy.h>
#include <LibIPC/ProxyConfig.h>
#include <LibURL/URL.h>

namespace IPC {

// Audit entry for network identity activity tracking
struct NetworkAuditEntry {
    MonotonicTime timestamp;
    URL::URL url;
    ByteString method;
    Optional<u16> response_code;
    size_t bytes_sent { 0 };
    size_t bytes_received { 0 };

    NetworkAuditEntry(URL::URL url, ByteString method)
        : timestamp(MonotonicTime::now())
        , url(move(url))
        , method(move(method))
    {
    }
};

// Network identity for per-tab isolation
// Each tab can have a unique network identity with:
// - Cryptographic identity (public/private key pair)
// - Network routing configuration (Tor circuit, VPN, proxy)
// - Audit trail of all network activity
class NetworkIdentity : public RefCounted<NetworkIdentity> {
public:
    // Create network identity for a specific page/tab
    [[nodiscard]] static ErrorOr<NonnullRefPtr<NetworkIdentity>> create_for_page(u64 page_id);

    // Create network identity with Tor circuit
    [[nodiscard]] static ErrorOr<NonnullRefPtr<NetworkIdentity>> create_with_tor(u64 page_id, ByteString circuit_id = {});

    // Create network identity with custom proxy
    [[nodiscard]] static ErrorOr<NonnullRefPtr<NetworkIdentity>> create_with_proxy(u64 page_id, ProxyConfig);

    ~NetworkIdentity() = default;

    // Page/tab identification
    [[nodiscard]] u64 page_id() const { return m_page_id; }
    [[nodiscard]] ByteString const& identity_id() const { return m_identity_id; }

    // Cryptographic identity (for future encrypted P2P protocols)
    [[nodiscard]] Optional<ByteString> const& public_key() const { return m_public_key; }
    [[nodiscard]] Optional<ByteString> const& private_key() const { return m_private_key; }

    // Network routing configuration
    [[nodiscard]] Optional<ProxyConfig> const& proxy_config() const { return m_proxy_config; }
    [[nodiscard]] bool has_proxy() const { return m_proxy_config.has_value(); }
    [[nodiscard]] bool has_tor_circuit() const
    {
        return m_proxy_config.has_value()
            && (m_proxy_config->type == ProxyType::SOCKS5 || m_proxy_config->type == ProxyType::SOCKS5H)
            && m_proxy_config->host == "localhost"sv
            && m_proxy_config->port == 9050;
    }

    // Tor circuit management
    [[nodiscard]] Optional<ByteString> const& tor_circuit_id() const { return m_tor_circuit_id; }
    ErrorOr<void> rotate_tor_circuit(); // Request new Tor circuit (NEWNYM)

    // VPN/interface routing (for future multi-VPN support)
    [[nodiscard]] Optional<ByteString> const& vpn_interface() const { return m_vpn_interface; }
    void set_vpn_interface(ByteString interface) { m_vpn_interface = move(interface); }

    // Proxy configuration
    void set_proxy_config(ProxyConfig config)
    {
        m_proxy_config = move(config);
        if (m_proxy_config->username.has_value())
            m_tor_circuit_id = m_proxy_config->username;
    }
    void clear_proxy_config()
    {
        // SECURITY: Clear credentials from memory before resetting config
        if (m_proxy_config.has_value())
            m_proxy_config->clear_credentials();

        m_proxy_config = {};
        m_tor_circuit_id = {};
    }

    // Audit trail
    void log_request(URL::URL const&, ByteString method);
    void log_response(URL::URL const&, u16 response_code, size_t bytes_sent, size_t bytes_received);
    [[nodiscard]] Vector<NetworkAuditEntry> const& audit_log() const { return m_audit_log; }
    [[nodiscard]] size_t total_requests() const { return m_audit_log.size(); }

    // Statistics
    [[nodiscard]] size_t total_bytes_sent() const;
    [[nodiscard]] size_t total_bytes_received() const;
    [[nodiscard]] MonotonicTime created_at() const { return m_created_at; }
    [[nodiscard]] AK::Duration age() const { return MonotonicTime::now() - m_created_at; }

    // Security: Clear sensitive data on destruction
    void clear_sensitive_data()
    {
        if (m_private_key.has_value()) {
            // Zero out private key memory
            explicit_bzero(const_cast<char*>(m_private_key->characters()), m_private_key->length());
            m_private_key = {};
        }

        // SECURITY: Clear proxy credentials from memory
        if (m_proxy_config.has_value())
            m_proxy_config->clear_credentials();

        m_proxy_config = {};
        m_tor_circuit_id = {};
    }

private:
    explicit NetworkIdentity(u64 page_id);

    ErrorOr<void> generate_cryptographic_identity();
    ErrorOr<void> initialize_tor_circuit(ByteString circuit_id);

    u64 m_page_id;
    ByteString m_identity_id;  // Unique identifier (e.g., "page-123-abc456")
    MonotonicTime m_created_at;

    // Cryptographic identity (for future P2P protocols)
    Optional<ByteString> m_public_key;
    Optional<ByteString> m_private_key;

    // Network routing
    Optional<ProxyConfig> m_proxy_config;
    Optional<ByteString> m_tor_circuit_id;
    Optional<ByteString> m_vpn_interface;

    // Audit trail
    Vector<NetworkAuditEntry> m_audit_log;
    static constexpr size_t MaxAuditEntries = 1000;  // Limit audit log size
};

// Tor availability checker
// Checks if Tor is running and accessible before attempting to use it
class TorAvailability {
public:
    // Check if Tor SOCKS5 proxy is available
    [[nodiscard]] static ErrorOr<void> check_socks5_available(ByteString host = "127.0.0.1"sv, u16 port = 9050);

    // Convenience wrapper - returns true if Tor is running
    [[nodiscard]] static bool is_tor_running();
};

}
