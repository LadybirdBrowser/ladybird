/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibURL/URL.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/WebSockets/WebSocket.h>

namespace Protocol {
class Request;
class RequestClient;
}

namespace WebView {

class RequestServerRequestAdapter
    : public Web::ResourceLoaderConnectorRequest
    , public Weakable<RequestServerRequestAdapter> {
public:
    static ErrorOr<NonnullRefPtr<RequestServerRequestAdapter>> try_create(NonnullRefPtr<Protocol::Request>);
    virtual ~RequestServerRequestAdapter() override;

    virtual void set_buffered_request_finished_callback(Protocol::Request::BufferedRequestFinished) override;
    virtual void set_unbuffered_request_callbacks(Protocol::Request::HeadersReceived, Protocol::Request::DataReceived, Protocol::Request::RequestFinished) override;
    virtual bool stop() override;

private:
    RequestServerRequestAdapter(NonnullRefPtr<Protocol::Request>);
    NonnullRefPtr<Protocol::Request> m_request;
};

class RequestServerAdapter : public Web::ResourceLoaderConnector {
public:
    explicit RequestServerAdapter(NonnullRefPtr<Protocol::RequestClient> protocol_client);

    static ErrorOr<NonnullRefPtr<RequestServerAdapter>> try_create(NonnullRefPtr<Protocol::RequestClient>);
    static ErrorOr<NonnullRefPtr<RequestServerAdapter>> try_create();
    virtual ~RequestServerAdapter() override;

    virtual void prefetch_dns(URL::URL const& url) override;
    virtual void preconnect(URL::URL const& url) override;

    virtual RefPtr<Web::ResourceLoaderConnectorRequest> start_request(ByteString const& method, URL::URL const&, HTTP::HeaderMap const& request_headers = {}, ReadonlyBytes request_body = {}, Core::ProxyData const& = {}) override;
    virtual RefPtr<Web::WebSockets::WebSocketClientSocket> websocket_connect(const URL::URL&, ByteString const& origin, Vector<ByteString> const& protocols) override;

private:
    RefPtr<Protocol::RequestClient> m_protocol_client;
};

}
