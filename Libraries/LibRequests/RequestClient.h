/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibHTTP/HeaderMap.h>
#include <LibIPC/ConnectionToServer.h>
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

    explicit RequestClient(IPC::Transport);
    virtual ~RequestClient() override;

    RefPtr<Request> start_request(ByteString const& method, URL::URL const&, HTTP::HeaderMap const& request_headers = {}, ReadonlyBytes request_body = {}, Core::ProxyData const& = {});

    RefPtr<WebSocket> websocket_connect(const URL::URL&, ByteString const& origin = {}, Vector<ByteString> const& protocols = {}, Vector<ByteString> const& extensions = {}, HTTP::HeaderMap const& request_headers = {});

    void ensure_connection(URL::URL const&, ::RequestServer::CacheLevel);

    bool stop_request(Badge<Request>, Request&);
    bool set_certificate(Badge<Request>, Request&, ByteString, ByteString);

private:
    virtual void die() override;

    virtual void request_started(i32, IPC::File const&) override;
    virtual void request_finished(i32, u64, RequestTimingInfo const&, Optional<NetworkError> const&) override;
    virtual void certificate_requested(i32) override;
    virtual void headers_became_available(i32, HTTP::HeaderMap const&, Optional<u32> const&, Optional<String> const&) override;

    virtual void websocket_connected(i64 websocket_id) override;
    virtual void websocket_received(i64 websocket_id, bool, ByteBuffer const&) override;
    virtual void websocket_errored(i64 websocket_id, i32) override;
    virtual void websocket_closed(i64 websocket_id, u16, ByteString const&, bool) override;
    virtual void websocket_ready_state_changed(i64 websocket_id, u32 ready_state) override;
    virtual void websocket_subprotocol(i64 websocket_id, ByteString const& subprotocol) override;
    virtual void websocket_certificate_requested(i64 websocket_id) override;

    HashMap<i32, RefPtr<Request>> m_requests;
    HashMap<i64, NonnullRefPtr<WebSocket>> m_websockets;

    i64 m_next_websocket_id { 0 };
};

}
