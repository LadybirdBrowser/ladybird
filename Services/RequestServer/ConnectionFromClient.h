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

private:
    explicit ConnectionFromClient(IPC::Transport);

    virtual Messages::RequestServer::InitTransportResponse init_transport(int peer_pid) override;
    virtual Messages::RequestServer::ConnectNewClientResponse connect_new_client() override;
    virtual Messages::RequestServer::IsSupportedProtocolResponse is_supported_protocol(ByteString) override;
    virtual void set_dns_server(ByteString host_or_address, u16 port, bool use_tls) override;
    virtual void start_request(i32 request_id, ByteString, URL::URL, HTTP::HeaderMap, ByteBuffer, Core::ProxyData) override;
    virtual Messages::RequestServer::StopRequestResponse stop_request(i32) override;
    virtual Messages::RequestServer::SetCertificateResponse set_certificate(i32, ByteString, ByteString) override;
    virtual void ensure_connection(URL::URL url, ::RequestServer::CacheLevel cache_level) override;

    virtual void websocket_connect(i64 websocket_id, URL::URL, ByteString, Vector<ByteString>, Vector<ByteString>, HTTP::HeaderMap) override;
    virtual void websocket_send(i64 websocket_id, bool, ByteBuffer) override;
    virtual void websocket_close(i64 websocket_id, u16, ByteString) override;
    virtual Messages::RequestServer::WebsocketSetCertificateResponse websocket_set_certificate(i64, ByteString, ByteString) override;

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

// FIXME: Find a good home for this
ByteString build_curl_resolve_list(DNS::LookupResult const&, StringView host, u16 port);
constexpr inline uintptr_t websocket_private_tag = 0x1;

}
