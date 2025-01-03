/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibDNS/Resolver.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibWebSocket/WebSocket.h>
#include <RequestServer/Forward.h>
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

    void did_receive_headers(Badge<Request>, Request&);
    void did_finish_request(Badge<Request>, Request&, bool success);
    void did_progress_request(Badge<Request>, Request&);
    void did_request_certificates(Badge<Request>, Request&);

private:
    explicit ConnectionFromClient(IPC::Transport);

    virtual Messages::RequestServer::InitTransportResponse init_transport(int peer_pid) override;
    virtual Messages::RequestServer::ConnectNewClientResponse connect_new_client() override;
    virtual Messages::RequestServer::IsSupportedProtocolResponse is_supported_protocol(ByteString const&) override;
    virtual void set_dns_server(ByteString const& host_or_address, u16 port, bool use_tls) override;
    virtual void start_request(i32 request_id, ByteString const&, URL::URL const&, HTTP::HeaderMap const&, ByteBuffer const&, Core::ProxyData const&) override;
    virtual Messages::RequestServer::StopRequestResponse stop_request(i32) override;
    virtual Messages::RequestServer::SetCertificateResponse set_certificate(i32, ByteString const&, ByteString const&) override;
    virtual void ensure_connection(URL::URL const& url, ::RequestServer::CacheLevel const& cache_level) override;

    virtual void websocket_connect(i64 websocket_id, URL::URL const&, ByteString const&, Vector<ByteString> const&, Vector<ByteString> const&, HTTP::HeaderMap const&) override;
    virtual void websocket_send(i64 websocket_id, bool, ByteBuffer const&) override;
    virtual void websocket_close(i64 websocket_id, u16, ByteString const&) override;
    virtual Messages::RequestServer::WebsocketSetCertificateResponse websocket_set_certificate(i64, ByteString const&, ByteString const&) override;

    HashMap<i32, RefPtr<WebSocket::WebSocket>> m_websockets;

    struct ActiveRequest;
    friend struct ActiveRequest;

    static int on_socket_callback(void*, int sockfd, int what, void* user_data, void*);
    static int on_timeout_callback(void*, long timeout_ms, void* user_data);
    static size_t on_header_received(void* buffer, size_t size, size_t nmemb, void* user_data);
    static size_t on_data_received(void* buffer, size_t size, size_t nmemb, void* user_data);

    HashMap<i32, NonnullOwnPtr<ActiveRequest>> m_active_requests;

    void check_active_requests();
    void* m_curl_multi { nullptr };
    RefPtr<Core::Timer> m_timer;
    HashMap<int, NonnullRefPtr<Core::Notifier>> m_read_notifiers;
    HashMap<int, NonnullRefPtr<Core::Notifier>> m_write_notifiers;
    NonnullRefPtr<Resolver> m_resolver;
};

}
