/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IDAllocator.h>
#include <AK/NonnullOwnPtr.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Proxy.h>
#include <LibCore/Socket.h>
#include <LibCore/StandardPaths.h>
#include <LibIPC/IPFSAPIClient.h>
#include <LibIPC/IPFSVerifier.h>
#include <LibIPC/Limits.h>
#include <LibIPC/NetworkIdentity.h>
#include <LibIPC/ProxyValidator.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibRequests/WebSocket.h>
#include <LibWebSocket/ConnectionInfo.h>
#include <LibWebSocket/Message.h>
#include <RequestServer/CURL.h>
#include <RequestServer/Cache/DiskCache.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/Request.h>
#include <RequestServer/Resolver.h>
#include <RequestServer/WebSocketImplCurl.h>

namespace RequestServer {

static HashMap<int, RefPtr<ConnectionFromClient>> s_connections;
static IDAllocator s_client_ids;

// Gateway timeout configuration (shorter than standard HTTP for faster failover)
// With 4 IPFS gateways, total max time = 4 * (10s connect + 30s transfer) = 160s
[[maybe_unused]] static long s_gateway_connect_timeout_seconds = 10L;  // Connect timeout for gateway requests
[[maybe_unused]] static long s_gateway_request_timeout_seconds = 30L;  // Total request timeout for gateway requests

Optional<DiskCache> g_disk_cache;

// Static storage for NetworkIdentity shared across all ConnectionFromClient instances
HashMap<u64, RefPtr<IPC::NetworkIdentity>> ConnectionFromClient::s_page_network_identities;
ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<RequestClientEndpoint, RequestServerEndpoint>(*this, move(transport), s_client_ids.allocate())
    , m_resolver(Resolver::default_resolver())
{
    s_connections.set(client_id(), *this);

    m_alt_svc_cache_path = ByteString::formatted("{}/Ladybird/alt-svc-cache.txt", Core::StandardPaths::user_data_directory());

    m_curl_multi = curl_multi_init();

    auto set_option = [this](auto option, auto value) {
        auto result = curl_multi_setopt(m_curl_multi, option, value);
        VERIFY(result == CURLM_OK);
    };
    set_option(CURLMOPT_SOCKETFUNCTION, &on_socket_callback);
    set_option(CURLMOPT_SOCKETDATA, this);
    set_option(CURLMOPT_TIMERFUNCTION, &on_timeout_callback);
    set_option(CURLMOPT_TIMERDATA, this);

    m_timer = Core::Timer::create_single_shot(0, [this] {
        auto result = curl_multi_socket_action(m_curl_multi, CURL_SOCKET_TIMEOUT, 0, nullptr);
        VERIFY(result == CURLM_OK);
        check_active_requests();
    });
}

ConnectionFromClient::~ConnectionFromClient()
{
    m_active_requests.clear();

    curl_multi_cleanup(m_curl_multi);
    m_curl_multi = nullptr;
}

void ConnectionFromClient::request_complete(Badge<Request>, int request_id)
{
    Core::deferred_invoke([weak_self = make_weak_ptr<ConnectionFromClient>(), request_id] {
        if (auto self = weak_self.strong_ref())
            self->m_active_requests.remove(request_id);
    });
}

void ConnectionFromClient::die()
{
    auto client_id = this->client_id();
    s_connections.remove(client_id);
    s_client_ids.deallocate(client_id);

    if (s_connections.is_empty())
        Core::EventLoop::current().quit(0);
}

// Helper method to get network identity for a specific page
RefPtr<IPC::NetworkIdentity> ConnectionFromClient::network_identity_for_page(u64 page_id)
{
    return s_page_network_identities.get(page_id).value_or(nullptr);
}

// Helper method to get or create network identity for a page
RefPtr<IPC::NetworkIdentity> ConnectionFromClient::get_or_create_network_identity_for_page(u64 page_id)
{
    if (auto identity = network_identity_for_page(page_id))
        return identity;

    // Create new network identity for this page
    auto identity = MUST(IPC::NetworkIdentity::create_for_page(page_id));
    s_page_network_identities.set(page_id, identity);
    dbgln("RequestServer: Created NetworkIdentity for page {}", page_id);
    return identity;
}

void ConnectionFromClient::enable_tor(u64 page_id, ByteString circuit_id)
{
    dbgln("RequestServer: enable_tor() called on page {} with circuit_id='{}'", page_id, circuit_id);

    // SECURITY: Validate circuit_id length to prevent DoS attacks
    if (circuit_id.length() > IPC::Limits::MaxCircuitIDLength) {
        dbgln("RequestServer: SECURITY: Circuit ID too long ({} bytes, max {})",
            circuit_id.length(), IPC::Limits::MaxCircuitIDLength);
        return;
    }

    // SECURITY: Validate circuit_id contains only safe characters (alphanumeric, dash, underscore)
    if (!circuit_id.is_empty()) {
        for (char c : circuit_id) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_')) {
                dbgln("RequestServer: SECURITY: Circuit ID contains invalid character: {}", c);
                return;
            }
        }
    }

    // Get or create network identity for this page
    auto network_identity = get_or_create_network_identity_for_page(page_id);

    // Generate circuit ID if not provided
    if (circuit_id.is_empty())
        circuit_id = network_identity->identity_id();

    // Configure Tor proxy on this connection ONLY
    auto tor_proxy = IPC::ProxyConfig::tor_proxy(circuit_id);

    // SECURITY: Apply proxy configuration without upfront validation
    // Rationale:
    // - Upfront validation requires synchronous TCP connection which blocks event loop (30-120s)
    // - User explicitly requested Tor, so fail-secure is appropriate
    // - If Tor is down, requests will fail when attempting to connect (correct behavior)
    // - Better to fail requests than silently fall back to unencrypted direct connection
    // - ProxyValidator is kept for potential async validation in future
    network_identity->set_proxy_config(tor_proxy);

    dbgln("RequestServer: Tor proxy configured at {}:{} for page {}", tor_proxy.host, tor_proxy.port, page_id);

    dbgln("RequestServer: Tor ENABLED for page {} ONLY with circuit {}",
        page_id,
        network_identity->tor_circuit_id().value_or("default"));

    // SECURITY FIX: Removed global state mutation that applied Tor to ALL connections.
    // Each connection must manage its own proxy configuration independently to prevent
    // cross-tab privacy violations and circuit correlation attacks.
    // See SECURITY_AUDIT_REPORT.md - Critical Vulnerability #1
}

void ConnectionFromClient::disable_tor(u64 page_id)
{
    auto network_identity = network_identity_for_page(page_id);
    if (!network_identity)
        return;

    network_identity->clear_proxy_config();
    dbgln("RequestServer: Tor disabled for page {} ONLY", page_id);

    // SECURITY FIX: Removed global state mutation that disabled Tor on ALL connections.
    // Each connection manages its own state independently.
    // See SECURITY_AUDIT_REPORT.md - Critical Vulnerability #1
}

void ConnectionFromClient::rotate_tor_circuit(u64 page_id)
{
    auto network_identity = network_identity_for_page(page_id);
    if (!network_identity) {
        dbgln("RequestServer: Cannot rotate circuit - no network identity for page {}", page_id);
        return;
    }

    if (!network_identity->has_tor_circuit()) {
        dbgln("RequestServer: Cannot rotate circuit - Tor not enabled for page {}", page_id);
        return;
    }

    auto result = network_identity->rotate_tor_circuit();
    if (result.is_error()) {
        dbgln("RequestServer: Failed to rotate Tor circuit for page {}: {}", page_id, result.error());
        return;
    }

    auto new_circuit_id = network_identity->tor_circuit_id().value_or("unknown");
    dbgln("RequestServer: Tor circuit rotated for page {} ONLY to {}", page_id, new_circuit_id);

    // SECURITY FIX: Removed global state mutation that rotated circuits for ALL connections.
    // Each connection manages its own circuit independently.
    // See SECURITY_AUDIT_REPORT.md - Critical Vulnerability #1
}

void ConnectionFromClient::set_proxy(u64 page_id, ByteString host, u16 port, ByteString proxy_type, Optional<ByteString> username, Optional<ByteString> password)
{
    dbgln("RequestServer: set_proxy() called on page {} ({}:{})", page_id, host, port);

    // SECURITY: Validate port range (must be 1-65535)
    if (port < IPC::Limits::MinPortNumber || port > IPC::Limits::MaxPortNumber) {
        dbgln("RequestServer: SECURITY: Invalid proxy port {} (must be {}-{})",
            port, IPC::Limits::MinPortNumber, IPC::Limits::MaxPortNumber);
        return;
    }

    // SECURITY: Validate hostname length (RFC 1035 limit)
    if (host.length() > IPC::Limits::MaxHostnameLength) {
        dbgln("RequestServer: SECURITY: Proxy hostname too long ({} bytes, max {})",
            host.length(), IPC::Limits::MaxHostnameLength);
        return;
    }

    // SECURITY: Validate hostname contains only valid characters (no control chars)
    // Allowed: alphanumeric, dots, dashes, colons (for IPv6), and brackets (for IPv6)
    for (size_t i = 0; i < host.length(); ++i) {
        char c = host[i];
        bool is_valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                        c == ':' || c == '[' || c == ']';
        if (!is_valid) {
            dbgln("RequestServer: SECURITY: Invalid character in hostname at position {}: 0x{:02x}", i, static_cast<u8>(c));
            return;
        }
    }

    // SECURITY: Validate hostname is not empty
    if (host.is_empty()) {
        dbgln("RequestServer: SECURITY: Proxy hostname cannot be empty");
        return;
    }

    // SECURITY: Validate username length if provided
    if (username.has_value() && username->length() > IPC::Limits::MaxUsernameLength) {
        dbgln("RequestServer: SECURITY: Proxy username too long ({} bytes, max {})",
            username->length(), IPC::Limits::MaxUsernameLength);
        return;
    }

    // SECURITY: Validate password length if provided
    if (password.has_value() && password->length() > IPC::Limits::MaxPasswordLength) {
        dbgln("RequestServer: SECURITY: Proxy password too long ({} bytes, max {})",
            password->length(), IPC::Limits::MaxPasswordLength);
        return;
    }

    // SECURITY: Validate proxy type is one of the allowed values
    IPC::ProxyType validated_type;
    if (proxy_type == "SOCKS5H"sv)
        validated_type = IPC::ProxyType::SOCKS5H;
    else if (proxy_type == "SOCKS5"sv)
        validated_type = IPC::ProxyType::SOCKS5;
    else if (proxy_type == "HTTP"sv)
        validated_type = IPC::ProxyType::HTTP;
    else if (proxy_type == "HTTPS"sv)
        validated_type = IPC::ProxyType::HTTPS;
    else {
        dbgln("RequestServer: SECURITY: Invalid proxy type '{}' (must be SOCKS5, SOCKS5H, HTTP, or HTTPS)", proxy_type);
        return;
    }

    // Get or create network identity for this page
    auto network_identity = get_or_create_network_identity_for_page(page_id);

    // Build ProxyConfig from validated parameters
    IPC::ProxyConfig config;
    config.host = move(host);
    config.port = port;
    config.type = validated_type;
    config.username = move(username);
    config.password = move(password);

    // SECURITY: Apply proxy configuration without upfront validation
    // Rationale:
    // - Upfront validation requires synchronous TCP connection which blocks event loop (30-120s)
    // - User explicitly requested proxy, so fail-secure is appropriate
    // - If proxy is down, requests will fail when attempting to connect (correct behavior)
    // - Better to fail requests than silently fall back to unencrypted direct connection
    // - ProxyValidator is kept for potential async validation in future
    network_identity->set_proxy_config(config);

    dbgln("RequestServer: Proxy configured at {}:{} (type {}) for page {}",
        config.host, config.port, static_cast<int>(config.type), page_id);

    dbgln("RequestServer: Proxy ENABLED for page {} ONLY ({}:{})", page_id, config.host, config.port);

    // SECURITY FIX: Removed global state mutation that applied proxy to ALL connections.
    // Each connection must manage its own proxy configuration independently to prevent
    // cross-tab privacy violations and network interference.
    // See SECURITY_AUDIT_REPORT.md - Critical Vulnerability #1
}

void ConnectionFromClient::clear_proxy(u64 page_id)
{
    auto network_identity = network_identity_for_page(page_id);
    if (!network_identity) {
        dbgln("RequestServer: Cannot clear proxy - no network identity for page {}", page_id);
        return;
    }

    network_identity->clear_proxy_config();
    dbgln("RequestServer: Proxy disabled for page {} ONLY", page_id);

    // SECURITY FIX: Removed global state mutation that disabled proxy on ALL connections.
    // Each connection manages its own state independently.
    // See SECURITY_AUDIT_REPORT.md - Critical Vulnerability #1
}

Messages::RequestServer::GetNetworkAuditResponse ConnectionFromClient::get_network_audit()
{
    Vector<ByteString> audit_entries;
    size_t total_bytes_sent = 0;
    size_t total_bytes_received = 0;

    // SECURITY NOTE: This retrieves audit data for the DEFAULT network identity only.
    // In a per-tab implementation, this would need a page_id parameter.
    // For now, we get the first available network identity from the map.
    RefPtr<IPC::NetworkIdentity> network_identity;
    if (!s_page_network_identities.is_empty()) {
        network_identity = s_page_network_identities.begin()->value;
    }

    if (!network_identity) {
        dbgln("RequestServer: Cannot get audit - no network identity");
        return { move(audit_entries), total_bytes_sent, total_bytes_received };
    }

    // Serialize audit log entries as pipe-delimited strings for easy parsing
    for (auto const& entry : network_identity->audit_log()) {
        auto timestamp_ms = entry.timestamp.milliseconds();
        auto response_code = entry.response_code.has_value() ? ByteString::number(*entry.response_code) : ByteString("0");

        auto serialized = ByteString::formatted("{}|{}|{}|{}|{}|{}",
            timestamp_ms,
            entry.method,
            entry.url.to_byte_string(),
            response_code,
            entry.bytes_sent,
            entry.bytes_received);

        audit_entries.append(move(serialized));
    }

    total_bytes_sent = network_identity->total_bytes_sent();
    total_bytes_received = network_identity->total_bytes_received();

    dbgln("RequestServer: Returning {} audit entries, {} bytes sent, {} bytes received",
        audit_entries.size(), total_bytes_sent, total_bytes_received);

    return { move(audit_entries), total_bytes_sent, total_bytes_received };
}

Messages::RequestServer::IpfsPinAddResponse ConnectionFromClient::ipfs_pin_add(ByteString cid)
{
    // Security: Rate limiting
    if (!check_rate_limit())
        return false;

    // Security: CID string validation
    if (!validate_string_length(cid, "cid"sv))
        return false;

    dbgln("RequestServer: Pinning IPFS content: {}", cid);

    auto result = IPC::IPFSAPIClient::pin_add(cid);
    if (result.is_error()) {
        dbgln("RequestServer: Failed to pin CID {}: {}", cid, result.error());
        return false;
    }

    return true;
}

Messages::RequestServer::IpfsPinRemoveResponse ConnectionFromClient::ipfs_pin_remove(ByteString cid)
{
    // Security: Rate limiting
    if (!check_rate_limit())
        return false;

    // Security: CID string validation
    if (!validate_string_length(cid, "cid"sv))
        return false;

    dbgln("RequestServer: Unpinning IPFS content: {}", cid);

    auto result = IPC::IPFSAPIClient::pin_remove(cid);
    if (result.is_error()) {
        dbgln("RequestServer: Failed to unpin CID {}: {}", cid, result.error());
        return false;
    }

    return true;
}

Messages::RequestServer::IpfsPinListResponse ConnectionFromClient::ipfs_pin_list()
{
    // Security: Rate limiting
    if (!check_rate_limit())
        return Vector<ByteString> {};

    dbgln("RequestServer: Listing pinned IPFS content");

    auto result = IPC::IPFSAPIClient::pin_list();
    if (result.is_error()) {
        dbgln("RequestServer: Failed to list pins: {}", result.error());
        return Vector<ByteString> {};
    }

    return result.release_value();
}

Messages::RequestServer::InitTransportResponse ConnectionFromClient::init_transport([[maybe_unused]] int peer_pid)
{
#ifdef AK_OS_WINDOWS
    m_transport->set_peer_pid(peer_pid);
    return Core::System::getpid();
#endif
    VERIFY_NOT_REACHED();
}

Messages::RequestServer::ConnectNewClientResponse ConnectionFromClient::connect_new_client()
{
    // Security: Rate limiting
    if (!check_rate_limit())
        return IPC::File {};

    auto client_socket = create_client_socket();
    if (client_socket.is_error()) {
        dbgln("Failed to create client socket: {}", client_socket.error());
        return IPC::File {};
    }

    return client_socket.release_value();
}

Messages::RequestServer::ConnectNewClientsResponse ConnectionFromClient::connect_new_clients(size_t count)
{
    // Security: Rate limiting
    if (!check_rate_limit())
        return Vector<IPC::File> {};

    // Security: Count validation (DoS prevention)
    if (!validate_count(count, 100, "client count"sv))
        return Vector<IPC::File> {};

    Vector<IPC::File> files;
    files.ensure_capacity(count);

    for (size_t i = 0; i < count; ++i) {
        auto client_socket = create_client_socket();
        if (client_socket.is_error()) {
            dbgln("Failed to create client socket: {}", client_socket.error());
            return Vector<IPC::File> {};
        }

        files.unchecked_append(client_socket.release_value());
    }

    return files;
}

ErrorOr<IPC::File> ConnectionFromClient::create_client_socket()
{
    // TODO: Mach IPC

    int socket_fds[2] {};
    TRY(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, socket_fds));

    auto client_socket = Core::LocalSocket::adopt_fd(socket_fds[0]);
    if (client_socket.is_error()) {
        close(socket_fds[0]);
        close(socket_fds[1]);
        return client_socket.release_error();
    }

    // Note: A ref is stored in the static s_connections map
    auto client = adopt_ref(*new ConnectionFromClient(make<IPC::Transport>(client_socket.release_value())));

    return IPC::File::adopt_fd(socket_fds[1]);
}

Messages::RequestServer::IsSupportedProtocolResponse ConnectionFromClient::is_supported_protocol(ByteString protocol)
{
    // Security: Rate limiting
    if (!check_rate_limit())
        return false;

    // Security: Protocol string validation
    if (!validate_string_length(protocol, "protocol"sv))
        return false;

    return protocol == "http"sv || protocol == "https"sv;
}

void ConnectionFromClient::set_dns_server(ByteString host_or_address, u16 port, bool use_tls, bool validate_dnssec_locally)
{
    // Security: Rate limiting
    if (!check_rate_limit())
        return;

    // Security: Host/address string validation
    if (!validate_string_length(host_or_address, "host_or_address"sv))
        return;

    auto& dns_info = DNSInfo::the();

    if (host_or_address == dns_info.server_hostname && port == dns_info.port && use_tls == dns_info.use_dns_over_tls && validate_dnssec_locally == dns_info.validate_dnssec_locally)
        return;

    auto result = [&] -> ErrorOr<void> {
        Core::SocketAddress addr;
        if (auto v4 = IPv4Address::from_string(host_or_address); v4.has_value())
            addr = { v4.value(), port };
        else if (auto v6 = IPv6Address::from_string(host_or_address); v6.has_value())
            addr = { v6.value(), port };
        else
            TRY(m_resolver->dns.lookup(host_or_address)->await())->cached_addresses().first().visit([&](auto& address) { addr = { address, port }; });

        dns_info.server_address = addr;
        dns_info.server_hostname = host_or_address;
        dns_info.port = port;
        dns_info.use_dns_over_tls = use_tls;
        dns_info.validate_dnssec_locally = validate_dnssec_locally;
        return {};
    }();

    if (result.is_error())
        dbgln("Failed to set DNS server: {}", result.error());
    else
        m_resolver->dns.reset_connection();
}

void ConnectionFromClient::set_use_system_dns()
{
    // Security: Rate limiting
    if (!check_rate_limit())
        return;

    auto& dns_info = DNSInfo::the();
    dns_info.server_hostname = {};
    dns_info.server_address = {};
    m_resolver->dns.reset_connection();
}

#ifdef AK_OS_WINDOWS
void ConnectionFromClient::start_request(i32, ByteString, URL::URL, HTTP::HeaderMap, ByteBuffer, Core::ProxyData, u64)
{
    VERIFY(0 && "RequestServer::ConnectionFromClient::start_request is not implemented");
}

void ConnectionFromClient::issue_network_request(i32, ByteString, URL::URL, HTTP::HeaderMap, ByteBuffer, Core::ProxyData, u64, Optional<ResumeRequestForFailedCacheEntry>)
{
    VERIFY(0 && "RequestServer::ConnectionFromClient::issue_network_request is not implemented");
}
#else
void ConnectionFromClient::start_request(i32 request_id, ByteString method, URL::URL url, HTTP::HeaderMap request_headers, ByteBuffer request_body, Core::ProxyData proxy_data, u64 page_id)
{
    // Security: Rate limiting
    if (!check_rate_limit())
        return;

    // Security: URL validation (SSRF prevention)
    if (!validate_url(url))
        return;

    // Security: Method string validation
    if (!validate_string_length(method, "method"sv))
        return;

    // Security: Header validation (CRLF injection prevention)
    if (!validate_header_map(request_headers))
        return;

    // Security: Request body size validation (buffer exhaustion prevention)
    if (!validate_buffer_size(request_body.size(), "request_body"sv))
        return;

    dbgln_if(REQUESTSERVER_DEBUG, "RequestServer: start_request({}, {}, page_id={})", request_id, url, page_id);

    // IPFS Integration: Detect P2P protocol types
    Request::ProtocolType protocol_type = Request::ProtocolType::HTTP;
    ByteString original_resource_id;
    ByteString remaining_path;

    if (url.scheme() == "ipfs"sv) {
        protocol_type = Request::ProtocolType::IPFS;
        auto path = url.serialize_path().to_byte_string();
        if (path.starts_with("/"sv)) path = path.substring(1);
        if (auto slash = path.find('/'); slash.has_value()) {
            original_resource_id = path.substring(0, *slash);
            remaining_path = path.substring(*slash);
        } else {
            original_resource_id = path;
        }
        url = transform_ipfs_url_to_gateway(request_id, url, page_id);
    } else if (url.scheme() == "ipns"sv) {
        protocol_type = Request::ProtocolType::IPNS;
        auto path = url.serialize_path().to_byte_string();
        if (path.starts_with("/"sv)) path = path.substring(1);
        if (auto slash = path.find('/'); slash.has_value()) {
            original_resource_id = path.substring(0, *slash);
            remaining_path = path.substring(*slash);
        } else {
            original_resource_id = path;
        }
        url = transform_ipns_url_to_gateway(request_id, url, page_id);
    } else if (url.serialized_host().ends_with_bytes(".eth"sv)) {
        protocol_type = Request::ProtocolType::ENS;
        original_resource_id = url.serialized_host().to_byte_string();
        remaining_path = url.serialize_path().to_byte_string();
        url = transform_ens_url_to_gateway(request_id, url, page_id);
    }

    // Get network identity for this page (for Tor/proxy configuration)
    auto network_identity = network_identity_for_page(page_id);

    // Create the Request object using the (potentially transformed) URL
    auto request = Request::fetch(request_id, g_disk_cache, *this, m_curl_multi, m_resolver, move(url), method, request_headers, request_body, m_alt_svc_cache_path, proxy_data, network_identity);

    // Set protocol type on the request
    request->set_protocol_type(protocol_type);

    // Setup IPFS verification callback if this is an IPFS request
    if (protocol_type == Request::ProtocolType::IPFS) {
        setup_ipfs_verification(request_id, *request, original_resource_id);
    }

    if (protocol_type != Request::ProtocolType::HTTP) {
        setup_gateway_fallback(request_id, *request, protocol_type, original_resource_id,
                              remaining_path, method, request_headers, request_body, proxy_data, page_id);
    }

    m_active_requests.set(request_id, move(request));
}

void ConnectionFromClient::issue_network_request(i32 request_id, ByteString method, URL::URL url, HTTP::HeaderMap request_headers, ByteBuffer request_body, Core::ProxyData proxy_data, u64 page_id, Optional<ResumeRequestForFailedCacheEntry> resume)
{
    (void)resume;  // TODO: Handle resume for failed cache entries

    // Get network identity for this page (for Tor/proxy configuration)
    auto network_identity = network_identity_for_page(page_id);

    // Create the Request object using the provided URL (no IPFS detection)
    auto request = Request::fetch(request_id, g_disk_cache, *this, m_curl_multi, m_resolver, move(url), method, request_headers, request_body, m_alt_svc_cache_path, proxy_data, network_identity);

    m_active_requests.set(request_id, move(request));
}
#endif

int ConnectionFromClient::on_socket_callback(CURL*, int sockfd, int what, void* user_data, void*)
{
    auto* client = static_cast<ConnectionFromClient*>(user_data);

    if (what == CURL_POLL_REMOVE) {
        client->m_read_notifiers.remove(sockfd);
        client->m_write_notifiers.remove(sockfd);
        return 0;
    }

    if (what & CURL_POLL_IN) {
        client->m_read_notifiers.ensure(sockfd, [client, sockfd, multi = client->m_curl_multi] {
            auto notifier = Core::Notifier::construct(sockfd, Core::NotificationType::Read);
            notifier->on_activation = [client, sockfd, multi] {
                auto result = curl_multi_socket_action(multi, sockfd, CURL_CSELECT_IN, nullptr);
                VERIFY(result == CURLM_OK);

                client->check_active_requests();
            };

            notifier->set_enabled(true);
            return notifier;
        });
    }

    if (what & CURL_POLL_OUT) {
        client->m_write_notifiers.ensure(sockfd, [client, sockfd, multi = client->m_curl_multi] {
            auto notifier = Core::Notifier::construct(sockfd, Core::NotificationType::Write);
            notifier->on_activation = [client, sockfd, multi] {
                auto result = curl_multi_socket_action(multi, sockfd, CURL_CSELECT_OUT, nullptr);
                VERIFY(result == CURLM_OK);

                client->check_active_requests();
            };

            notifier->set_enabled(true);
            return notifier;
        });
    }

    return 0;
}

int ConnectionFromClient::on_timeout_callback(void*, long timeout_ms, void* user_data)
{
    auto* client = static_cast<ConnectionFromClient*>(user_data);
    if (!client->m_timer)
        return 0;

    if (timeout_ms < 0)
        client->m_timer->stop();
    else
        client->m_timer->restart(timeout_ms);

    return 0;
}

void ConnectionFromClient::check_active_requests()
{
    int msgs_in_queue = 0;
    while (auto* msg = curl_multi_info_read(m_curl_multi, &msgs_in_queue)) {
        if (msg->msg != CURLMSG_DONE)
            continue;

        void* application_private = nullptr;
        auto result = curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &application_private);
        VERIFY(result == CURLE_OK);
        VERIFY(application_private != nullptr);

        // FIXME: Come up with a unified way to track websockets and standard fetches instead of this nasty tagged pointer
        if (reinterpret_cast<uintptr_t>(application_private) & websocket_private_tag) {
            auto* websocket_impl = reinterpret_cast<WebSocketImplCurl*>(reinterpret_cast<uintptr_t>(application_private) & ~websocket_private_tag);
            if (msg->data.result == CURLE_OK) {
                if (!websocket_impl->did_connect())
                    websocket_impl->on_connection_error();
            } else {
                websocket_impl->on_connection_error();
            }
            continue;
        }

        auto* request = static_cast<Request*>(application_private);
        request->notify_fetch_complete({}, msg->data.result);
    }
}

Messages::RequestServer::StopRequestResponse ConnectionFromClient::stop_request(i32 request_id)
{
    // Security: Rate limiting
    if (!check_rate_limit())
        return false;

    // Security: Request ID validation
    if (!validate_request_id(request_id))
        return false;

    auto request = m_active_requests.take(request_id);
    if (!request.has_value()) {
        dbgln("StopRequest: Request ID {} not found", request_id);
        return false;
    }

    return true;
}

Messages::RequestServer::SetCertificateResponse ConnectionFromClient::set_certificate(i32 request_id, ByteString certificate, ByteString key)
{
    // Security: Rate limiting
    if (!check_rate_limit())
        return false;

    // Security: Certificate string validation
    if (!validate_string_length(certificate, "certificate"sv))
        return false;

    // Security: Key string validation
    if (!validate_string_length(key, "key"sv))
        return false;

    (void)request_id;
    (void)certificate;
    (void)key;
    TODO();
}

void ConnectionFromClient::ensure_connection(URL::URL url, ::RequestServer::CacheLevel cache_level)
{
    // Security: Rate limiting
    if (!check_rate_limit())
        return;

    // Security: URL validation (SSRF prevention)
    if (!validate_url(url))
        return;

    auto connect_only_request_id = get_random<i32>();

    auto request = Request::connect(connect_only_request_id, *this, m_curl_multi, m_resolver, move(url), cache_level);
    m_active_requests.set(connect_only_request_id, move(request));
}

void ConnectionFromClient::clear_cache()
{
    // Security: Rate limiting
    if (!check_rate_limit())
        return;

    if (g_disk_cache.has_value())
        g_disk_cache->clear_cache();
}

void ConnectionFromClient::websocket_connect(i64 websocket_id, URL::URL url, ByteString origin, Vector<ByteString> protocols, Vector<ByteString> extensions, HTTP::HeaderMap additional_request_headers)
{
    // Security: Rate limiting
    if (!check_rate_limit())
        return;

    // Security: URL validation (SSRF prevention)
    if (!validate_url(url))
        return;

    // Security: Origin string validation
    if (!validate_string_length(origin, "origin"sv))
        return;

    // Security: Protocols vector validation (DoS prevention)
    if (!validate_vector_size(protocols, "protocols"sv))
        return;

    // Security: Extensions vector validation (DoS prevention)
    if (!validate_vector_size(extensions, "extensions"sv))
        return;

    // Security: Header validation (CRLF injection prevention)
    if (!validate_header_map(additional_request_headers))
        return;

    auto host = url.serialized_host().to_byte_string();

    m_resolver->dns.lookup(host, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA })
        ->when_rejected([this, websocket_id](auto const& error) {
            dbgln("WebSocketConnect: DNS lookup failed: {}", error);
            async_websocket_errored(websocket_id, static_cast<i32>(Requests::WebSocket::Error::CouldNotEstablishConnection));
        })
        .when_resolved([this, websocket_id, host = move(host), url = move(url), origin = move(origin), protocols = move(protocols), extensions = move(extensions), additional_request_headers = move(additional_request_headers)](auto const& dns_result) mutable {
            if (dns_result->is_empty() || !dns_result->has_cached_addresses()) {
                dbgln("WebSocketConnect: DNS lookup failed for '{}'", host);
                async_websocket_errored(websocket_id, static_cast<i32>(Requests::WebSocket::Error::CouldNotEstablishConnection));
                return;
            }

            WebSocket::ConnectionInfo connection_info(move(url));
            connection_info.set_origin(move(origin));
            connection_info.set_protocols(move(protocols));
            connection_info.set_extensions(move(extensions));
            connection_info.set_headers(move(additional_request_headers));
            connection_info.set_dns_result(move(dns_result));

            if (auto const& path = default_certificate_path(); !path.is_empty())
                connection_info.set_root_certificates_path(path);

            auto impl = WebSocketImplCurl::create(m_curl_multi);
            auto connection = WebSocket::WebSocket::create(move(connection_info), move(impl));

            connection->on_open = [this, websocket_id]() {
                async_websocket_connected(websocket_id);
            };
            connection->on_message = [this, websocket_id](auto message) {
                async_websocket_received(websocket_id, message.is_text(), message.data());
            };
            connection->on_error = [this, websocket_id](auto message) {
                async_websocket_errored(websocket_id, (i32)message);
            };
            connection->on_close = [this, websocket_id](u16 code, ByteString reason, bool was_clean) {
                async_websocket_closed(websocket_id, code, move(reason), was_clean);
            };
            connection->on_ready_state_change = [this, websocket_id](auto state) {
                async_websocket_ready_state_changed(websocket_id, (u32)state);
            };

            connection->start();
            m_websockets.set(websocket_id, move(connection));
        });
}

void ConnectionFromClient::websocket_send(i64 websocket_id, bool is_text, ByteBuffer data)
{
    // Security: Rate limiting
    if (!check_rate_limit())
        return;

    // Security: WebSocket ID validation
    if (!validate_websocket_id(websocket_id))
        return;

    // Security: Data buffer size validation (buffer exhaustion prevention)
    if (!validate_buffer_size(data.size(), "websocket data"sv))
        return;

    if (auto connection = m_websockets.get(websocket_id).value_or({}); connection && connection->ready_state() == WebSocket::ReadyState::Open)
        connection->send(WebSocket::Message { move(data), is_text });
}

void ConnectionFromClient::websocket_close(i64 websocket_id, u16 code, ByteString reason)
{
    // Security: Rate limiting
    if (!check_rate_limit())
        return;

    // Security: WebSocket ID validation
    if (!validate_websocket_id(websocket_id))
        return;

    // Security: Reason string validation
    if (!validate_string_length(reason, "close reason"sv))
        return;

    if (auto connection = m_websockets.get(websocket_id).value_or({}); connection && connection->ready_state() == WebSocket::ReadyState::Open)
        connection->close(code, reason);
}

Messages::RequestServer::WebsocketSetCertificateResponse ConnectionFromClient::websocket_set_certificate(i64 websocket_id, ByteString certificate, ByteString key)
{
    // Security: Rate limiting
    if (!check_rate_limit())
        return false;

    // Security: WebSocket ID validation
    if (!validate_websocket_id(websocket_id))
        return false;

    // Security: Certificate string validation
    if (!validate_string_length(certificate, "certificate"sv))
        return false;

    // Security: Key string validation
    if (!validate_string_length(key, "key"sv))
        return false;

    auto success = false;
    if (auto connection = m_websockets.get(websocket_id).value_or({}); connection) {
        // NO OP here
        // connection->set_certificate(certificate, key);
        success = true;
    }
    return success;
}

void ConnectionFromClient::issue_ipfs_request(i32 request_id, ByteString method, URL::URL ipfs_url, HTTP::HeaderMap request_headers, ByteBuffer request_body, Core::ProxyData proxy_data, u64 page_id)
{
    // Extract CID from ipfs:// URL
    // Format: ipfs://CID or ipfs://CID/path
    auto path_string = ipfs_url.serialize_path().to_byte_string();
    if (path_string.starts_with("/"sv))
        path_string = path_string.substring(1);

    // Extract just the CID (before any path separator)
    auto cid_string = path_string;
    auto remaining_path = ByteString();
    if (auto slash_pos = path_string.find('/'); slash_pos.has_value()) {
        cid_string = path_string.substring(0, slash_pos.value());
        remaining_path = ByteString::formatted("/{}", path_string.substring(slash_pos.value() + 1));
    }

    dbgln("IPFS: Transforming ipfs://{} to gateway request", path_string);

    // Parse and validate CID
    auto parsed_cid_result = IPC::IPFSVerifier::parse_cid(cid_string);
    if (parsed_cid_result.is_error()) {
        dbgln("IPFS: Failed to parse CID '{}': {}", cid_string, parsed_cid_result.error());
        // Send error response to client
        async_request_finished(request_id, 0, {}, Requests::NetworkError::MalformedUrl);
        return;
    }

    auto parsed_cid = parsed_cid_result.release_value();
    dbgln("IPFS: Parsed CID version {} - {}", parsed_cid.version == IPC::CIDVersion::V0 ? "v0" : "v1", parsed_cid.raw_cid);

    // Store parsed_cid for verification after download
    m_pending_ipfs_verifications.set(request_id, move(parsed_cid));

    // Check if local IPFS daemon is available
    bool use_local_daemon = IPC::IPFSAvailability::is_daemon_running();

    // Store fallback info for retry on failure
    // Clone headers and body since they'll be moved
    GatewayFallbackInfo fallback_info {
        .protocol = GatewayProtocol::IPFS,
        .current_gateway_index = static_cast<size_t>(use_local_daemon ? 0 : 1), // Start with local or first public gateway
        .resource_identifier = cid_string,
        .path = remaining_path,
        .method = method,
        .headers = request_headers,
        .body = MUST(ByteBuffer::copy(request_body)),
        .proxy_data = proxy_data,
        .page_id = page_id
    };

    m_gateway_fallback_requests.set(request_id, move(fallback_info));

    // Construct initial gateway URL
    URL::URL gateway_url;
    size_t initial_gateway_index = use_local_daemon ? 0 : 1;
    auto gateway_base = s_ipfs_gateways[initial_gateway_index];

    auto url_opt = URL::create_with_url_or_path(
        ByteString::formatted("{}/ipfs/{}{}", gateway_base, cid_string, remaining_path));
    gateway_url = url_opt.value();

    dbgln("IPFS: Trying gateway {} ({})", initial_gateway_index + 1, gateway_base);

    // Issue the transformed HTTP request via standard network request
    issue_network_request(request_id, move(method), move(gateway_url), move(request_headers), move(request_body), proxy_data, page_id);
}

void ConnectionFromClient::issue_ipns_request(i32 request_id, ByteString method, URL::URL ipns_url, HTTP::HeaderMap request_headers, ByteBuffer request_body, Core::ProxyData proxy_data, u64 page_id)
{
    // Extract IPNS name from ipns:// URL
    // Format: ipns://name or ipns://name/path
    // IPNS names can be:
    // - IPNS hashes (like CIDs but for mutable content)
    // - Domain names (with DNSLink)
    auto path_string = ipns_url.serialize_path().to_byte_string();
    if (path_string.starts_with("/"sv))
        path_string = path_string.substring(1);

    // Extract just the name (before any path separator)
    auto name_string = path_string;
    auto remaining_path = ByteString();
    if (auto slash_pos = path_string.find('/'); slash_pos.has_value()) {
        name_string = path_string.substring(0, slash_pos.value());
        remaining_path = ByteString::formatted("/{}", path_string.substring(slash_pos.value() + 1));
    }

    dbgln("IPNS: Transforming ipns://{} to gateway request", path_string);

    // IPNS names don't need validation like CIDs - they're resolved by the gateway
    // The gateway will return 404 if the name doesn't exist

    // Check if local IPFS daemon is available
    bool use_local_daemon = IPC::IPFSAvailability::is_daemon_running();

    // Store fallback info for retry on failure
    GatewayFallbackInfo fallback_info {
        .protocol = GatewayProtocol::IPNS,
        .current_gateway_index = static_cast<size_t>(use_local_daemon ? 0 : 1), // Start with local or first public gateway
        .resource_identifier = name_string,
        .path = remaining_path,
        .method = method,
        .headers = request_headers,
        .body = MUST(ByteBuffer::copy(request_body)),
        .proxy_data = proxy_data,
        .page_id = page_id
    };

    m_gateway_fallback_requests.set(request_id, move(fallback_info));

    // Construct initial gateway URL
    URL::URL gateway_url;
    size_t initial_gateway_index = use_local_daemon ? 0 : 1;
    auto gateway_base = s_ipns_gateways[initial_gateway_index];

    auto url_opt = URL::create_with_url_or_path(
        ByteString::formatted("{}/ipns/{}{}", gateway_base, name_string, remaining_path));
    gateway_url = url_opt.value();

    dbgln("IPNS: Trying gateway {} ({})", initial_gateway_index + 1, gateway_base);

    // Note: IPNS content cannot be verified by hash like IPFS content
    // IPNS names can be updated to point to different CIDs over time
    // We trust the gateway to resolve the current CID for the IPNS name

    // Issue the transformed HTTP request via standard network request
    issue_network_request(request_id, move(method), move(gateway_url), move(request_headers), move(request_body), proxy_data, page_id);
}

void ConnectionFromClient::issue_ens_request(i32 request_id, ByteString method, URL::URL ens_url, HTTP::HeaderMap request_headers, ByteBuffer request_body, Core::ProxyData proxy_data, u64 page_id)
{
    // Extract .eth domain and path from URL
    // Format: http://example.eth/path or https://example.eth/path
    auto eth_domain = ens_url.serialized_host().to_byte_string();
    auto path = ens_url.serialize_path().to_byte_string();
    auto query = ens_url.query().value_or({}).to_byte_string();
    auto fragment = ens_url.fragment().value_or({}).to_byte_string();

    // Build path string with query and fragment
    StringBuilder path_builder;
    path_builder.append(path);
    if (!query.is_empty()) {
        path_builder.append('?');
        path_builder.append(query);
    }
    if (!fragment.is_empty()) {
        path_builder.append('#');
        path_builder.append(fragment);
    }
    auto full_path = path_builder.to_byte_string();

    dbgln("ENS: Resolving {}{}", eth_domain, full_path);

    // Store fallback info for retry on failure
    GatewayFallbackInfo fallback_info {
        .protocol = GatewayProtocol::ENS,
        .current_gateway_index = 0, // Start with first gateway (.limo)
        .resource_identifier = eth_domain,
        .path = full_path,
        .method = method,
        .headers = request_headers,
        .body = MUST(ByteBuffer::copy(request_body)),
        .proxy_data = proxy_data,
        .page_id = page_id
    };

    m_gateway_fallback_requests.set(request_id, move(fallback_info));

    // Construct gateway URL using first gateway (.limo)
    // Primary: https://example.eth.limo/path
    // Pattern: append suffix to .eth domain for HTTPS resolution
    auto gateway_suffix = s_ens_gateways[0];
    auto gateway_host = ByteString::formatted("{}{}", eth_domain, gateway_suffix);

    StringBuilder url_builder;
    url_builder.append("https://"sv);
    url_builder.append(gateway_host);
    url_builder.append(full_path);

    auto gateway_url_string = url_builder.to_byte_string();
    auto url_opt = URL::create_with_url_or_path(gateway_url_string);
    if (!url_opt.has_value()) {
        dbgln("ENS: Failed to construct gateway URL: {}", gateway_url_string);
        async_request_finished(request_id, 0, {}, Requests::NetworkError::MalformedUrl);
        return;
    }

    auto gateway_url = url_opt.value();
    dbgln("ENS: Trying gateway 1 ({}{}): {}", eth_domain, gateway_suffix, gateway_url.to_string());

    // Note: ENS names are resolved by the gateway to IPFS content or other addresses
    // The gateway handles all blockchain resolution
    // No local validation needed - gateway will return 404 if domain doesn't exist

    // Issue the transformed HTTP request via standard network request
    issue_network_request(request_id, move(method), move(gateway_url), move(request_headers), move(request_body), proxy_data, page_id);
}

void ConnectionFromClient::retry_with_next_gateway(i32 request_id)
{
    // Check if this request has fallback info
    auto fallback_it = m_gateway_fallback_requests.find(request_id);
    if (fallback_it == m_gateway_fallback_requests.end()) {
        dbgln("Gateway fallback: No fallback info for request {}", request_id);
        return;
    }

    auto& fallback = fallback_it->value;
    fallback.current_gateway_index++;

    // Determine gateway count based on protocol
    size_t gateway_count = 0;
    switch (fallback.protocol) {
    case GatewayProtocol::IPFS:
        gateway_count = sizeof(s_ipfs_gateways) / sizeof(s_ipfs_gateways[0]);
        break;
    case GatewayProtocol::IPNS:
        gateway_count = sizeof(s_ipns_gateways) / sizeof(s_ipns_gateways[0]);
        break;
    case GatewayProtocol::ENS:
        gateway_count = sizeof(s_ens_gateways) / sizeof(s_ens_gateways[0]);
        break;
    }

    // Check if we've exhausted all gateways
    if (fallback.current_gateway_index >= gateway_count) {
        ByteString protocol_name;
        ByteString user_message;

        switch (fallback.protocol) {
        case GatewayProtocol::IPFS:
            protocol_name = "IPFS";
            user_message = ByteString::formatted(
                "Failed to load IPFS content '{}' after trying {} gateways. "
                "This may indicate the content is not available on the IPFS network, "
                "or all gateways are currently unreachable. "
                "You can try again later or check if a local IPFS daemon is running.",
                fallback.resource_identifier, gateway_count);
            break;
        case GatewayProtocol::IPNS:
            protocol_name = "IPNS";
            user_message = ByteString::formatted(
                "Failed to resolve IPNS name '{}' after trying {} gateways. "
                "The name may not exist, may have expired, or all gateways are unreachable. "
                "Verify the IPNS name is correct and try again later.",
                fallback.resource_identifier, gateway_count);
            break;
        case GatewayProtocol::ENS:
            protocol_name = "ENS";
            user_message = ByteString::formatted(
                "Failed to resolve ENS domain '{}' after trying {} gateways. "
                "The domain may not be registered, may not have content records set, "
                "or the ENS gateways are unreachable. "
                "Verify the domain exists on the Ethereum blockchain.",
                fallback.resource_identifier, gateway_count);
            break;
        }

        dbgln("Gateway fallback: Exhausted all {} gateways for {} request {}: {}",
            gateway_count, protocol_name, request_id, user_message);

        // Clean up fallback info
        m_gateway_fallback_requests.remove(request_id);

        // Send final error to client with context
        // Note: Would be better to send custom error message, but NetworkError enum is limited
        // The debug message above provides context in browser console
        async_request_finished(request_id, 0, {}, Requests::NetworkError::UnableToConnect);
        return;
    }

    // Construct new gateway URL
    URL::URL gateway_url;
    ByteString gateway_url_string;

    switch (fallback.protocol) {
    case GatewayProtocol::IPFS: {
        auto gateway_base = s_ipfs_gateways[fallback.current_gateway_index];
        gateway_url_string = ByteString::formatted("{}/ipfs/{}{}",
            gateway_base,
            fallback.resource_identifier,
            fallback.path);
        dbgln("Gateway fallback: IPFS trying gateway {} ({}): {}",
            fallback.current_gateway_index + 1,
            gateway_base,
            gateway_url_string);
        break;
    }
    case GatewayProtocol::IPNS: {
        auto gateway_base = s_ipns_gateways[fallback.current_gateway_index];
        gateway_url_string = ByteString::formatted("{}/ipns/{}{}",
            gateway_base,
            fallback.resource_identifier,
            fallback.path);
        dbgln("Gateway fallback: IPNS trying gateway {} ({}): {}",
            fallback.current_gateway_index + 1,
            gateway_base,
            gateway_url_string);
        break;
    }
    case GatewayProtocol::ENS: {
        auto gateway_suffix = s_ens_gateways[fallback.current_gateway_index];
        auto gateway_host = ByteString::formatted("{}{}", fallback.resource_identifier, gateway_suffix);

        StringBuilder url_builder;
        url_builder.append("https://"sv);
        url_builder.append(gateway_host);
        url_builder.append(fallback.path);
        gateway_url_string = url_builder.to_byte_string();

        dbgln("Gateway fallback: ENS trying gateway {} ({} → {}): {}",
            fallback.current_gateway_index + 1,
            fallback.resource_identifier,
            gateway_host,
            gateway_url_string);
        break;
    }
    }

    // Parse the URL
    auto url_opt = URL::create_with_url_or_path(gateway_url_string);
    if (!url_opt.has_value()) {
        dbgln("Gateway fallback: Failed to construct gateway URL: {}", gateway_url_string);
        retry_with_next_gateway(request_id); // Try next gateway
        return;
    }

    gateway_url = url_opt.value();

    // Copy request parameters (we need to clone them since they're consumed)
    auto method_copy = fallback.method;
    auto headers_copy = fallback.headers;
    auto body_copy = MUST(ByteBuffer::copy(fallback.body));
    auto proxy_data_copy = fallback.proxy_data;
    auto page_id_copy = fallback.page_id;

    // Cancel current request if it exists
    if (m_active_requests.contains(request_id)) {
        m_active_requests.remove(request_id);
    }

    // Issue the new request with the next gateway
    issue_network_request(request_id, move(method_copy), move(gateway_url),
                         move(headers_copy), move(body_copy),
                         proxy_data_copy, page_id_copy);
}

// IPFS Integration: URL transformation helpers

URL::URL ConnectionFromClient::transform_ipfs_url_to_gateway(i32 request_id, URL::URL const& ipfs_url, u64 page_id)
{
    (void)request_id;
    (void)page_id;
    auto path_string = ipfs_url.serialize_path().to_byte_string();
    if (path_string.starts_with("/"sv))
        path_string = path_string.substring(1);

    auto cid_string = path_string;
    auto remaining_path = ByteString();
    if (auto slash_pos = path_string.find('/'); slash_pos.has_value()) {
        cid_string = path_string.substring(0, slash_pos.value());
        remaining_path = path_string.substring(slash_pos.value());
    }

    auto gateway_base = s_ipfs_gateways[0];
    auto url_string = ByteString::formatted("{}/ipfs/{}{}", gateway_base, cid_string, remaining_path);
    return URL::create_with_url_or_path(url_string).value_or(ipfs_url);
}

URL::URL ConnectionFromClient::transform_ipns_url_to_gateway(i32 request_id, URL::URL const& ipns_url, u64 page_id)
{
    (void)request_id;
    (void)page_id;
    auto path_string = ipns_url.serialize_path().to_byte_string();
    if (path_string.starts_with("/"sv))
        path_string = path_string.substring(1);

    auto name_string = path_string;
    auto remaining_path = ByteString();
    if (auto slash_pos = path_string.find('/'); slash_pos.has_value()) {
        name_string = path_string.substring(0, slash_pos.value());
        remaining_path = path_string.substring(slash_pos.value());
    }

    auto gateway_base = s_ipns_gateways[0];
    auto url_string = ByteString::formatted("{}/ipns/{}{}", gateway_base, name_string, remaining_path);
    return URL::create_with_url_or_path(url_string).value_or(ipns_url);
}

URL::URL ConnectionFromClient::transform_ens_url_to_gateway(i32 request_id, URL::URL const& ens_url, u64 page_id)
{
    (void)request_id;
    (void)page_id;
    auto eth_domain = ens_url.serialized_host().to_byte_string();
    auto path = ens_url.serialize_path().to_byte_string();

    auto gateway_suffix = s_ens_gateways[0];
    auto gateway_host = ByteString::formatted("{}{}", eth_domain, gateway_suffix);
    auto url_string = ByteString::formatted("https://{}{}", gateway_host, path);

    return URL::create_with_url_or_path(url_string).value_or(ens_url);
}

void ConnectionFromClient::setup_ipfs_verification(i32 request_id, Request& request, ByteString const& cid_string)
{
    auto parsed_cid_result = IPC::IPFSVerifier::parse_cid(cid_string);
    if (parsed_cid_result.is_error()) {
        dbgln("Failed to parse CID: {}", parsed_cid_result.error());
        return;
    }

    auto parsed_cid = parsed_cid_result.release_value();
    m_pending_ipfs_verifications.set(request_id, move(parsed_cid));

    request.set_content_verification_callback([this, request_id](ReadonlyBytes content) -> ErrorOr<bool> {
        auto cid_it = m_pending_ipfs_verifications.find(request_id);
        if (cid_it == m_pending_ipfs_verifications.end())
            return Error::from_string_literal("No CID found for verification");

        auto result = TRY(IPC::IPFSVerifier::verify_content(cid_it->value, content));
        m_pending_ipfs_verifications.remove(request_id);
        return result;
    });
}

void ConnectionFromClient::setup_gateway_fallback(i32 request_id, Request& request, Request::ProtocolType protocol,
    ByteString const& resource_id, ByteString const& path, ByteString const& method,
    HTTP::HeaderMap const& headers, ByteBuffer const& body, Core::ProxyData const& proxy_data, u64 page_id)
{
    auto body_clone_result = ByteBuffer::copy(body);
    if (body_clone_result.is_error()) {
        dbgln("Failed to clone body for gateway fallback: {}", body_clone_result.error());
        return;
    }

    // Convert Request::ProtocolType to GatewayProtocol
    GatewayProtocol gateway_protocol;
    switch (protocol) {
    case Request::ProtocolType::IPFS:
        gateway_protocol = GatewayProtocol::IPFS;
        break;
    case Request::ProtocolType::IPNS:
        gateway_protocol = GatewayProtocol::IPNS;
        break;
    case Request::ProtocolType::ENS:
        gateway_protocol = GatewayProtocol::ENS;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    GatewayFallbackInfo info {
        .protocol = gateway_protocol,
        .current_gateway_index = 0,
        .resource_identifier = resource_id,
        .path = path,
        .method = method,
        .headers = headers,
        .body = body_clone_result.release_value(),
        .proxy_data = proxy_data,
        .page_id = page_id,
    };
    m_gateway_fallback_requests.set(request_id, move(info));

    request.set_gateway_fallback_callback([this, request_id]() {
        retry_with_next_gateway(request_id);
    });
}

}
