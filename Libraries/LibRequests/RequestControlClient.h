/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibCore/Promise.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibRequests/CacheSizes.h>
#include <LibRequests/NetworkUsage.h>
#include <RequestServer/IsPrivate.h>
#include <RequestServer/RequestServerControlClientEndpoint.h>
#include <RequestServer/RequestServerControlEndpoint.h>

namespace Requests {

// The control plane of RequestServer, spoken only by the process that launched it.
class RequestControlClient final
    : public IPC::ConnectionToServer<RequestServerControlClientEndpoint, RequestServerControlEndpoint>
    , public RequestServerControlClientEndpoint {
    C_OBJECT_ABSTRACT(RequestControlClient)

public:
    using InitTransport = Messages::RequestServerControl::InitTransport;

    explicit RequestControlClient(NonnullOwnPtr<IPC::Transport>);
    virtual ~RequestControlClient() override;

    NonnullRefPtr<Core::Promise<CacheSizes>> estimate_cache_size_accessed_since(UnixDateTime since);
    NonnullRefPtr<Core::Promise<Empty>> clear_cache(UnixDateTime since);

    Function<void(Vector<NetworkUsage>, u64 interval_microseconds)> on_network_usage;
    Function<String(URL::URL const&, RequestServer::IsPrivate)> on_retrieve_http_cookie;
    Function<void()> on_request_server_died;

private:
    virtual void die() override;

    virtual void network_usage(Vector<NetworkUsage> usage, u64 interval_microseconds) override;
    virtual void retrieve_http_cookie(int client_id, u64 request_id, RequestServer::RequestType request_type, u64 cookie_request_id, URL::URL url, RequestServer::IsPrivate) override;
    virtual void estimated_cache_size(u64 cache_size_estimation_id, CacheSizes sizes) override;
    virtual void removed_cache_entries(u64 clear_cache_request_id) override;

    HashMap<u64, NonnullRefPtr<Core::Promise<CacheSizes>>> m_pending_cache_size_estimations;
    u64 m_next_cache_size_estimation_id { 0 };

    HashMap<u64, NonnullRefPtr<Core::Promise<Empty>>> m_pending_clear_cache_requests;
    u64 m_next_clear_cache_request_id { 0 };
};

}
