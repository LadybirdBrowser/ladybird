/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Timer.h>
#include <LibHTTP/Cache/CacheMode.h>
#include <LibHTTP/Cache/DiskCacheSettings.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibHTTP/Forward.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibRequests/RequestTransferLease.h>
#include <LibRequests/WebSocket.h>
#include <LibWebSocket/WebSocket.h>
#include <RequestServer/Forward.h>
#include <RequestServer/IsPrivate.h>
#include <RequestServer/RequestClientEndpoint.h>
#include <RequestServer/RequestServerEndpoint.h>

namespace RequestServer {

// Cap on an AIA response: these carry one DER or PKCS#7 certificate bundle, never anything large.
constexpr inline size_t max_aia_response_size = 64 * KiB;

struct AIAFetch {
    AK_ALLOC_WITH_KMALLOC;

    ByteString url;
    ByteBuffer body;
    Vector<u64> request_ids;
    curl_slist* resolve_list { nullptr };
};

// Diagnostics hook shared with the control connection; only does anything under REQUESTSERVER_WIRE_DEBUG.
void note_event_tick(StringView label);

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<RequestClientEndpoint, RequestServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

    friend class ControlConnectionFromClient;

public:
    using ConnectionMap = HashMap<int, NonnullRefPtr<ConnectionFromClient>>;

    struct RequestTransferLease {
        NonnullRefPtr<ConnectionFromClient> owner;
        u64 request_id { 0 };
    };

    using RequestTransferLeaseMap = HashMap<Requests::RequestTransferLeaseKey, RequestTransferLease>;

    ~ConnectionFromClient() override;

    virtual void die() override;

    bool websocket_retrieved_http_cookie(Badge<ControlConnectionFromClient>, u64 websocket_id, u64 cookie_request_id, String cookie);

    IsPrivate is_private() const { return m_is_private; }

    void start_revalidation_request(Badge<Request>, ByteString method, URL::URL, NonnullRefPtr<HTTP::HeaderList> request_headers, ByteBuffer request_body, HTTP::Cookie::IncludeCredentials);
    void request_complete(Badge<Request>, Request const&);
    void fetch_aia_intermediate(Badge<Request>, ByteString const& url, u64 for_request_id);

private:
    ConnectionFromClient(NonnullOwnPtr<IPC::Transport>, IsPrivate, ConnectionMap&, RequestTransferLeaseMap&, Optional<HTTP::DiskCache&>, ByteString alt_svc_cache_path);

    virtual Messages::RequestServer::InitTransportResponse init_transport(int peer_pid) override;

    virtual Messages::RequestServer::IsSupportedProtocolResponse is_supported_protocol(ByteString) override;
    virtual Messages::RequestServer::GetClientIdResponse get_client_id() override;
    virtual void start_request(u64 request_id, ByteString, URL::URL, Vector<HTTP::Header>, ByteBuffer, HTTP::CacheMode, HTTP::Cookie::IncludeCredentials, bool create_transfer_lease, Optional<u32> address_selection_hint, bool notify_on_cache_miss, i32 originating_process_id, u64 originating_page_id) override;
    virtual void adopt_request(int source_client_id, u64 source_request_id, u64 target_request_id, bool preserve_transfer_lease) override;
    virtual void release_request_transfer_lease(int source_client_id, u64 source_request_id) override;
    virtual Messages::RequestServer::StopRequestResponse stop_request(u64 request_id) override;
    virtual Messages::RequestServer::SetCertificateResponse set_certificate(u64 request_id, ByteString, ByteString) override;
    virtual void ensure_connection(u64 request_id, URL::URL url, ::RequestServer::CacheLevel cache_level) override;

    virtual Messages::RequestServer::StoreCacheAssociatedDataResponse store_cache_associated_data(URL::URL, ByteString method, Vector<HTTP::Header> request_headers, Optional<u64> vary_key, HTTP::CacheEntryAssociatedData, Core::AnonymousBuffer) override;
    virtual Messages::RequestServer::RetrieveCacheAssociatedDataResponse retrieve_cache_associated_data(URL::URL, ByteString method, Vector<HTTP::Header> request_headers, Optional<u64> vary_key, HTTP::CacheEntryAssociatedData) override;

    virtual Messages::RequestServer::CreateSyntheticCacheEntryResponse create_synthetic_cache_entry(URL::URL, ByteString method) override;

    virtual void websocket_connect(u64 websocket_id, URL::URL, ByteString, Vector<ByteString>, Vector<ByteString>, Vector<HTTP::Header>) override;
    virtual void websocket_send(u64 websocket_id, bool, ByteBuffer) override;
    virtual void websocket_send_shared(u64 websocket_id, bool, Core::AnonymousBuffer) override;
    virtual void websocket_close(u64 websocket_id, u16, ByteString) override;
    virtual Messages::RequestServer::WebsocketSetCertificateResponse websocket_set_certificate(u64, ByteString, ByteString) override;

    static int on_socket_callback(void*, int sockfd, int what, void* user_data, void*);
    static int on_timeout_callback(void*, long timeout_ms, void* user_data);
    void check_active_requests();
    void complete_aia_fetch(void* easy_handle, int result_code);
    void fail_websocket(u64 websocket_id, Requests::WebSocket::Error);
    void connect_websocket(u64 websocket_id, URL::URL, ByteString origin, Vector<ByteString> protocols, Vector<ByteString> extensions, Vector<HTTP::Header> request_headers);

    IsPrivate m_is_private { IsPrivate::No };

    ConnectionMap& m_connections;
    RequestTransferLeaseMap& m_request_transfer_leases;
    Optional<HTTP::DiskCache&> m_disk_cache;

    void* m_curl_multi { nullptr };

    HashMap<u64, NonnullOwnPtr<Request>> m_active_requests;
    HashMap<u64, NonnullOwnPtr<Request>> m_active_revalidation_requests;
    void start_aia_fetch(ByteString const& url, ByteString const& fetch_url, ByteString resolve_entry);
    void abandon_aia_lookup(ByteString const& url);

    HashMap<void*, NonnullOwnPtr<AIAFetch>> m_aia_fetches;
    // AIA URLs whose DNS lookup is still outstanding, mapped to the requests waiting on them. Keeps a second
    // request asking for the same URL from starting a duplicate lookup before m_aia_fetches has an entry.
    HashMap<ByteString, Vector<u64>> m_pending_aia_lookups;
    HashTable<u64> m_pending_websockets;

    struct WebSocketCookieRequest {
        u64 cookie_request_id { 0 };
        Function<void(String)> continuation;
    };
    HashMap<u64, WebSocketCookieRequest> m_websocket_cookie_requests;
    HashMap<u64, RefPtr<WebSocket::WebSocket>> m_websockets;

    RefPtr<Core::Timer> m_timer;
    Optional<MonotonicTime> m_curl_timer_due_at;
    HashMap<int, NonnullRefPtr<Core::Notifier>> m_read_notifiers;
    HashMap<int, NonnullRefPtr<Core::Notifier>> m_write_notifiers;

    NonnullRefPtr<Resolver> m_resolver;
    Optional<ByteString> m_alt_svc_cache_path;

    u64 m_next_revalidation_request_id { 0 };

    Optional<MonotonicTime> m_burst_window_started_at;
    u64 m_requests_in_burst_window { 0 };
};

constexpr inline uintptr_t websocket_private_tag = 0x1;
constexpr inline uintptr_t aia_fetch_private_tag = 0x2;

}
