/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibRequests/RequestClient.h>
#include <LibRequests/WebSocket.h>

namespace Requests {

WebSocket::WebSocket(RequestClient& client, i32 connection_id)
    : m_client(client)
    , m_connection_id(connection_id)
{
}

WebSocket::ReadyState WebSocket::ready_state()
{
    return static_cast<WebSocket::ReadyState>(m_client->websocket_ready_state(m_connection_id));
}

ByteString WebSocket::subprotocol_in_use()
{
    return m_client->websocket_subprotocol_in_use(m_connection_id);
}

void WebSocket::send(ByteBuffer binary_or_text_message, bool is_text)
{
    m_client->async_websocket_send(m_connection_id, is_text, move(binary_or_text_message));
}

void WebSocket::send(StringView text_message)
{
    send(ByteBuffer::copy(text_message.bytes()).release_value_but_fixme_should_propagate_errors(), true);
}

void WebSocket::close(u16 code, ByteString reason)
{
    m_client->async_websocket_close(m_connection_id, code, move(reason));
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
        if (!m_client->websocket_set_certificate(m_connection_id, result.certificate, result.key))
            dbgln("WebSocket: set_certificate failed");
    }
}
}
