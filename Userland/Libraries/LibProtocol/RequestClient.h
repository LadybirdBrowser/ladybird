/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibHTTP/HeaderMap.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibProtocol/WebSocket.h>
#include <LibWebSocket/WebSocket.h>
#include <RequestServer/RequestClientEndpoint.h>
#include <RequestServer/RequestServerEndpoint.h>

namespace Protocol {

class Request;

class RequestClient final
    : public IPC::ConnectionToServer<RequestClientEndpoint, RequestServerEndpoint>
    , public RequestClientEndpoint {
    IPC_CLIENT_CONNECTION(RequestClient, "/tmp/session/%sid/portal/request"sv)

public:
    explicit RequestClient(NonnullOwnPtr<Core::LocalSocket>);
    virtual ~RequestClient() override;

    RefPtr<Request> start_request(ByteString const& method, URL::URL const&, HTTP::HeaderMap const& request_headers = {}, ReadonlyBytes request_body = {}, Core::ProxyData const& = {});

    RefPtr<WebSocket> websocket_connect(const URL::URL&, ByteString const& origin = {}, Vector<ByteString> const& protocols = {}, Vector<ByteString> const& extensions = {}, HTTP::HeaderMap const& request_headers = {});

    void ensure_connection(URL::URL const&, ::RequestServer::CacheLevel);

    bool stop_request(Badge<Request>, Request&);
    bool set_certificate(Badge<Request>, Request&, ByteString, ByteString);

    void dump_connection_info();

private:
    virtual void die() override;

    virtual void request_started(i32, IPC::File const&) override;
    virtual void request_progress(i32, Optional<u64> const&, u64) override;
    virtual void request_finished(i32, bool, u64) override;
    virtual void certificate_requested(i32) override;
    virtual void headers_became_available(i32, HTTP::HeaderMap const&, Optional<u32> const&) override;

    virtual void websocket_connected(i32) override;
    virtual void websocket_received(i32, bool, ByteBuffer const&) override;
    virtual void websocket_errored(i32, i32) override;
    virtual void websocket_closed(i32, u16, ByteString const&, bool) override;
    virtual void websocket_certificate_requested(i32) override;

    HashMap<i32, RefPtr<Request>> m_requests;
    HashMap<i32, NonnullRefPtr<WebSocket>> m_websockets;
};

}
