/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/HashMap.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibWebSocket/WebSocket.h>
#include <RequestServer/Forward.h>
#include <RequestServer/RequestClientEndpoint.h>
#include <RequestServer/RequestServerEndpoint.h>

namespace RequestServer {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<RequestClientEndpoint, RequestServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

public:
    ~ConnectionFromClient() override;

    virtual void die() override;

    void request_complete(Badge<Request>, u64 request_id);

private:
    explicit ConnectionFromClient(NonnullOwnPtr<IPC::Transport>);

    virtual Messages::RequestServer::InitTransportResponse init_transport(int peer_pid) override;
    virtual Messages::RequestServer::ConnectNewClientResponse connect_new_client() override;
    virtual Messages::RequestServer::ConnectNewClientsResponse connect_new_clients(size_t count) override;

    virtual Messages::RequestServer::IsSupportedProtocolResponse is_supported_protocol(ByteString) override;
    virtual void set_dns_server(ByteString host_or_address, u16 port, bool use_tls, bool validate_dnssec_locally) override;
    virtual void set_use_system_dns() override;
    virtual void start_request(u64 request_id, ByteString, URL::URL, Vector<HTTP::Header>, ByteBuffer, Core::ProxyData) override;
    virtual Messages::RequestServer::StopRequestResponse stop_request(u64 request_id) override;
    virtual Messages::RequestServer::SetCertificateResponse set_certificate(u64 request_id, ByteString, ByteString) override;
    virtual void ensure_connection(u64 request_id, URL::URL url, ::RequestServer::CacheLevel cache_level) override;

    virtual void estimate_cache_size_accessed_since(u64 cache_size_estimation_id, UnixDateTime since) override;
    virtual void remove_cache_entries_accessed_since(UnixDateTime since) override;

    virtual void websocket_connect(u64 websocket_id, URL::URL, ByteString, Vector<ByteString>, Vector<ByteString>, Vector<HTTP::Header>) override;
    virtual void websocket_send(u64 websocket_id, bool, ByteBuffer) override;
    virtual void websocket_close(u64 websocket_id, u16, ByteString) override;
    virtual Messages::RequestServer::WebsocketSetCertificateResponse websocket_set_certificate(u64, ByteString, ByteString) override;

    static int on_socket_callback(void*, int sockfd, int what, void* user_data, void*);
    static int on_timeout_callback(void*, long timeout_ms, void* user_data);
    void check_active_requests();

    static ErrorOr<IPC::File> create_client_socket();

    void* m_curl_multi { nullptr };

    HashMap<u64, NonnullOwnPtr<Request>> m_active_requests;
    HashMap<u64, RefPtr<WebSocket::WebSocket>> m_websockets;

    RefPtr<Core::Timer> m_timer;
    HashMap<int, NonnullRefPtr<Core::Notifier>> m_read_notifiers;
    HashMap<int, NonnullRefPtr<Core::Notifier>> m_write_notifiers;

    NonnullRefPtr<Resolver> m_resolver;
    ByteString m_alt_svc_cache_path;
};

constexpr inline uintptr_t websocket_private_tag = 0x1;

}
