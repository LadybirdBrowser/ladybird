/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibRequests/RequestClient.h>
#include <LibRequests/WebSocket.h>

namespace Requests {

WebSocket::WebSocket(RequestClient& client, i64 connection_id)
    : m_client(client)
    , m_websocket_id(connection_id)
{
}

WebSocket::ReadyState WebSocket::ready_state()
{
    return m_ready_state;
}

void WebSocket::set_ready_state(ReadyState ready_state)
{
    m_ready_state = ready_state;
}

ByteString WebSocket::subprotocol_in_use()
{
    return m_subprotocol;
}

void WebSocket::set_subprotocol_in_use(ByteString subprotocol)
{
    m_subprotocol = move(subprotocol);
}

void WebSocket::send(ByteBuffer const& binary_or_text_message, bool is_text)
{
    m_client->async_websocket_send(m_websocket_id, is_text, move(binary_or_text_message));
}

void WebSocket::send(StringView text_message)
{
    send(ByteBuffer::copy(text_message.bytes()).release_value_but_fixme_should_propagate_errors(), true);
}

void WebSocket::close(u16 code, ByteString const& reason)
{
    m_client->async_websocket_close(m_websocket_id, code, move(reason));
}

void WebSocket::did_open(Badge<RequestClient>)
{
    if (on_open)
        on_open();
}

void WebSocket::did_receive(Badge<RequestClient>, ByteBuffer data, bool is_text)
{
    if (on_message)
        on_message(WebSocket::Message { move(data), is_text });
}

void WebSocket::did_error(Badge<RequestClient>, i32 error_code)
{
    if (on_error)
        on_error((WebSocket::Error)error_code);
}

void WebSocket::did_close(Badge<RequestClient>, u16 code, ByteString reason, bool was_clean)
{
    if (on_close)
        on_close(code, move(reason), was_clean);
}

void WebSocket::did_request_certificates(Badge<RequestClient>)
{
    if (on_certificate_requested) {
        auto result = on_certificate_requested();
        if (!m_client->websocket_set_certificate(m_websocket_id, result.certificate, result.key))
            dbgln("WebSocket: set_certificate failed");
    }
}

}
