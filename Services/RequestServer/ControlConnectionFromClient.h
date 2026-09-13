/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibCore/Timer.h>
#include <LibHTTP/Cache/DiskCacheSettings.h>
#include <LibHTTP/Forward.h>
#include <LibIPC/ConnectionFromClient.h>
#include <RequestServer/CURL.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/Forward.h>
#include <RequestServer/RequestServerControlClientEndpoint.h>
#include <RequestServer/RequestServerControlEndpoint.h>

namespace RequestServer {

// The control plane of RequestServer. Only the process that launched RequestServer speaks this endpoint, over the
// initial socket. Everything that applies to the process as a whole lives here; data connections cannot reach it.
class ControlConnectionFromClient final
    : public IPC::ConnectionFromClient<RequestServerControlClientEndpoint, RequestServerControlEndpoint> {
    C_OBJECT(ControlConnectionFromClient);

public:
    ~ControlConnectionFromClient() override;

    virtual void die() override;

    static Optional<ControlConnectionFromClient&> the();

private:
    ControlConnectionFromClient(NonnullOwnPtr<IPC::Transport>, RequestServer::ConnectionFromClient::ConnectionMap&, RequestServer::ConnectionFromClient::RequestTransferLeaseMap&, Optional<HTTP::DiskCache&>, ByteString alt_svc_cache_path);

    virtual Messages::RequestServerControl::InitTransportResponse init_transport(int peer_pid) override;

    virtual Messages::RequestServerControl::ConnectNewClientResponse connect_new_client(IsPrivate) override;
    virtual Messages::RequestServerControl::ConnectNewClientsResponse connect_new_clients(size_t count, IsPrivate) override;

    virtual void set_disk_cache_settings(HTTP::DiskCacheSettings) override;

    virtual void set_dns_server(ByteString host_or_address, u16 port, bool use_tls, bool validate_dnssec_locally) override;
    virtual void set_use_system_dns() override;

    virtual void set_performance_monitor_enabled(bool) override;

    virtual void estimate_cache_size_accessed_since(u64 cache_size_estimation_id, UnixDateTime since) override;
    virtual void remove_cache_entries_accessed_since(u64 clear_cache_request_id, UnixDateTime since) override;

    virtual void retrieved_http_cookie(int client_id, u64 request_id, RequestType request_type, u64 cookie_request_id, String cookie) override;

    ErrorOr<IPC::TransportHandle> create_client_socket(IsPrivate);
    void push_network_usage();

    RequestServer::ConnectionFromClient::ConnectionMap& m_connections;
    RequestServer::ConnectionFromClient::RequestTransferLeaseMap& m_request_transfer_leases;
    Optional<HTTP::DiskCache&> m_disk_cache;
    ByteString m_alt_svc_cache_path;

    NonnullRefPtr<Resolver> m_resolver;

    RefPtr<Core::Timer> m_performance_timer;
    Optional<MonotonicTime> m_last_performance_push;
};

}
