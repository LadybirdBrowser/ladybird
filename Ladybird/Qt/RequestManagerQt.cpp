/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RequestManagerQt.h"
#include "WebSocketImplQt.h"
#include "WebSocketQt.h"
#include <QNetworkCookie>

namespace Ladybird {

RequestManagerQt::RequestManagerQt()
{
    m_qnam = new QNetworkAccessManager(this);

    QObject::connect(m_qnam, &QNetworkAccessManager::finished, this, &RequestManagerQt::reply_finished);
}

void RequestManagerQt::reply_finished(QNetworkReply* reply)
{
    auto request = m_pending.get(reply).value();
    m_pending.remove(reply);
    request->did_finish();
}

RefPtr<Web::ResourceLoaderConnectorRequest> RequestManagerQt::start_request(ByteString const& method, URL::URL const& url, HTTP::HeaderMap const& request_headers, ReadonlyBytes request_body, Core::ProxyData const& proxy)
{
    if (!url.scheme().bytes_as_string_view().is_one_of_ignoring_ascii_case("http"sv, "https"sv)) {
        return nullptr;
    }
    auto request_or_error = Request::create(*m_qnam, method, url, request_headers, request_body, proxy);
    if (request_or_error.is_error()) {
        return nullptr;
    }
    auto request = request_or_error.release_value();
    m_pending.set(&request->reply(), *request);
    return request;
}

ErrorOr<NonnullRefPtr<RequestManagerQt::Request>> RequestManagerQt::Request::create(QNetworkAccessManager& qnam, ByteString const& method, URL::URL const& url, HTTP::HeaderMap const& request_headers, ReadonlyBytes request_body, Core::ProxyData const&)
{
    QNetworkRequest request { QString(url.to_byte_string().characters()) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);

    // NOTE: We disable HTTP2 as it's significantly slower (up to 5x, possibly more)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    QNetworkReply* reply = nullptr;

    for (auto const& it : request_headers.headers()) {
        // FIXME: We currently strip the Accept-Encoding header on outgoing requests from LibWeb
        //        since otherwise it'll ask for compression without Qt being aware of it.
        //        This is very hackish and I'm sure we can do it in concert with Qt somehow.
        if (it.name == "Accept-Encoding")
            continue;
        request.setRawHeader(QByteArray(it.name.characters()), QByteArray(it.value.characters()));
    }

    if (method.equals_ignoring_ascii_case("head"sv)) {
        reply = qnam.head(request);
    } else if (method.equals_ignoring_ascii_case("get"sv)) {
        reply = qnam.get(request);
    } else if (method.equals_ignoring_ascii_case("post"sv)) {
        reply = qnam.post(request, QByteArray((char const*)request_body.data(), request_body.size()));
    } else if (method.equals_ignoring_ascii_case("put"sv)) {
        reply = qnam.put(request, QByteArray((char const*)request_body.data(), request_body.size()));
    } else if (method.equals_ignoring_ascii_case("delete"sv)) {
        reply = qnam.deleteResource(request);
    } else {
        reply = qnam.sendCustomRequest(request, QByteArray(method.characters()), QByteArray((char const*)request_body.data(), request_body.size()));
    }

    return adopt_ref(*new Request(*reply));
}

RefPtr<Web::WebSockets::WebSocketClientSocket> RequestManagerQt::websocket_connect(URL::URL const& url, AK::ByteString const& origin, Vector<AK::ByteString> const& protocols)
{
    WebSocket::ConnectionInfo connection_info(url);
    connection_info.set_origin(origin);
    connection_info.set_protocols(protocols);

    auto impl = adopt_ref(*new WebSocketImplQt);
    auto web_socket = WebSocket::WebSocket::create(move(connection_info), move(impl));
    web_socket->start();
    return WebSocketQt::create(web_socket);
}

RequestManagerQt::Request::Request(QNetworkReply& reply)
    : m_reply(reply)
{
}

RequestManagerQt::Request::~Request() = default;

void RequestManagerQt::Request::set_buffered_request_finished_callback(Protocol::Request::BufferedRequestFinished on_buffered_request_finished)
{
    this->on_buffered_request_finish = move(on_buffered_request_finished);
}

void RequestManagerQt::Request::set_unbuffered_request_callbacks(Protocol::Request::HeadersReceived, Protocol::Request::DataReceived, Protocol::Request::RequestFinished on_request_finished)
{
    dbgln("Unbuffered requests are not yet supported with Qt networking");
    on_request_finished(false, 0);
}

void RequestManagerQt::Request::did_finish()
{
    auto buffer = m_reply.readAll();
    auto http_status_code = m_reply.attribute(QNetworkRequest::Attribute::HttpStatusCodeAttribute).toInt();
    HTTP::HeaderMap response_headers;
    for (auto& it : m_reply.rawHeaderPairs()) {
        auto name = ByteString(it.first.data(), it.first.length());
        auto value = ByteString(it.second.data(), it.second.length());
        if (name.equals_ignoring_ascii_case("set-cookie"sv)) {
            // NOTE: Qt may have bundled multiple Set-Cookie headers into a single one.
            //       We have to extract the full list of cookies via QNetworkReply::header().
            auto set_cookie_list = m_reply.header(QNetworkRequest::SetCookieHeader).value<QList<QNetworkCookie>>();
            for (auto const& cookie : set_cookie_list) {
                response_headers.set(name, cookie.toRawForm().data());
            }
        } else {
            response_headers.set(name, value);
        }
    }
    bool success = http_status_code != 0;
    on_buffered_request_finish(success, buffer.length(), response_headers, http_status_code, ReadonlyBytes { buffer.data(), (size_t)buffer.size() });
}

}
