/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Endian.h>
#include <AK/Random.h>
#include <AK/Utf8View.h>
#include <LibCore/Timer.h>
#include <LibCrypto/Hash/HashManager.h>
#include <LibWebSocket/WebSocket.h>

namespace WebSocket {

static constexpr int s_closing_handshake_timeout_ms = 30'000;

// The longest message we accept — in a single frame, or in fragments. Gecko/Blink cap a frame's length at the same
// INT32_MAX, and fail a longer frame with 1009: Gecko mMaxMessageSize (checked in WebSocketChannel::ProcessInput()),
// Blink WebSocketFrameParser::DecodeFrameHeader().
static constexpr u64 s_maximum_message_size = NumericLimits<i32>::max();

// Note : The websocket protocol is defined by RFC 6455, found at https://tools.ietf.org/html/rfc6455
// In this file, section numbers will refer to the RFC 6455

NonnullRefPtr<WebSocket> WebSocket::create(ConnectionInfo connection, NonnullRefPtr<WebSocketImpl> impl)
{
    return adopt_ref(*new WebSocket(move(connection), move(impl)));
}

WebSocket::WebSocket(ConnectionInfo connection, NonnullRefPtr<WebSocketImpl> impl)
    : m_connection(move(connection))
    , m_impl(move(impl))
{
}

void WebSocket::start()
{
    VERIFY(m_state == WebSocket::InternalState::NotStarted);
    VERIFY(m_impl);

    m_impl->on_connection_error = [this] {
        if (m_state == InternalState::Closing) {
            // If the connection drops while we are waiting for the server's close frame, check if we actually received
            // one in the last read. If we did, we can consider this a clean close.
            bool was_clean = m_last_close_code != to_underlying(CloseStatusCode::NoStatusReceived);
            set_state(was_clean ? InternalState::Closed : InternalState::Errored);
            if (!was_clean)
                notify_error(Error::ServerClosedSocket);
            notify_close(m_last_close_code, m_last_close_message, was_clean);
            discard_connection();
            return;
        }
        fail_connection(to_underlying(CloseStatusCode::AbnormalClosure), WebSocket::Error::CouldNotEstablishConnection, "Connection error (underlying socket)");
    };
    m_impl->on_connected = [this] {
        if (m_state != WebSocket::InternalState::EstablishingProtocolConnection)
            return;
        if (m_impl->handshake_complete_when_connected()) {
            set_state(WebSocket::InternalState::Open);
            notify_open();
        } else {
            set_state(WebSocket::InternalState::SendingClientHandshake);
            send_client_handshake();
            drain_read();
        }
    };
    m_impl->on_ready_to_read = [this] {
        drain_read();
    };
    set_state(WebSocket::InternalState::EstablishingProtocolConnection);
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
    // Section 3.1: close(code, reason): https://websockets.spec.whatwg.org/#the-websocket-interface
    VERIFY(m_impl);

    switch (m_state) {
    case InternalState::Closed:
    case InternalState::Closing:
        // "If this’s ready state is CLOSING (2) or CLOSED (3)
        // Do nothing."
        break;
    case InternalState::NotStarted:
    case InternalState::EstablishingProtocolConnection:
    case InternalState::SendingClientHandshake:
    case InternalState::WaitingForServerHandshake:
        // "If the WebSocket connection is not yet established [WSP]
        // Fail the WebSocket connection and set this’s ready state to CLOSING (2)."
        set_state(InternalState::Closing);
        fail_connection(to_underlying(CloseStatusCode::AbnormalClosure), WebSocket::Error::CouldNotEstablishConnection, "Closing connection that's not yet established");
        break;
    case InternalState::Open: {
        // "If the WebSocket closing handshake has not yet been started [WSP]
        // Start the WebSocket closing handshake and set this’s ready state to CLOSING (2)."
        auto message_bytes = message.bytes();
        auto close_payload = ByteBuffer::create_uninitialized(message_bytes.size() + 2).release_value_but_fixme_should_propagate_errors(); // FIXME: Handle possible OOM situation.
        // Section 5.5.1:
        // > If there is a body, the first two bytes of the body MUST be a 2-byte unsigned integer (in network byte order)
        // > representing a status code with value /code/ defined in Section 7.4.
        NetworkOrdered<u16> network_ordered_code { code };
        close_payload.overwrite(0, &network_ordered_code, sizeof(network_ordered_code));
        close_payload.overwrite(2, message_bytes.data(), message_bytes.size());
        send_frame(WebSocket::OpCode::ConnectionClose, close_payload, true);
        set_state(InternalState::Closing);
        break;
    }
    default:
        // "Otherwise
        // Set this’s ready state to CLOSING (2)."
        set_state(InternalState::Closing);
        break;
    }
}

void WebSocket::drain_read()
{
    if (m_impl->eof()) {
        // The connection got closed by the server
        set_state(WebSocket::InternalState::Closed);
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
        // NB: The socket tells us just once that it has data to read, however much it has — so keep reading until
        // there's none left. Gecko/Blink read a chunk, parse it, and repeat until the socket would block, the same way:
        // Gecko WebSocketChannel::OnInputStreamReady(), Blink WebSocketChannel::ReadFrames().
        while (m_state == InternalState::Open || m_state == InternalState::Closing) {
            auto result = m_impl->read(65536);
            if (result.is_error()) {
                fail_connection(to_underlying(CloseStatusCode::AbnormalClosure), WebSocket::Error::ServerClosedSocket, {});
                return;
            }
            auto bytes = result.release_value();
            if (bytes.is_empty())
                break;
            m_buffered_data.append(bytes.data(), bytes.size());
            do {
                if (auto maybe_error = read_frame(); maybe_error.is_error())
                    break;
            } while (!m_buffered_data.is_empty());
        }
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
    builder.appendff("Host: {}", url.serialized_host());
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
    for (auto const& header : m_connection.headers()) {
        builder.appendff("{}: {}\r\n", header.name, header.value);
    }

    builder.append("\r\n"sv);

    set_state(WebSocket::InternalState::WaitingForServerHandshake);
    auto success = m_impl->send(builder.string_view().bytes());
    VERIFY(success);
}

void WebSocket::fail_connection(u16 close_status_code, WebSocket::Error error_code, ByteString const& reason)
{
    if (!reason.is_empty())
        dbgln("WebSocket: {}", reason);
    set_state(WebSocket::InternalState::Errored);
    notify_error(error_code);
    notify_close(close_status_code, reason, false);
    discard_connection();
}

// The server handshake message is defined in the third list of section 4.1
void WebSocket::read_server_handshake()
{
    VERIFY(m_impl);
    VERIFY(m_state == WebSocket::InternalState::WaitingForServerHandshake);

    auto fail_opening_handshake = [&](ByteString const& reason, CloseStatusCode close_status_code = CloseStatusCode::AbnormalClosure) {
        fail_connection(to_underlying(close_status_code), WebSocket::Error::ConnectionUpgradeFailed, reason);
    };

    // Read the server handshake
    if (!m_impl->can_read_line())
        return;

    if (!m_has_read_server_handshake_first_line) {
        auto header = m_impl->read_line(PAGE_SIZE).release_value_but_fixme_should_propagate_errors();
        auto parts = header.split(' ');
        if (parts.size() < 2) {
            fail_opening_handshake("Server HTTP Handshake contained HTTP header was malformed");
            return;
        }
        if (parts[0] != "HTTP/1.1") {
            fail_opening_handshake(ByteString::formatted("Server HTTP Handshake contained HTTP header {} which isn't supported", parts[0]));
            return;
        }
        if (parts[1] != "101") {
            // 1. If the status code is not 101, handle as per HTTP procedures.
            // FIXME: This could be a redirect or a 401 authentication request, which we do not handle.
            fail_opening_handshake(ByteString::formatted("Server HTTP Handshake return status {} which isn't supported", parts[1]));
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
                fail_opening_handshake("Server HTTP Handshake didn't contain an |Upgrade| header");
                return;
            }
            if (!m_has_read_server_handshake_connection) {
                // 2. |Connection| should be present
                fail_opening_handshake("Server HTTP Handshake didn't contain a |Connection| header");
                return;
            }
            if (!m_has_read_server_handshake_accept) {
                // 2. |Sec-WebSocket-Accept| should be present
                fail_opening_handshake("Server HTTP Handshake didn't contain a |Sec-WebSocket-Accept| header");
                return;
            }

            set_state(WebSocket::InternalState::Open);
            notify_open();
            return;
        }

        auto parts = line.split(':');
        if (parts.size() < 2) {
            // The header field is not valid
            fail_opening_handshake(ByteString::formatted("Got invalid header line {} in the Server HTTP handshake", line));
            return;
        }

        auto header_name = parts[0];

        if (header_name.equals_ignoring_ascii_case("Upgrade"sv)) {
            // 2. |Upgrade| should be case-insensitive "websocket"
            if (!parts[1].trim_whitespace().equals_ignoring_ascii_case("websocket"sv)) {
                fail_opening_handshake(ByteString::formatted("Server HTTP Handshake Header |Upgrade| should be 'websocket', got '{}'. Failing connection.", parts[1]));
                return;
            }

            m_has_read_server_handshake_upgrade = true;
            continue;
        }

        if (header_name.equals_ignoring_ascii_case("Connection"sv)) {
            // 3. |Connection| should be case-insensitive "Upgrade"
            if (!parts[1].trim_whitespace().equals_ignoring_ascii_case("Upgrade"sv)) {
                fail_opening_handshake(ByteString::formatted("Server HTTP Handshake Header |Connection| should be 'Upgrade', got '{}'. Failing connection.", parts[1]));
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
                fail_opening_handshake(ByteString::formatted("Server HTTP Handshake Header |Sec-Websocket-Accept| should be '{}', got '{}'. Failing connection.", expected_sha1_string, parts[1]));
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
                    fail_opening_handshake(ByteString::formatted("Server HTTP Handshake Header |Sec-WebSocket-Extensions| contains '{}', which is not supported by the client. Failing connection.", trimmed_extension));
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
                fail_opening_handshake(ByteString::formatted("Server HTTP Handshake Header |Sec-WebSocket-Protocol| contains '{}', which is not supported by the client. Failing connection.", server_protocol));
                return;
            }
            m_subprotocol_in_use = server_protocol;
            continue;
        }
    }

    // If needed, we will keep reading the header on the next drain_read call
}

ErrorOr<void> WebSocket::read_frame()
{
    VERIFY(m_impl);
    VERIFY(m_state == WebSocket::InternalState::Open || m_state == WebSocket::InternalState::Closing);

    size_t cursor = 0;
    auto get_buffered_bytes = [&](size_t count) -> ReadonlyBytes {
        // NB: The cursor never passes the end of the buffered data, so this subtraction can't wrap — whereas
        // cursor + count does, for a count near the top of size_t's range.
        if (count > m_buffered_data.size() - cursor)
            return {};
        auto bytes = m_buffered_data.span().slice(cursor, count);
        cursor += count;
        return bytes;
    };

    // NB: Fewer than 2 bytes buffered only means the rest of the header hasn't arrived yet — it's drain_read() that
    // notices the server closing the connection. Gecko/WebKit/Blink wait for the rest of a header the same way: Gecko
    // ProcessInput(), WebKit parseFrame(), Blink DecodeFrameHeader().
    auto head_bytes = get_buffered_bytes(2);
    if (head_bytes.is_null())
        return AK::Error::from_errno(EAGAIN);

    auto op_code_value = head_bytes[0] & 0x0f;
    if ((op_code_value >= 0x3 && op_code_value <= 0x7) || op_code_value >= 0xb) {
        fail_connection(to_underlying(CloseStatusCode::ProtocolError), WebSocket::Error::ServerClosedSocket, "Server sent a reserved opcode");
        return AK::Error::from_errno(EPROTO);
    }

    if (head_bytes[0] & 0x70) {
        fail_connection(to_underlying(CloseStatusCode::ProtocolError), WebSocket::Error::ServerClosedSocket, "Server set a reserved frame bit");
        return AK::Error::from_errno(EPROTO);
    }

    auto op_code = static_cast<WebSocket::OpCode>(op_code_value);
    bool is_final_frame = head_bytes[0] & 0x80;
    bool is_control_frame = head_bytes[0] & 0x08;
    if (head_bytes[1] & 0x80) {
        fail_connection(to_underlying(CloseStatusCode::ProtocolError), WebSocket::Error::ServerClosedSocket, "Server sent a masked frame");
        return AK::Error::from_errno(EPROTO);
    }

    // Parse the payload length.
    size_t payload_length;
    auto payload_length_bits = head_bytes[1] & 0x7f;
    if (payload_length_bits == 127) {
        // A code of 127 means that the next 8 bytes contains the payload length
        auto actual_bytes = get_buffered_bytes(8);
        if (actual_bytes.is_null())
            return AK::Error::from_errno(EAGAIN);
        u64 full_payload_length = (u64)((u64)(actual_bytes[0] & 0xff) << 56)
            | (u64)((u64)(actual_bytes[1] & 0xff) << 48)
            | (u64)((u64)(actual_bytes[2] & 0xff) << 40)
            | (u64)((u64)(actual_bytes[3] & 0xff) << 32)
            | (u64)((u64)(actual_bytes[4] & 0xff) << 24)
            | (u64)((u64)(actual_bytes[5] & 0xff) << 16)
            | (u64)((u64)(actual_bytes[6] & 0xff) << 8)
            | (u64)((u64)(actual_bytes[7] & 0xff) << 0);

        // https://datatracker.ietf.org/doc/html/rfc6455#section-5.2
        // "If 127, the following 8 bytes interpreted as a 64-bit unsigned integer (the most significant bit MUST be 0)
        // are the payload length."
        if (full_payload_length > static_cast<u64>(NumericLimits<i64>::max())) {
            fail_connection(to_underlying(CloseStatusCode::ProtocolError), WebSocket::Error::ServerClosedSocket, "Server sent a frame length with its most significant bit set");
            return AK::Error::from_errno(EPROTO);
        }

        if (full_payload_length > s_maximum_message_size) {
            fail_connection(to_underlying(CloseStatusCode::MessageTooBig), WebSocket::Error::ServerClosedSocket, "Server sent a frame that's too long");
            return AK::Error::from_errno(EMSGSIZE);
        }

        payload_length = (size_t)full_payload_length;
    } else if (payload_length_bits == 126) {
        // A code of 126 means that the next 2 bytes contains the payload length
        auto actual_bytes = get_buffered_bytes(2);
        if (actual_bytes.is_null())
            return AK::Error::from_errno(EAGAIN);
        payload_length = (size_t)((size_t)(actual_bytes[0] & 0xff) << 8)
            | (size_t)((size_t)(actual_bytes[1] & 0xff) << 0);
    } else {
        payload_length = (size_t)payload_length_bits;
    }

    if (is_control_frame && (!is_final_frame || payload_length > 125)) {
        fail_connection(to_underlying(CloseStatusCode::ProtocolError), WebSocket::Error::ServerClosedSocket, "Server sent an invalid control frame");
        return AK::Error::from_errno(EPROTO);
    }

    // A message that arrives in fragments gets the same limit as one that arrives in a single frame — so a server can't
    // make the fragment buffer grow without bound. Gecko/Blink limit the whole message too: Gecko ProcessInput() adds
    // mFragmentAccumulator to a frame's length before checking mMaxMessageSize, and Blink fails a message that's longer
    // than max_message_size_ (WebSocketChannelImpl::ConsumeDataFrame()). In contrast, WebKit sets no limit of its own.
    if (!is_control_frame && m_fragmented_data_buffer.size() + payload_length > s_maximum_message_size) {
        fail_connection(to_underlying(CloseStatusCode::MessageTooBig), WebSocket::Error::ServerClosedSocket, "Server sent a message that's too long");
        return AK::Error::from_errno(EMSGSIZE);
    }

    // Wait until the whole payload has arrived before allocating anything for it — so a frame header on its own can't
    // make us allocate. Gecko/WebKit/Blink don't allocate from a header either: Gecko WebSocketChannel::ProcessInput()
    // and WebKit WebSocketFrame::parseFrame() wait for the whole payload too, and Blink hands out only the bytes that
    // have arrived (WebSocketFrameParser::DecodeFramePayload()).
    auto payload_bytes = get_buffered_bytes(payload_length);
    if (payload_bytes.is_null())
        return AK::Error::from_errno(EAGAIN);
    auto payload = ByteBuffer::copy(payload_bytes).release_value_but_fixme_should_propagate_errors(); // FIXME: Handle possible OOM situation.

    if (cursor == m_buffered_data.size()) {
        m_buffered_data.clear();
    } else {
        Vector<u8> new_buffered_data;
        new_buffered_data.append(m_buffered_data.data() + cursor, m_buffered_data.size() - cursor);
        m_buffered_data = move(new_buffered_data);
    }

    if (op_code == WebSocket::OpCode::ConnectionClose) {
        if (payload.size() > 1) {
            m_last_close_code = (((u16)(payload[0] & 0xff) << 8) | ((u16)(payload[1] & 0xff)));
            auto close_message = ByteString(ReadonlyBytes(payload.offset_pointer(2), payload.size() - 2));
            if (!Utf8View(close_message).validate()) {
                fail_connection(to_underlying(CloseStatusCode::InvalidPayload), WebSocket::Error::ServerClosedSocket, {});
                return AK::Error::from_errno(EPROTO);
            }
            m_last_close_message = move(close_message);
        } else {
            m_last_close_code = 1000;
            m_last_close_message = {};
        }
        close(m_last_close_code, m_last_close_message);
        return {};
    }
    if (op_code == WebSocket::OpCode::Ping) {
        // https://datatracker.ietf.org/doc/html/rfc6455#section-5.5.2
        // "Upon receipt of a Ping frame, an endpoint MUST send a Pong frame in response, unless it already received a
        // Close frame."
        // AD-HOC: We also don't reply once we've sent a Close frame of our own — and neither do WebKit/Blink: WebKit
        // WebSocketTask::sendFrame() (m_didSendClosingHandshake), Blink WebSocketChannel::HandleFrameByState(). In
        // contrast, Gecko WebSocketChannel::ProcessInput() keeps replying til the server's Close frame has arrived.
        if (m_state == WebSocket::InternalState::Open)
            send_frame(WebSocket::OpCode::Pong, payload, true);
        return {};
    }
    if (op_code == WebSocket::OpCode::Pong) {
        // We can safely ignore the pong
        return {};
    }
    if (!is_final_frame) {
        if (op_code != WebSocket::OpCode::Continuation) {
            // First fragmented message
            m_initial_fragment_opcode = op_code;
        }
        // First and next fragmented message
        m_fragmented_data_buffer.append(payload.data(), payload_length);
        return {};
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
        notify_message(Message(move(payload), true));
        return {};
    }
    if (op_code == WebSocket::OpCode::Binary) {
        notify_message(Message(move(payload), false));
        return {};
    }
    dbgln("Websocket: Found unknown opcode {}", (u8)op_code);
    return {};
}

void WebSocket::send_frame(WebSocket::OpCode op_code, ReadonlyBytes payload, bool is_final)
{
    VERIFY(m_impl);
    VERIFY(m_state == WebSocket::InternalState::Open);

    ByteBuffer buf = MUST(ByteBuffer::create_uninitialized(1 + 9 + 4 + payload.size()));
    size_t offset = 0;

    u8 frame_head[1] = { (u8)((is_final ? 0x80 : 0x00) | ((u8)(op_code) & 0xf)) };
    buf.overwrite(offset, frame_head, 1);
    offset += 1;
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
            buf.overwrite(offset, payload_length, 9);
            offset += 9;
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
            buf.overwrite(offset, payload_length, 9);
            offset += 9;
        }
    } else if (payload.size() >= 126) {
        // Send (the 'mask' flag + 126) + the 2-byte payload length
        u8 payload_length[3] = {
            (u8)((has_mask ? 0x80 : 0x00) | 126),
            (u8)((payload.size() >> 8) & 0xff),
            (u8)((payload.size() >> 0) & 0xff),
        };
        buf.overwrite(offset, payload_length, 3);
        offset += 3;
    } else {
        // Send the mask flag + the payload in a single byte
        u8 payload_length[1] = {
            (u8)((has_mask ? 0x80 : 0x00) | (u8)(payload.size() & 0x7f)),
        };
        buf.overwrite(offset, payload_length, 1);
        offset += 1;
    }
    if (has_mask) {
        // Section 10.3 :
        // > Clients MUST choose a new masking key for each frame, using an algorithm
        // > that cannot be predicted by end applications that provide data
        u8 masking_key[4];
        fill_with_random(masking_key);
        buf.overwrite(offset, masking_key, 4);
        offset += 4;
        // Mask the payload
        auto masked_payload = buf.span().slice(offset, payload.size());
        for (size_t i = 0; i < payload.size(); ++i) {
            masked_payload[i] = payload[i] ^ (masking_key[i % 4]);
        }
        offset += payload.size();
    } else if (payload.size() > 0) {
        buf.overwrite(offset, payload.data(), payload.size());
        offset += payload.size();
    }
    m_impl->send(buf.span().slice(0, offset));
}

void WebSocket::discard_connection()
{
    if (m_discard_connection_requested)
        return;
    m_discard_connection_requested = true;

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

void WebSocket::set_state(InternalState state)
{
    if (m_state == state)
        return;
    auto old_ready_state = ready_state();
    m_state = state;

    if (state == InternalState::Closing) {
        if (!m_closing_handshake_timer) {
            m_closing_handshake_timer = Core::Timer::create_single_shot(s_closing_handshake_timeout_ms, [this] {
                if (m_state != InternalState::Closing)
                    return;
                fail_connection(to_underlying(CloseStatusCode::AbnormalClosure), WebSocket::Error::ServerClosedSocket, "Timed out waiting for the peer's close frame");
            });
        } else {
            m_closing_handshake_timer->restart(s_closing_handshake_timeout_ms);
        }
    } else if (m_closing_handshake_timer) {
        m_closing_handshake_timer->stop();
    }

    auto new_ready_state = ready_state();
    if (old_ready_state != new_ready_state) {
        if (on_ready_state_change)
            on_ready_state_change(ready_state());
    }
}

}
