/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Random.h>
#include <LibCrypto/Hash/HashManager.h>
#include <LibWebSocket/Impl/WebSocketImplSerenity.h>
#include <LibWebSocket/WebSocket.h>
#include <unistd.h>

namespace WebSocket {

// Note : The websocket protocol is defined by RFC 6455, found at https://tools.ietf.org/html/rfc6455
// In this file, section numbers will refer to the RFC 6455

NonnullRefPtr<WebSocket> WebSocket::create(ConnectionInfo connection, RefPtr<WebSocketImpl> impl)
{
    return adopt_ref(*new WebSocket(move(connection), move(impl)));
}

WebSocket::WebSocket(ConnectionInfo connection, RefPtr<WebSocketImpl> impl)
    : m_connection(move(connection))
    , m_impl(move(impl))
{
}

void WebSocket::start()
{
    VERIFY(m_state == WebSocket::InternalState::NotStarted);

    if (!m_impl)
        m_impl = adopt_ref(*new WebSocketImplSerenity);

    m_impl->on_connection_error = [this] {
        dbgln("WebSocket: Connection error (underlying socket)");
        fatal_error(WebSocket::Error::CouldNotEstablishConnection);
    };
    m_impl->on_connected = [this] {
        if (m_state != WebSocket::InternalState::EstablishingProtocolConnection)
            return;
        m_state = WebSocket::InternalState::SendingClientHandshake;
        send_client_handshake();
        drain_read();
    };
    m_impl->on_ready_to_read = [this] {
        drain_read();
    };
    m_state = WebSocket::InternalState::EstablishingProtocolConnection;
    m_impl->connect(m_connection);
}

ReadyState WebSocket::ready_state()
{
    switch (m_state) {
    case WebSocket::InternalState::NotStarted:
    case WebSocket::InternalState::EstablishingProtocolConnection:
    case WebSocket::InternalState::SendingClientHandshake:
    case WebSocket::InternalState::WaitingForServerHandshake:
        return ReadyState::Connecting;
    case WebSocket::InternalState::Open:
        return ReadyState::Open;
    case WebSocket::InternalState::Closing:
        return ReadyState::Closing;
    case WebSocket::InternalState::Closed:
    case WebSocket::InternalState::Errored:
        return ReadyState::Closed;
    default:
        VERIFY_NOT_REACHED();
        return ReadyState::Closed;
    }
}

ByteString WebSocket::subprotocol_in_use()
{
    return m_subprotocol_in_use;
}

void WebSocket::send(Message const& message)
{
    // Calling send on a socket that is not opened is not allowed
    VERIFY(m_state == WebSocket::InternalState::Open);
    VERIFY(m_impl);
    if (message.is_text())
        send_frame(WebSocket::OpCode::Text, message.data(), true);
    else
        send_frame(WebSocket::OpCode::Binary, message.data(), true);
}

void WebSocket::close(u16 code, ByteString const& message)
{
    VERIFY(m_impl);

    switch (m_state) {
    case InternalState::NotStarted:
    case InternalState::EstablishingProtocolConnection:
    case InternalState::SendingClientHandshake:
    case InternalState::WaitingForServerHandshake:
        // FIXME: Fail the connection.
        m_state = InternalState::Closing;
        break;
    case InternalState::Open: {
        auto message_bytes = message.bytes();
        auto close_payload = ByteBuffer::create_uninitialized(message_bytes.size() + 2).release_value_but_fixme_should_propagate_errors(); // FIXME: Handle possible OOM situation.
        close_payload.overwrite(0, (u8*)&code, 2);
        close_payload.overwrite(2, message_bytes.data(), message_bytes.size());
        send_frame(WebSocket::OpCode::ConnectionClose, close_payload, true);
        m_state = InternalState::Closing;
        break;
    }
    default:
        break;
    }
}

void WebSocket::drain_read()
{
    if (m_impl->eof()) {
        // The connection got closed by the server
        m_state = WebSocket::InternalState::Closed;
        notify_close(m_last_close_code, m_last_close_message, true);
        discard_connection();
        return;
    }

    switch (m_state) {
    case InternalState::NotStarted:
    case InternalState::EstablishingProtocolConnection:
    case InternalState::SendingClientHandshake: {
        auto initializing_bytes = m_impl->read(1024);
        if (!initializing_bytes.is_error())
            dbgln("drain_read() was called on a websocket that isn't opened yet. Read {} bytes from the socket.", initializing_bytes.value().size());
    } break;
    case InternalState::WaitingForServerHandshake: {
        read_server_handshake();
    } break;
    case InternalState::Open:
    case InternalState::Closing: {
        auto result = m_impl->read(65536);
        if (result.is_error()) {
            fatal_error(WebSocket::Error::ServerClosedSocket);
            return;
        }
        auto bytes = result.release_value();
        m_buffered_data.append(bytes.data(), bytes.size());
        read_frame();
    } break;
    case InternalState::Closed:
    case InternalState::Errored: {
        auto closed_bytes = m_impl->read(1024);
        if (!closed_bytes.is_error())
            dbgln("drain_read() was called on a closed websocket. Read {} bytes from the socket.", closed_bytes.value().size());
    } break;
    default:
        VERIFY_NOT_REACHED();
    }
}

// The client handshake message is defined in the second list of section 4.1
void WebSocket::send_client_handshake()
{
    VERIFY(m_impl);
    VERIFY(m_state == WebSocket::InternalState::SendingClientHandshake);
    StringBuilder builder;

    // 2. and 3. GET /resource name/ HTTP 1.1
    builder.appendff("GET {} HTTP/1.1\r\n", m_connection.resource_name());

    // 4. Host
    auto url = m_connection.url();
    builder.appendff("Host: {}", url.serialized_host().release_value_but_fixme_should_propagate_errors());
    if (!m_connection.is_secure() && url.port_or_default() != 80)
        builder.appendff(":{}", url.port_or_default());
    else if (m_connection.is_secure() && url.port_or_default() != 443)
        builder.appendff(":{}", url.port_or_default());
    builder.append("\r\n"sv);

    // 5. and 6. Connection Upgrade
    builder.append("Upgrade: websocket\r\n"sv);
    builder.append("Connection: Upgrade\r\n"sv);

    // 7. 16-byte nonce encoded as Base64
    u8 nonce_data[16];
    fill_with_random(nonce_data);
    // FIXME: change to TRY() and make method fallible
    m_websocket_key = MUST(encode_base64({ nonce_data, 16 })).to_byte_string();
    builder.appendff("Sec-WebSocket-Key: {}\r\n", m_websocket_key);

    // 8. Origin (optional field)
    if (!m_connection.origin().is_empty()) {
        builder.appendff("Origin: {}\r\n", m_connection.origin());
    }

    // 9. Websocket version
    builder.append("Sec-WebSocket-Version: 13\r\n"sv);

    // 10. Websocket protocol (optional field)
    if (!m_connection.protocols().is_empty()) {
        builder.append("Sec-WebSocket-Protocol: "sv);
        builder.join(',', m_connection.protocols());
        builder.append("\r\n"sv);
    }

    // 11. Websocket extensions (optional field)
    if (!m_connection.extensions().is_empty()) {
        builder.append("Sec-WebSocket-Extensions: "sv);
        builder.join(',', m_connection.extensions());
        builder.append("\r\n"sv);
    }

    // 12. Additional headers
    for (auto& header : m_connection.headers().headers()) {
        builder.appendff("{}: {}\r\n", header.name, header.value);
    }

    builder.append("\r\n"sv);

    m_state = WebSocket::InternalState::WaitingForServerHandshake;
    auto success = m_impl->send(builder.string_view().bytes());
    VERIFY(success);
}

// The server handshake message is defined in the third list of section 4.1
void WebSocket::read_server_handshake()
{
    VERIFY(m_impl);
    VERIFY(m_state == WebSocket::InternalState::WaitingForServerHandshake);
    // Read the server handshake
    if (!m_impl->can_read_line())
        return;

    if (!m_has_read_server_handshake_first_line) {
        auto header = m_impl->read_line(PAGE_SIZE).release_value_but_fixme_should_propagate_errors();
        auto parts = header.split(' ');
        if (parts.size() < 2) {
            dbgln("WebSocket: Server HTTP Handshake contained HTTP header was malformed");
            fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
            discard_connection();
            return;
        }
        if (parts[0] != "HTTP/1.1") {
            dbgln("WebSocket: Server HTTP Handshake contained HTTP header {} which isn't supported", parts[0]);
            fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
            discard_connection();
            return;
        }
        if (parts[1] != "101") {
            // 1. If the status code is not 101, handle as per HTTP procedures.
            // FIXME : This could be a redirect or a 401 authentication request, which we do not handle.
            dbgln("WebSocket: Server HTTP Handshake return status {} which isn't supported", parts[1]);
            fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
            return;
        }
        m_has_read_server_handshake_first_line = true;
    }

    // Read the rest of the reply until we find an empty line
    while (m_impl->can_read_line()) {
        auto line = m_impl->read_line(PAGE_SIZE).release_value_but_fixme_should_propagate_errors();
        if (line.is_whitespace()) {
            // We're done with the HTTP headers.
            // Fail the connection if we're missing any of the following:
            if (!m_has_read_server_handshake_upgrade) {
                // 2. |Upgrade| should be present
                dbgln("WebSocket: Server HTTP Handshake didn't contain an |Upgrade| header");
                fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
                return;
            }
            if (!m_has_read_server_handshake_connection) {
                // 2. |Connection| should be present
                dbgln("WebSocket: Server HTTP Handshake didn't contain a |Connection| header");
                fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
                return;
            }
            if (!m_has_read_server_handshake_accept) {
                // 2. |Sec-WebSocket-Accept| should be present
                dbgln("WebSocket: Server HTTP Handshake didn't contain a |Sec-WebSocket-Accept| header");
                fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
                return;
            }

            m_state = WebSocket::InternalState::Open;
            notify_open();
            return;
        }

        auto parts = line.split(':');
        if (parts.size() < 2) {
            // The header field is not valid
            dbgln("WebSocket: Got invalid header line {} in the Server HTTP handshake", line);
            fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
            return;
        }

        auto header_name = parts[0];

        if (header_name.equals_ignoring_ascii_case("Upgrade"sv)) {
            // 2. |Upgrade| should be case-insensitive "websocket"
            if (!parts[1].trim_whitespace().equals_ignoring_ascii_case("websocket"sv)) {
                dbgln("WebSocket: Server HTTP Handshake Header |Upgrade| should be 'websocket', got '{}'. Failing connection.", parts[1]);
                fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
                return;
            }

            m_has_read_server_handshake_upgrade = true;
            continue;
        }

        if (header_name.equals_ignoring_ascii_case("Connection"sv)) {
            // 3. |Connection| should be case-insensitive "Upgrade"
            if (!parts[1].trim_whitespace().equals_ignoring_ascii_case("Upgrade"sv)) {
                dbgln("WebSocket: Server HTTP Handshake Header |Connection| should be 'Upgrade', got '{}'. Failing connection.", parts[1]);
                return;
            }

            m_has_read_server_handshake_connection = true;
            continue;
        }

        if (header_name.equals_ignoring_ascii_case("Sec-WebSocket-Accept"sv)) {
            // 4. |Sec-WebSocket-Accept| should be base64(SHA1(|Sec-WebSocket-Key| + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
            auto expected_content = ByteString::formatted("{}258EAFA5-E914-47DA-95CA-C5AB0DC85B11", m_websocket_key);

            Crypto::Hash::Manager hash;
            hash.initialize(Crypto::Hash::HashKind::SHA1);
            hash.update(expected_content);
            auto expected_sha1 = hash.digest();
            // FIXME: change to TRY() and make method fallible
            auto expected_sha1_string = MUST(encode_base64({ expected_sha1.immutable_data(), expected_sha1.data_length() }));
            if (!parts[1].trim_whitespace().equals_ignoring_ascii_case(expected_sha1_string)) {
                dbgln("WebSocket: Server HTTP Handshake Header |Sec-Websocket-Accept| should be '{}', got '{}'. Failing connection.", expected_sha1_string, parts[1]);
                fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
                return;
            }

            m_has_read_server_handshake_accept = true;
            continue;
        }

        if (header_name.equals_ignoring_ascii_case("Sec-WebSocket-Extensions"sv)) {
            // 5. |Sec-WebSocket-Extensions| should not contain an extension that doesn't appear in m_connection->extensions()
            auto server_extensions = parts[1].split(',');
            for (auto const& extension : server_extensions) {
                auto trimmed_extension = extension.trim_whitespace();
                bool found_extension = false;
                for (auto const& supported_extension : m_connection.extensions()) {
                    if (trimmed_extension.equals_ignoring_ascii_case(supported_extension)) {
                        found_extension = true;
                    }
                }
                if (!found_extension) {
                    dbgln("WebSocket: Server HTTP Handshake Header |Sec-WebSocket-Extensions| contains '{}', which is not supported by the client. Failing connection.", trimmed_extension);
                    fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
                    return;
                }
            }
            continue;
        }

        if (header_name.equals_ignoring_ascii_case("Sec-WebSocket-Protocol"sv)) {
            // 6. If the response includes a |Sec-WebSocket-Protocol| header field and this header field indicates the use of a subprotocol that was not present in the client's handshake (the server has indicated a subprotocol not requested by the client), the client MUST _Fail the WebSocket Connection_.
            // Additionally, Section 4.2.2 says this is "Either a single value representing the subprotocol the server is ready to use or null."
            auto server_protocol = parts[1].trim_whitespace();
            bool found_protocol = false;
            for (auto const& supported_protocol : m_connection.protocols()) {
                if (server_protocol.equals_ignoring_ascii_case(supported_protocol)) {
                    found_protocol = true;
                }
            }
            if (!found_protocol) {
                dbgln("WebSocket: Server HTTP Handshake Header |Sec-WebSocket-Protocol| contains '{}', which is not supported by the client. Failing connection.", server_protocol);
                fatal_error(WebSocket::Error::ConnectionUpgradeFailed);
                return;
            }
            m_subprotocol_in_use = server_protocol;
            continue;
        }
    }

    // If needed, we will keep reading the header on the next drain_read call
}

void WebSocket::read_frame()
{
    VERIFY(m_impl);
    VERIFY(m_state == WebSocket::InternalState::Open || m_state == WebSocket::InternalState::Closing);

    size_t cursor = 0;
    auto get_buffered_bytes = [&](size_t count) -> ReadonlyBytes {
        if (cursor + count > m_buffered_data.size())
            return {};
        auto bytes = m_buffered_data.span().slice(cursor, count);
        cursor += count;
        return bytes;
    };

    auto head_bytes = get_buffered_bytes(2);
    if (head_bytes.is_null() || head_bytes.is_empty()) {
        // The connection got closed.
        m_state = WebSocket::InternalState::Closed;
        notify_close(m_last_close_code, m_last_close_message, true);
        discard_connection();
        return;
    }

    auto op_code = (WebSocket::OpCode)(head_bytes[0] & 0x0f);
    bool is_final_frame = head_bytes[0] & 0x80;
    bool is_masked = head_bytes[1] & 0x80;

    // Parse the payload length.
    size_t payload_length;
    auto payload_length_bits = head_bytes[1] & 0x7f;
    if (payload_length_bits == 127) {
        // A code of 127 means that the next 8 bytes contains the payload length
        auto actual_bytes = get_buffered_bytes(8);
        if (actual_bytes.is_null())
            return;
        u64 full_payload_length = (u64)((u64)(actual_bytes[0] & 0xff) << 56)
            | (u64)((u64)(actual_bytes[1] & 0xff) << 48)
            | (u64)((u64)(actual_bytes[2] & 0xff) << 40)
            | (u64)((u64)(actual_bytes[3] & 0xff) << 32)
            | (u64)((u64)(actual_bytes[4] & 0xff) << 24)
            | (u64)((u64)(actual_bytes[5] & 0xff) << 16)
            | (u64)((u64)(actual_bytes[6] & 0xff) << 8)
            | (u64)((u64)(actual_bytes[7] & 0xff) << 0);
        VERIFY(full_payload_length <= NumericLimits<size_t>::max());
        payload_length = (size_t)full_payload_length;
    } else if (payload_length_bits == 126) {
        // A code of 126 means that the next 2 bytes contains the payload length
        auto actual_bytes = get_buffered_bytes(2);
        if (actual_bytes.is_null())
            return;
        payload_length = (size_t)((size_t)(actual_bytes[0] & 0xff) << 8)
            | (size_t)((size_t)(actual_bytes[1] & 0xff) << 0);
    } else {
        payload_length = (size_t)payload_length_bits;
    }

    // Parse the mask, if it exists.
    // Note : this is technically non-conformant with Section 5.1 :
    // > A server MUST NOT mask any frames that it sends to the client.
    // > A client MUST close a connection if it detects a masked frame.
    // > (These rules might be relaxed in a future specification.)
    // But because it doesn't cost much, we can support receiving masked frames anyways.
    u8 masking_key[4];
    if (is_masked) {
        auto masking_key_data = get_buffered_bytes(4);
        if (masking_key_data.is_null())
            return;
        masking_key[0] = masking_key_data[0];
        masking_key[1] = masking_key_data[1];
        masking_key[2] = masking_key_data[2];
        masking_key[3] = masking_key_data[3];
    }

    auto payload = ByteBuffer::create_uninitialized(payload_length).release_value_but_fixme_should_propagate_errors(); // FIXME: Handle possible OOM situation.
    u64 read_length = 0;
    while (read_length < payload_length) {
        auto payload_part = get_buffered_bytes(payload_length - read_length);
        if (payload_part.is_null())
            return;
        // We read at most "actual_length - read" bytes, so this is safe to do.
        payload.overwrite(read_length, payload_part.data(), payload_part.size());
        read_length += payload_part.size();
    }

    if (cursor == m_buffered_data.size()) {
        m_buffered_data.clear();
    } else {
        Vector<u8> new_buffered_data;
        new_buffered_data.append(m_buffered_data.data() + cursor, m_buffered_data.size() - cursor);
        m_buffered_data = move(new_buffered_data);
    }

    if (is_masked) {
        // Unmask the payload
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = payload[i] ^ (masking_key[i % 4]);
        }
    }

    if (op_code == WebSocket::OpCode::ConnectionClose) {
        if (payload.size() > 1) {
            m_last_close_code = (((u16)(payload[0] & 0xff) << 8) | ((u16)(payload[1] & 0xff)));
            m_last_close_message = ByteString(ReadonlyBytes(payload.offset_pointer(2), payload.size() - 2));
        }
        m_state = WebSocket::InternalState::Closing;
        return;
    }
    if (op_code == WebSocket::OpCode::Ping) {
        // Immediately send a pong frame as a reply, with the given payload.
        send_frame(WebSocket::OpCode::Pong, payload, true);
        return;
    }
    if (op_code == WebSocket::OpCode::Pong) {
        // We can safely ignore the pong
        return;
    }
    if (!is_final_frame) {
        if (op_code != WebSocket::OpCode::Continuation) {
            // First fragmented message
            m_initial_fragment_opcode = op_code;
        }
        // First and next fragmented message
        m_fragmented_data_buffer.append(payload.data(), payload_length);
        return;
    }
    if (is_final_frame && op_code == WebSocket::OpCode::Continuation) {
        // Last fragmented message
        m_fragmented_data_buffer.append(payload.data(), payload_length);
        op_code = m_initial_fragment_opcode;
        payload.clear();
        payload.append(m_fragmented_data_buffer.data(), m_fragmented_data_buffer.size());
        m_fragmented_data_buffer.clear();
    }
    if (op_code == WebSocket::OpCode::Text) {
        notify_message(Message(payload, true));
        return;
    }
    if (op_code == WebSocket::OpCode::Binary) {
        notify_message(Message(payload, false));
        return;
    }
    dbgln("Websocket: Found unknown opcode {}", (u8)op_code);
}

void WebSocket::send_frame(WebSocket::OpCode op_code, ReadonlyBytes payload, bool is_final)
{
    VERIFY(m_impl);
    VERIFY(m_state == WebSocket::InternalState::Open);
    u8 frame_head[1] = { (u8)((is_final ? 0x80 : 0x00) | ((u8)(op_code) & 0xf)) };
    m_impl->send(ReadonlyBytes(frame_head, 1));
    // Section 5.1 : a client MUST mask all frames that it sends to the server
    bool has_mask = true;
    // FIXME: If the payload has a size > size_t max on a 32-bit platform, we could
    //     technically stream it via non-final packets. However, the size was already
    //     truncated earlier in the call stack when stuffing into a ReadonlyBytes
    if (payload.size() > NumericLimits<u16>::max()) {
        // Send (the 'mask' flag + 127) + the 8-byte payload length
        if constexpr (sizeof(size_t) >= 8) {
            u8 payload_length[9] = {
                (u8)((has_mask ? 0x80 : 0x00) | 127),
                (u8)((payload.size() >> 56) & 0xff),
                (u8)((payload.size() >> 48) & 0xff),
                (u8)((payload.size() >> 40) & 0xff),
                (u8)((payload.size() >> 32) & 0xff),
                (u8)((payload.size() >> 24) & 0xff),
                (u8)((payload.size() >> 16) & 0xff),
                (u8)((payload.size() >> 8) & 0xff),
                (u8)((payload.size() >> 0) & 0xff),
            };
            m_impl->send(ReadonlyBytes(payload_length, 9));
        } else {
            u8 payload_length[9] = {
                (u8)((has_mask ? 0x80 : 0x00) | 127),
                0,
                0,
                0,
                0,
                (u8)((payload.size() >> 24) & 0xff),
                (u8)((payload.size() >> 16) & 0xff),
                (u8)((payload.size() >> 8) & 0xff),
                (u8)((payload.size() >> 0) & 0xff),
            };
            m_impl->send(ReadonlyBytes(payload_length, 9));
        }
    } else if (payload.size() >= 126) {
        // Send (the 'mask' flag + 126) + the 2-byte payload length
        u8 payload_length[3] = {
            (u8)((has_mask ? 0x80 : 0x00) | 126),
            (u8)((payload.size() >> 8) & 0xff),
            (u8)((payload.size() >> 0) & 0xff),
        };
        m_impl->send(ReadonlyBytes(payload_length, 3));
    } else {
        // Send the mask flag + the payload in a single byte
        u8 payload_length[1] = {
            (u8)((has_mask ? 0x80 : 0x00) | (u8)(payload.size() & 0x7f)),
        };
        m_impl->send(ReadonlyBytes(payload_length, 1));
    }
    if (has_mask) {
        // Section 10.3 :
        // > Clients MUST choose a new masking key for each frame, using an algorithm
        // > that cannot be predicted by end applications that provide data
        u8 masking_key[4];
        fill_with_random(masking_key);
        m_impl->send(ReadonlyBytes(masking_key, 4));
        // don't try to send empty payload
        if (payload.size() == 0)
            return;
        // Mask the payload
        auto buffer_result = ByteBuffer::create_uninitialized(payload.size());
        if (!buffer_result.is_error()) {
            auto& masked_payload = buffer_result.value();
            for (size_t i = 0; i < payload.size(); ++i) {
                masked_payload[i] = payload[i] ^ (masking_key[i % 4]);
            }
            m_impl->send(masked_payload);
        }
    } else if (payload.size() > 0) {
        m_impl->send(payload);
    }
}

void WebSocket::fatal_error(WebSocket::Error error)
{
    m_state = WebSocket::InternalState::Errored;
    notify_error(error);
    discard_connection();
}

void WebSocket::discard_connection()
{
    deferred_invoke([this] {
        VERIFY(m_impl);
        m_impl->discard_connection();
        m_impl->on_connection_error = nullptr;
        m_impl->on_connected = nullptr;
        m_impl->on_ready_to_read = nullptr;
        m_impl = nullptr;
    });
}

void WebSocket::notify_open()
{
    if (!on_open)
        return;
    on_open();
}

void WebSocket::notify_close(u16 code, ByteString reason, bool was_clean)
{
    if (!on_close)
        return;
    on_close(code, move(reason), was_clean);
}

void WebSocket::notify_error(WebSocket::Error error)
{
    if (!on_error)
        return;
    on_error(error);
}

void WebSocket::notify_message(Message message)
{
    if (!on_message)
        return;
    on_message(move(message));
}

}
