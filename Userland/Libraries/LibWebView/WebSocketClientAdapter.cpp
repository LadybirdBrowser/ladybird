/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibRequests/RequestClient.h>
#include <LibRequests/WebSocket.h>
#include <LibWebView/WebSocketClientAdapter.h>

namespace WebView {

RefPtr<WebSocketClientSocketAdapter> WebSocketClientSocketAdapter::create(NonnullRefPtr<Requests::WebSocket> websocket)
{
    return adopt_ref(*new WebSocketClientSocketAdapter(move(websocket)));
}

WebSocketClientSocketAdapter::WebSocketClientSocketAdapter(NonnullRefPtr<Requests::WebSocket> websocket)
    : m_websocket(move(websocket))
{
    m_websocket->on_open = [weak_this = make_weak_ptr()] {
        if (auto strong_this = weak_this.strong_ref())
            if (strong_this->on_open)
                strong_this->on_open();
    };
    m_websocket->on_message = [weak_this = make_weak_ptr()](auto message) {
        if (auto strong_this = weak_this.strong_ref()) {
            if (strong_this->on_message) {
                strong_this->on_message(Web::WebSockets::WebSocketClientSocket::Message {
                    .data = move(message.data),
                    .is_text = message.is_text,
                });
            }
        }
    };
    m_websocket->on_error = [weak_this = make_weak_ptr()](auto error) {
        if (auto strong_this = weak_this.strong_ref()) {
            if (strong_this->on_error) {
                switch (error) {
                case Requests::WebSocket::Error::CouldNotEstablishConnection:
                    strong_this->on_error(Web::WebSockets::WebSocketClientSocket::Error::CouldNotEstablishConnection);
                    return;
                case Requests::WebSocket::Error::ConnectionUpgradeFailed:
                    strong_this->on_error(Web::WebSockets::WebSocketClientSocket::Error::ConnectionUpgradeFailed);
                    return;
                case Requests::WebSocket::Error::ServerClosedSocket:
                    strong_this->on_error(Web::WebSockets::WebSocketClientSocket::Error::ServerClosedSocket);
                    return;
                }
                VERIFY_NOT_REACHED();
            }
        }
    };
    m_websocket->on_close = [weak_this = make_weak_ptr()](u16 code, ByteString reason, bool was_clean) {
        if (auto strong_this = weak_this.strong_ref())
            if (strong_this->on_close)
                strong_this->on_close(code, move(reason), was_clean);
    };
    m_websocket->on_certificate_requested = [weak_this = make_weak_ptr()] {
        if (auto strong_this = weak_this.strong_ref()) {
            if (strong_this->on_certificate_requested) {
                auto certificate_and_key = weak_this->on_certificate_requested();
                return Requests::WebSocket::CertificateAndKey {
                    .certificate = move(certificate_and_key.certificate),
                    .key = move(certificate_and_key.key),
                };
            }
        }
        return Requests::WebSocket::CertificateAndKey {};
    };
}

WebSocketClientSocketAdapter::~WebSocketClientSocketAdapter() = default;

Web::WebSockets::WebSocket::ReadyState WebSocketClientSocketAdapter::ready_state()
{
    switch (m_websocket->ready_state()) {
    case Requests::WebSocket::ReadyState::Connecting:
        return Web::WebSockets::WebSocket::ReadyState::Connecting;
    case Requests::WebSocket::ReadyState::Open:
        return Web::WebSockets::WebSocket::ReadyState::Open;
    case Requests::WebSocket::ReadyState::Closing:
        return Web::WebSockets::WebSocket::ReadyState::Closing;
    case Requests::WebSocket::ReadyState::Closed:
        return Web::WebSockets::WebSocket::ReadyState::Closed;
    }
    VERIFY_NOT_REACHED();
}

ByteString WebSocketClientSocketAdapter::subprotocol_in_use()
{
    return m_websocket->subprotocol_in_use();
}

void WebSocketClientSocketAdapter::send(ByteBuffer binary_or_text_message, bool is_text)
{
    m_websocket->send(binary_or_text_message, is_text);
}

void WebSocketClientSocketAdapter::send(StringView text_message)
{
    m_websocket->send(text_message);
}

void WebSocketClientSocketAdapter::close(u16 code, ByteString reason)
{
    m_websocket->close(code, reason);
}

}
