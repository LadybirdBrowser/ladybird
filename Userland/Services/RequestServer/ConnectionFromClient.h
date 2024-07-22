/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibCore/SharedCircularQueue.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibThreading/MutexProtected.h>
#include <LibThreading/ThreadPool.h>
#include <LibWebSocket/WebSocket.h>
#include <RequestServer/Forward.h>
#include <RequestServer/RequestClientEndpoint.h>
#include <RequestServer/RequestServerEndpoint.h>

namespace RequestServer {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<RequestClientEndpoint, RequestServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

public:
    ~ConnectionFromClient() override = default;

    virtual void die() override;

    void did_receive_headers(Badge<Request>, Request&);
    void did_finish_request(Badge<Request>, Request&, bool success);
    void did_progress_request(Badge<Request>, Request&);
    void did_request_certificates(Badge<Request>, Request&);

    struct StartRequest {
        i32 request_id;
        ByteString method;
        URL::URL url;
        HTTP::HeaderMap request_headers;
        ByteBuffer request_body;
        Core::ProxyData proxy_data;
    };

    struct EnsureConnection {
        URL::URL url;
        CacheLevel cache_level;
    };

    using Work = Variant<StartRequest, EnsureConnection, Empty>;
    void worker_do_work(Work);

private:
    explicit ConnectionFromClient(NonnullOwnPtr<Core::LocalSocket>);

    virtual Messages::RequestServer::ConnectNewClientResponse connect_new_client() override;
    virtual Messages::RequestServer::IsSupportedProtocolResponse is_supported_protocol(ByteString const&) override;
    virtual void start_request(i32 request_id, ByteString const&, URL::URL const&, HTTP::HeaderMap const&, ByteBuffer const&, Core::ProxyData const&) override;
    virtual Messages::RequestServer::StopRequestResponse stop_request(i32) override;
    virtual Messages::RequestServer::SetCertificateResponse set_certificate(i32, ByteString const&, ByteString const&) override;
    virtual void ensure_connection(URL::URL const& url, ::RequestServer::CacheLevel const& cache_level) override;

    virtual Messages::RequestServer::WebsocketConnectResponse websocket_connect(URL::URL const&, ByteString const&, Vector<ByteString> const&, Vector<ByteString> const&, HTTP::HeaderMap const&) override;
    virtual Messages::RequestServer::WebsocketReadyStateResponse websocket_ready_state(i32) override;
    virtual Messages::RequestServer::WebsocketSubprotocolInUseResponse websocket_subprotocol_in_use(i32) override;
    virtual void websocket_send(i32, bool, ByteBuffer const&) override;
    virtual void websocket_close(i32, u16, ByteString const&) override;
    virtual Messages::RequestServer::WebsocketSetCertificateResponse websocket_set_certificate(i32, ByteString const&, ByteString const&) override;

    Threading::MutexProtected<HashMap<i32, OwnPtr<Request>>> m_requests;
    HashMap<i32, RefPtr<WebSocket::WebSocket>> m_websockets;

    void enqueue(Work);

    Threading::Mutex m_ipc_mutex;
};

}
