/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibHTTP/Cache/CacheMode.h>
#include <LibHTTP/Cookie/IncludeCredentials.h>
#include <LibHTTP/HeaderList.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibRequests/CacheSizes.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibRequests/WebSocket.h>
#include <LibWebSocket/WebSocket.h>
#include <RequestServer/RequestClientEndpoint.h>
#include <RequestServer/RequestServerEndpoint.h>

namespace Requests {

class Request;

class RequestClient final
    : public IPC::ConnectionToServer<RequestClientEndpoint, RequestServerEndpoint>
    , public RequestClientEndpoint {
    C_OBJECT_ABSTRACT(RequestClient)

public:
    using InitTransport = Messages::RequestServer::InitTransport;

    explicit RequestClient(NonnullOwnPtr<IPC::Transport>);
    virtual ~RequestClient() override;

    RefPtr<Request> start_request(ByteString const& method, URL::URL const&, Optional<HTTP::HeaderList const&> request_headers = {}, ReadonlyBytes request_body = {}, HTTP::CacheMode = HTTP::CacheMode::Default, HTTP::Cookie::IncludeCredentials = HTTP::Cookie::IncludeCredentials::Yes, Core::ProxyData const& = {});
    bool stop_request(Badge<Request>, Request&);
    void ensure_connection(URL::URL const&, RequestServer::CacheLevel);

    bool set_certificate(Badge<Request>, Request&, ByteString, ByteString);

    RefPtr<WebSocket> websocket_connect(URL::URL const&, ByteString const& origin, Vector<ByteString> const& protocols, Vector<ByteString> const& extensions, HTTP::HeaderList const& request_headers);

    NonnullRefPtr<Core::Promise<CacheSizes>> estimate_cache_size_accessed_since(UnixDateTime since);

    Function<String(URL::URL const&)> on_retrieve_http_cookie;
    Function<void()> on_request_server_died;

private:
    virtual void die() override;

    virtual void request_started(u64 request_id, IPC::File) override;
    virtual void request_finished(u64 request_id, u64, RequestTimingInfo, Optional<NetworkError>) override;
    virtual void headers_became_available(u64 request_id, Vector<HTTP::Header>, Optional<u32>, Optional<String>) override;

    virtual void retrieve_http_cookie(int client_id, u64 request_id, URL::URL url) override;

    virtual void certificate_requested(u64 request_id) override;

    virtual void websocket_connected(u64 websocket_id) override;
    virtual void websocket_received(u64 websocket_id, bool, ByteBuffer) override;
    virtual void websocket_errored(u64 websocket_id, i32) override;
    virtual void websocket_closed(u64 websocket_id, u16, ByteString, bool) override;
    virtual void websocket_ready_state_changed(u64 websocket_id, u32 ready_state) override;
    virtual void websocket_subprotocol(u64 websocket_id, ByteString subprotocol) override;
    virtual void websocket_certificate_requested(u64 websocket_id) override;

    virtual void estimated_cache_size(u64 cache_size_estimation_id, CacheSizes sizes) override;

    HashMap<u64, RefPtr<Request>> m_requests;
    u64 m_next_request_id { 0 };

    HashMap<u64, NonnullRefPtr<WebSocket>> m_websockets;
    u64 m_next_websocket_id { 0 };

    HashMap<u64, NonnullRefPtr<Core::Promise<CacheSizes>>> m_pending_cache_size_estimations;
    u64 m_next_cache_size_estimation_id { 0 };
};

}
