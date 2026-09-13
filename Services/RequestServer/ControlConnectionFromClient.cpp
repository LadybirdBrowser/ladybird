/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibIPC/TransportHandle.h>
#include <RequestServer/ControlConnectionFromClient.h>
#include <RequestServer/Request.h>
#include <RequestServer/Resolver.h>

namespace RequestServer {

static ControlConnectionFromClient* s_control_connection = nullptr;

ControlConnectionFromClient::ControlConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport, RequestServer::ConnectionFromClient::ConnectionMap& connections, RequestServer::ConnectionFromClient::RequestTransferLeaseMap& request_transfer_leases, Optional<HTTP::DiskCache&> disk_cache, ByteString alt_svc_cache_path)
    : IPC::ConnectionFromClient<RequestServerControlClientEndpoint, RequestServerControlEndpoint>(*this, move(transport), 0)
    , m_connections(connections)
    , m_request_transfer_leases(request_transfer_leases)
    , m_disk_cache(disk_cache)
    , m_alt_svc_cache_path(move(alt_svc_cache_path))
    , m_resolver(Resolver::default_resolver())
{
    VERIFY(s_control_connection == nullptr);
    s_control_connection = this;
}

ControlConnectionFromClient::~ControlConnectionFromClient()
{
    if (s_control_connection == this)
        s_control_connection = nullptr;
}

Optional<ControlConnectionFromClient&> ControlConnectionFromClient::the()
{
    if (s_control_connection)
        return *s_control_connection;
    return {};
}

void ControlConnectionFromClient::die()
{
    m_performance_timer = nullptr;
    Request::set_performance_monitor_enabled(false);

    if (s_control_connection == this)
        s_control_connection = nullptr;

    if (m_connections.is_empty())
        Core::EventLoop::current().quit(0);
}

Messages::RequestServerControl::InitTransportResponse ControlConnectionFromClient::init_transport([[maybe_unused]] int peer_pid)
{
#ifdef AK_OS_WINDOWS
    m_transport->set_peer_pid(peer_pid);
    return Core::System::getpid();
#endif
    VERIFY_NOT_REACHED();
}

ErrorOr<IPC::TransportHandle> ControlConnectionFromClient::create_client_socket(IsPrivate is_private)
{
    auto paired = TRY(IPC::Transport::create_paired());
    auto handle = move(paired.remote_handle);
    auto disk_cache = is_private == IsPrivate::Yes ? Optional<HTTP::DiskCache&> {} : m_disk_cache;

    // Note: A ref is stored in the m_connections map
    auto client = adopt_ref(*new RequestServer::ConnectionFromClient(move(paired.local), is_private, m_connections, m_request_transfer_leases, disk_cache, m_alt_svc_cache_path));

    return handle;
}

Messages::RequestServerControl::ConnectNewClientResponse ControlConnectionFromClient::connect_new_client(IsPrivate is_private)
{
    auto client_socket = create_client_socket(is_private);
    if (client_socket.is_error()) {
        dbgln("Failed to create client socket: {}", client_socket.error());
        return IPC::TransportHandle {};
    }

    return client_socket.release_value();
}

Messages::RequestServerControl::ConnectNewClientsResponse ControlConnectionFromClient::connect_new_clients(size_t count, IsPrivate is_private)
{
    Vector<IPC::TransportHandle> handles;
    handles.ensure_capacity(count);

    for (size_t i = 0; i < count; ++i) {
        auto client_socket = create_client_socket(is_private);
        if (client_socket.is_error()) {
            dbgln("Failed to create client socket: {}", client_socket.error());
            return Vector<IPC::TransportHandle> {};
        }

        handles.unchecked_append(client_socket.release_value());
    }

    return handles;
}

void ControlConnectionFromClient::set_disk_cache_settings(HTTP::DiskCacheSettings disk_cache_settings)
{
    if (m_disk_cache.has_value())
        m_disk_cache->set_maximum_disk_cache_size(disk_cache_settings.maximum_size);
}

void ControlConnectionFromClient::set_dns_server(ByteString host_or_address, u16 port, bool use_tls, bool validate_dnssec_locally)
{
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

void ControlConnectionFromClient::set_use_system_dns()
{
    auto& dns_info = DNSInfo::the();
    dns_info.server_hostname = {};
    dns_info.server_address = {};

    m_resolver->dns.reset_connection();
}

void ControlConnectionFromClient::set_performance_monitor_enabled(bool enabled)
{
    Request::set_performance_monitor_enabled(enabled);
    if (enabled) {
        if (!m_performance_timer) {
            // NB: Establish baselines for requests already in flight before enabling the monitor.
            for (auto& connection : m_connections) {
                for (auto& request : connection.value->m_active_requests)
                    request.value->sample_network_usage();
            }
            m_last_performance_push = MonotonicTime::now();
            m_performance_timer = Core::Timer::create_repeating(500, [this] { push_network_usage(); });
            m_performance_timer->start();
        }
    } else {
        m_performance_timer = nullptr;
    }
}

void ControlConnectionFromClient::push_network_usage()
{
    for (auto& connection : m_connections) {
        for (auto& request : connection.value->m_active_requests)
            request.value->sample_network_usage();
    }
    auto now = MonotonicTime::now();
    async_network_usage(Request::take_network_usage(), (now - *m_last_performance_push).to_microseconds());
    m_last_performance_push = now;
}

void ControlConnectionFromClient::estimate_cache_size_accessed_since(u64 cache_size_estimation_id, UnixDateTime since)
{
    Requests::CacheSizes sizes;

    if (m_disk_cache.has_value())
        sizes = m_disk_cache->estimate_cache_size_accessed_since(since);

    async_estimated_cache_size(cache_size_estimation_id, sizes);
}

void ControlConnectionFromClient::remove_cache_entries_accessed_since(u64 clear_cache_request_id, UnixDateTime since)
{
    if (m_disk_cache.has_value())
        m_disk_cache->remove_entries_accessed_since(since);

    async_removed_cache_entries(clear_cache_request_id);
}

void ControlConnectionFromClient::retrieved_http_cookie(int client_id, u64 request_id, RequestType request_type, u64 cookie_request_id, String cookie)
{
    note_event_tick("ipc-retrieved-cookie"sv);

    if (auto connection = m_connections.get(client_id); connection.has_value()) {
        auto request = [&]() {
            switch (request_type) {
            case RequestType::Fetch:
                return (*connection)->m_active_requests.get(request_id);
            case RequestType::BackgroundRevalidation:
                return (*connection)->m_active_revalidation_requests.get(request_id);
            case RequestType::Connect:
                did_misbehave("HTTP cookie response has an invalid request type");
                return decltype((*connection)->m_active_requests.get(request_id)) {};
            }
            VERIFY_NOT_REACHED();
        }();

        if (request.has_value() && !(*request)->notify_retrieved_http_cookie({}, cookie_request_id, cookie))
            did_misbehave("Duplicate or unexpected HTTP cookie response");
    }
}

}
