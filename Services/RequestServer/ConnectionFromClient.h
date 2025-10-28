/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/SourceLocation.h>
#include <LibDNS/Resolver.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/Limits.h>
#include <LibIPC/NetworkIdentity.h>
#include <LibIPC/RateLimiter.h>
#include <LibWebSocket/WebSocket.h>
#include <RequestServer/RequestClientEndpoint.h>
#include <RequestServer/RequestServerEndpoint.h>

namespace RequestServer {

struct Resolver : public RefCounted<Resolver>
    , Weakable<Resolver> {
    Resolver(Function<ErrorOr<DNS::Resolver::SocketResult>()> create_socket)
        : dns(move(create_socket))
    {
    }

    DNS::Resolver dns;
};

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<RequestClientEndpoint, RequestServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

public:
    ~ConnectionFromClient() override;

    virtual void die() override;

    // Network identity management (per-page for circuit isolation)
    [[nodiscard]] RefPtr<IPC::NetworkIdentity> network_identity_for_page(u64 page_id);
    [[nodiscard]] RefPtr<IPC::NetworkIdentity> get_or_create_network_identity_for_page(u64 page_id);

private:
    explicit ConnectionFromClient(NonnullOwnPtr<IPC::Transport>);

    virtual Messages::RequestServer::InitTransportResponse init_transport(int peer_pid) override;
    virtual Messages::RequestServer::ConnectNewClientResponse connect_new_client() override;
    virtual Messages::RequestServer::ConnectNewClientsResponse connect_new_clients(size_t count) override;

    virtual Messages::RequestServer::IsSupportedProtocolResponse is_supported_protocol(ByteString) override;
    virtual void set_dns_server(ByteString host_or_address, u16 port, bool use_tls, bool validate_dnssec_locally) override;
    virtual void set_use_system_dns() override;
    virtual void start_request(i32 request_id, ByteString, URL::URL, HTTP::HeaderMap, ByteBuffer, Core::ProxyData, u64 page_id) override;
    virtual Messages::RequestServer::StopRequestResponse stop_request(i32) override;
    virtual Messages::RequestServer::SetCertificateResponse set_certificate(i32, ByteString, ByteString) override;
    virtual void ensure_connection(URL::URL url, ::RequestServer::CacheLevel cache_level) override;

    virtual void clear_cache() override;

    // Tor network control IPC handlers (with page_id for per-tab isolation)
    virtual void enable_tor(u64 page_id, ByteString circuit_id) override;
    virtual void disable_tor(u64 page_id) override;
    virtual void rotate_tor_circuit(u64 page_id) override;

    // VPN/Proxy control IPC handlers (with page_id for per-tab isolation)
    virtual void set_proxy(u64 page_id, ByteString host, u16 port, ByteString proxy_type, Optional<ByteString> username, Optional<ByteString> password) override;
    virtual void clear_proxy(u64 page_id) override;

    // Network audit log retrieval
    virtual Messages::RequestServer::GetNetworkAuditResponse get_network_audit() override;

    // IPFS content pinning
    virtual Messages::RequestServer::IpfsPinAddResponse ipfs_pin_add(ByteString cid) override;
    virtual Messages::RequestServer::IpfsPinRemoveResponse ipfs_pin_remove(ByteString cid) override;
    virtual Messages::RequestServer::IpfsPinListResponse ipfs_pin_list() override;

    virtual void websocket_connect(i64 websocket_id, URL::URL, ByteString, Vector<ByteString>, Vector<ByteString>, HTTP::HeaderMap) override;
    virtual void websocket_send(i64 websocket_id, bool, ByteBuffer) override;
    virtual void websocket_close(i64 websocket_id, u16, ByteString) override;
    virtual Messages::RequestServer::WebsocketSetCertificateResponse websocket_set_certificate(i64, ByteString, ByteString) override;

    struct ResumeRequestForFailedCacheEntry {
        size_t start_offset { 0 };
        int writer_fd { 0 };
    };
    void issue_network_request(i32 request_id, ByteString, URL::URL, HTTP::HeaderMap, ByteBuffer, Core::ProxyData, u64 page_id, Optional<ResumeRequestForFailedCacheEntry> = {});
    void issue_network_request_with_optional_dns(i32 request_id, ByteString, URL::URL, HTTP::HeaderMap, ByteBuffer, Core::ProxyData, u64 page_id, Optional<ResumeRequestForFailedCacheEntry>, Optional<NonnullRefPtr<DNS::LookupResult>>);
    void issue_ipfs_request(i32 request_id, ByteString method, URL::URL ipfs_url, HTTP::HeaderMap, ByteBuffer, Core::ProxyData, u64 page_id);
    void issue_ipns_request(i32 request_id, ByteString method, URL::URL ipns_url, HTTP::HeaderMap, ByteBuffer, Core::ProxyData, u64 page_id);
    void issue_ens_request(i32 request_id, ByteString method, URL::URL ens_url, HTTP::HeaderMap, ByteBuffer, Core::ProxyData, u64 page_id);

    HashMap<i32, RefPtr<WebSocket::WebSocket>> m_websockets;

    struct ActiveRequest;
    friend struct ActiveRequest;

    static ErrorOr<IPC::File> create_client_socket();

    static int on_socket_callback(void*, int sockfd, int what, void* user_data, void*);
    static int on_timeout_callback(void*, long timeout_ms, void* user_data);
    static size_t on_header_received(void* buffer, size_t size, size_t nmemb, void* user_data);
    static size_t on_data_received(void* buffer, size_t size, size_t nmemb, void* user_data);

    HashMap<i32, NonnullOwnPtr<ActiveRequest>> m_active_requests;

    // IPFS content verification: Store CIDs for pending requests
    HashMap<i32, IPC::ParsedCID> m_pending_ipfs_verifications;

    void check_active_requests();
    void* m_curl_multi { nullptr };
    RefPtr<Core::Timer> m_timer;
    HashMap<int, NonnullRefPtr<Core::Notifier>> m_read_notifiers;
    HashMap<int, NonnullRefPtr<Core::Notifier>> m_write_notifiers;
    NonnullRefPtr<Resolver> m_resolver;
    ByteString m_alt_svc_cache_path;

    // Network identity per page_id for per-tab routing and audit
    // SECURITY: Each page/tab maintains independent proxy/Tor configuration
    // NOTE: Static to share state across all ConnectionFromClient instances
    // Multiple WebContent processes create separate ConnectionFromClient instances,
    // so we need global state to ensure enable_tor and start_request see the same data
    static HashMap<u64, RefPtr<IPC::NetworkIdentity>> s_page_network_identities;

    // Security validation helpers
    [[nodiscard]] bool validate_request_id(i32 request_id, SourceLocation location = SourceLocation::current())
    {
        if (!m_active_requests.contains(request_id)) {
            dbgln("Security: RequestServer attempted access to invalid request_id {} at {}:{}",
                request_id, location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool validate_websocket_id(i64 websocket_id, SourceLocation location = SourceLocation::current())
    {
        if (!m_websockets.contains(websocket_id)) {
            dbgln("Security: RequestServer attempted access to invalid websocket_id {} at {}:{}",
                websocket_id, location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool validate_url(URL::URL const& url, SourceLocation location = SourceLocation::current())
    {
        // Length validation
        auto url_string = url.to_string();
        if (url_string.bytes_as_string_view().length() > IPC::Limits::MaxURLLength) {
            dbgln("Security: RequestServer sent oversized URL ({} bytes, max {}) at {}:{}",
                url_string.bytes_as_string_view().length(),
                IPC::Limits::MaxURLLength, location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }

        // Scheme validation (http/https/ipfs/ipns allowed)
        if (!url.scheme().is_one_of("http"sv, "https"sv, "ipfs"sv, "ipns"sv)) {
            dbgln("Security: RequestServer attempted disallowed URL scheme '{}' at {}:{}",
                url.scheme(), location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }

        return true;
    }

    [[nodiscard]] bool validate_string_length(StringView string, StringView field_name, SourceLocation location = SourceLocation::current())
    {
        if (string.length() > IPC::Limits::MaxStringLength) {
            dbgln("Security: RequestServer sent oversized {} ({} bytes, max {}) at {}:{}",
                field_name, string.length(), IPC::Limits::MaxStringLength,
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool validate_buffer_size(size_t size, StringView field_name, SourceLocation location = SourceLocation::current())
    {
        // 100MB maximum for request bodies and WebSocket data
        static constexpr size_t MaxRequestBodySize = 100 * 1024 * 1024;
        if (size > MaxRequestBodySize) {
            dbgln("Security: RequestServer sent oversized {} ({} bytes, max {}) at {}:{}",
                field_name, size, MaxRequestBodySize,
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    template<typename T>
    [[nodiscard]] bool validate_vector_size(Vector<T> const& vector, StringView field_name, SourceLocation location = SourceLocation::current())
    {
        if (vector.size() > IPC::Limits::MaxVectorSize) {
            dbgln("Security: RequestServer sent oversized {} ({} elements, max {}) at {}:{}",
                field_name, vector.size(), IPC::Limits::MaxVectorSize,
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool validate_header_map(HTTP::HeaderMap const& headers, SourceLocation location = SourceLocation::current())
    {
        if (headers.headers().size() > IPC::Limits::MaxVectorSize) {
            dbgln("Security: RequestServer sent too many headers ({}, max {}) at {}:{}",
                headers.headers().size(), IPC::Limits::MaxVectorSize,
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }

        // Validate each header name and value
        for (auto const& header : headers.headers()) {
            if (!validate_string_length(header.name, "header name"sv, location))
                return false;
            if (!validate_string_length(header.value, "header value"sv, location))
                return false;

            // Check for CRLF injection
            if (header.name.contains('\r') || header.name.contains('\n') ||
                header.value.contains('\r') || header.value.contains('\n')) {
                dbgln("Security: RequestServer attempted CRLF injection in header at {}:{}",
                    location.filename(), location.line_number());
                track_validation_failure();
                return false;
            }
        }

        return true;
    }

    [[nodiscard]] bool validate_count(size_t count, size_t max_count, StringView field_name, SourceLocation location = SourceLocation::current())
    {
        if (count > max_count) {
            dbgln("Security: RequestServer sent excessive {} ({}, max {}) at {}:{}",
                field_name, count, max_count,
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool check_rate_limit(SourceLocation location = SourceLocation::current())
    {
        if (!m_rate_limiter.try_consume()) {
            dbgln("Security: RequestServer exceeded rate limit at {}:{}",
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    void track_validation_failure()
    {
        m_validation_failures++;
        if (m_validation_failures >= s_max_validation_failures) {
            dbgln("Security: RequestServer exceeded validation failure limit ({}), terminating connection",
                s_max_validation_failures);
            die();
        }
    }

    // Security infrastructure
    IPC::RateLimiter m_rate_limiter { 1000, AK::Duration::from_milliseconds(10) }; // 1000 messages/second
    size_t m_validation_failures { 0 };
    static constexpr size_t s_max_validation_failures = 100;
};

// FIXME: Find a good home for this
ByteString build_curl_resolve_list(DNS::LookupResult const&, StringView host, u16 port);
constexpr inline uintptr_t websocket_private_tag = 0x1;

}
