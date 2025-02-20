/*
 * Copyright (c) 2025, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/MemoryStream.h>
#include <LibCore/Forward.h>
#include <LibWebSocket/Impl/WebSocketImpl.h>
#include <curl/curl.h>

namespace RequestServer {

class WebSocketImplCurl final : public WebSocket::WebSocketImpl {
public:
    virtual ~WebSocketImplCurl() override;

    static NonnullRefPtr<WebSocketImplCurl> create(CURLM*);

    virtual void connect(WebSocket::ConnectionInfo const&) override;
    virtual bool can_read_line() override;
    virtual ErrorOr<ByteString> read_line(size_t) override;
    virtual ErrorOr<ByteBuffer> read(int max_size) override;
    virtual bool send(ReadonlyBytes) override;
    virtual bool eof() override;
    virtual void discard_connection() override;

    virtual bool handshake_complete_when_connected() const override { return true; }

    void did_connect();

private:
    explicit WebSocketImplCurl(CURLM*);

    CURLM* m_multi_handle { nullptr };
    CURL* m_easy_handle { nullptr };
    RefPtr<Core::Notifier> m_read_notifier;
    RefPtr<Core::Notifier> m_error_notifier;
    Vector<curl_slist*> m_curl_string_lists;
    AllocatingMemoryStream m_read_buffer;
};

}
