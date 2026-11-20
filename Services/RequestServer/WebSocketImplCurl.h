/*
 * Copyright (c) 2025, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/MemoryStream.h>
#include <LibCore/Forward.h>
#include <LibHTTP/Proxy.h>
#include <LibRequests/NetworkUsage.h>
#include <LibWebSocket/Impl/WebSocketImpl.h>
#include <RequestServer/IsPrivate.h>

typedef void CURL;
typedef void CURLM;
struct curl_slist;

namespace RequestServer {

class AIACollector;

class WebSocketImplCurl final : public WebSocket::WebSocketImpl {
public:
    virtual ~WebSocketImplCurl() override;

    static NonnullRefPtr<WebSocketImplCurl> create(CURLM*, IsPrivate);

    void set_proxy(Optional<HTTP::Proxy> proxy) { m_proxy = move(proxy); }
    void set_performance_origin(int client_id, u64 page_id) { m_performance_origin = { client_id, page_id, 0, 0 }; }

    virtual void connect(WebSocket::ConnectionInfo const&) override;
    virtual ErrorOr<ByteBuffer> read(int max_size) override;
    virtual bool send(ReadonlyBytes) override;
    virtual bool eof() override;
    virtual void discard_connection() override;

    bool did_connect();

private:
    WebSocketImplCurl(CURLM*, IsPrivate);

    void read_from_socket();
    bool flush_pending_write_buffer();
    void add_network_usage(u64 download_bytes, u64 upload_bytes) const;

    CURLM* m_multi_handle { nullptr };
    Optional<HTTP::Proxy> m_proxy;
    Requests::NetworkUsage m_performance_origin;
    CURL* m_easy_handle { nullptr };
    NonnullRefPtr<AIACollector> m_aia_collector;
    RefPtr<Core::Notifier> m_read_notifier;
    RefPtr<Core::Notifier> m_write_notifier;
    RefPtr<Core::Notifier> m_error_notifier;
    Vector<curl_slist*> m_curl_string_lists;
    AllocatingMemoryStream m_read_buffer;
    ByteBuffer m_pending_write_buffer;
    size_t m_pending_write_buffer_offset { 0 };
};

}
