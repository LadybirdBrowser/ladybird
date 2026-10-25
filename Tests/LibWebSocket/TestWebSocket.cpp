/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebSocket/Impl/WebSocketImpl.h>
#include <LibWebSocket/WebSocket.h>

namespace {

class TestWebSocketImpl final : public WebSocket::WebSocketImpl {
public:
    virtual void connect(WebSocket::ConnectionInfo const&) override { on_connected(); }
    virtual bool can_read_line() override { return false; }
    virtual ErrorOr<ByteString> read_line(size_t) override { return Error::from_errno(ENOTSUP); }
    virtual ErrorOr<ByteBuffer> read(int max_size) override
    {
        auto size = min(m_pending_data.size(), static_cast<size_t>(max_size));
        auto data = TRY(m_pending_data.slice(0, size));
        m_pending_data = TRY(m_pending_data.slice(size, m_pending_data.size() - size));
        return data;
    }
    virtual bool send(ReadonlyBytes frame) override
    {
        if (fail_writes)
            return false;
        m_sent_frames.append(MUST(ByteBuffer::copy(frame)));
        return true;
    }
    virtual bool eof() override { return m_eof; }
    virtual void discard_connection() override { }
    virtual bool handshake_complete_when_connected() const override { return true; }

    void receive(ByteBuffer data)
    {
        m_pending_data = move(data);
        on_ready_to_read();
    }

    void close_connection()
    {
        m_eof = true;
        on_ready_to_read();
    }

    Vector<ByteBuffer> const& sent_frames() const { return m_sent_frames; }
    bool fail_writes { false };

private:
    ByteBuffer m_pending_data;
    Vector<ByteBuffer> m_sent_frames;
    bool m_eof { false };
};

struct FrameResult {
    bool received_text { false };
    bool sent_frame { false };
    bool reported_error { false };
    u16 close_code { 0 };
    bool closed { false };
};

FrameResult run_frame_sequence(u8 first_opcode)
{
    auto implementation = adopt_ref(*new TestWebSocketImpl);
    auto url = URL::Parser::basic_parse("ws://localhost/"sv).release_value();
    auto websocket = WebSocket::WebSocket::create(WebSocket::ConnectionInfo(move(url)), implementation);

    FrameResult result;
    websocket->on_message = [&](auto message) { result.received_text = message.is_text(); };
    websocket->on_error = [&](auto) { result.reported_error = true; };
    websocket->on_close = [&](auto code, auto, auto) { result.close_code = code; };
    websocket->start();
    EXPECT(websocket->ready_state() == WebSocket::ReadyState::Open);

    u8 bytes[] { first_opcode, 0, 0x81, 1, 'A' };
    implementation->receive(MUST(ByteBuffer::copy(bytes)));
    result.closed = websocket->ready_state() == WebSocket::ReadyState::Closed;
    return result;
}

FrameResult receive_ping(size_t payload_size)
{
    auto implementation = adopt_ref(*new TestWebSocketImpl);
    auto url = URL::Parser::basic_parse("ws://localhost/"sv).release_value();
    auto websocket = WebSocket::WebSocket::create(WebSocket::ConnectionInfo(move(url)), implementation);

    FrameResult result;
    websocket->on_error = [&](auto) { result.reported_error = true; };
    websocket->on_close = [&](auto code, auto, auto) { result.close_code = code; };
    websocket->start();
    EXPECT(websocket->ready_state() == WebSocket::ReadyState::Open);

    size_t payload_offset = payload_size > 125 ? 4 : 2;
    auto frame = MUST(ByteBuffer::create_uninitialized(payload_offset + payload_size));
    frame[0] = 0x89;
    if (payload_size > 125) {
        frame[1] = 126;
        frame[2] = payload_size >> 8;
        frame[3] = payload_size;
    } else {
        frame[1] = payload_size;
    }
    for (size_t i = payload_offset; i < frame.size(); ++i)
        frame[i] = 'P';
    implementation->receive(move(frame));

    result.sent_frame = !implementation->sent_frames().is_empty();
    result.closed = websocket->ready_state() == WebSocket::ReadyState::Closed;
    return result;
}

struct ReceiveResult {
    Vector<ByteBuffer> messages;
    bool reported_error { false };
    u16 close_code { 0 };
    bool closed_cleanly { false };
    bool closed { false };
};

enum class CloseConnection {
    No,
    Yes,
};

ReceiveResult receive_chunks(ReadonlySpan<ReadonlyBytes> chunks, CloseConnection close_connection = CloseConnection::No)
{
    auto implementation = adopt_ref(*new TestWebSocketImpl);
    auto url = URL::Parser::basic_parse("ws://localhost/"sv).release_value();
    auto websocket = WebSocket::WebSocket::create(WebSocket::ConnectionInfo(move(url)), implementation);

    ReceiveResult result;
    websocket->on_message = [&](auto message) { result.messages.append(message.data()); };
    websocket->on_error = [&](auto) { result.reported_error = true; };
    websocket->on_close = [&](auto code, auto, auto was_clean) {
        result.close_code = code;
        result.closed_cleanly = was_clean;
    };
    websocket->start();
    EXPECT(websocket->ready_state() == WebSocket::ReadyState::Open);

    for (auto chunk : chunks)
        implementation->receive(MUST(ByteBuffer::copy(chunk)));
    if (close_connection == CloseConnection::Yes)
        implementation->close_connection();
    result.closed = websocket->ready_state() == WebSocket::ReadyState::Closed;
    return result;
}

ReceiveResult receive_frame(ReadonlyBytes frame)
{
    ReadonlyBytes const chunks[] { frame };
    return receive_chunks(chunks);
}

FrameResult receive_masked_frame(bool masked)
{
    auto implementation = adopt_ref(*new TestWebSocketImpl);
    auto url = URL::Parser::basic_parse("ws://localhost/"sv).release_value();
    auto websocket = WebSocket::WebSocket::create(WebSocket::ConnectionInfo(move(url)), implementation);

    FrameResult result;
    websocket->on_message = [&](auto message) { result.received_text = message.is_text(); };
    websocket->on_error = [&](auto) { result.reported_error = true; };
    websocket->on_close = [&](auto code, auto, auto) { result.close_code = code; };
    websocket->start();
    EXPECT(websocket->ready_state() == WebSocket::ReadyState::Open);

    if (masked) {
        u8 frame[] { 0x81, 0x81, 1, 2, 3, 4, static_cast<u8>('A' ^ 1) };
        implementation->receive(MUST(ByteBuffer::copy(frame)));
    } else {
        u8 frame[] { 0x81, 1, 'A' };
        implementation->receive(MUST(ByteBuffer::copy(frame)));
    }
    result.closed = websocket->ready_state() == WebSocket::ReadyState::Closed;
    return result;
}

FrameResult receive_frame_with_reserved_bit(bool has_reserved_bit)
{
    auto implementation = adopt_ref(*new TestWebSocketImpl);
    auto url = URL::Parser::basic_parse("ws://localhost/"sv).release_value();
    auto websocket = WebSocket::WebSocket::create(WebSocket::ConnectionInfo(move(url)), implementation);

    FrameResult result;
    websocket->on_message = [&](auto message) { result.received_text = message.is_text(); };
    websocket->on_error = [&](auto) { result.reported_error = true; };
    websocket->on_close = [&](auto code, auto, auto) { result.close_code = code; };
    websocket->start();
    EXPECT(websocket->ready_state() == WebSocket::ReadyState::Open);

    u8 frame[] { static_cast<u8>(has_reserved_bit ? 0xc1 : 0x81), 1, 'A' };
    implementation->receive(MUST(ByteBuffer::copy(frame)));
    result.closed = websocket->ready_state() == WebSocket::ReadyState::Closed;
    return result;
}

struct CloseResult {
    bool reported_error { false };
    bool reflected_invalid_reason { false };
    u16 close_code { 0 };
};

CloseResult receive_close(bool valid_reason)
{
    auto implementation = adopt_ref(*new TestWebSocketImpl);
    auto url = URL::Parser::basic_parse("ws://localhost/"sv).release_value();
    auto websocket = WebSocket::WebSocket::create(WebSocket::ConnectionInfo(move(url)), implementation);

    CloseResult result;
    websocket->on_error = [&](auto) { result.reported_error = true; };
    websocket->on_close = [&](auto code, auto reason, auto) {
        result.close_code = code;
        result.reflected_invalid_reason = reason.length() == 1 && static_cast<u8>(reason.characters()[0]) == 0xff;
    };
    websocket->start();
    EXPECT(websocket->ready_state() == WebSocket::ReadyState::Open);

    u8 frame[] { 0x88, 3, 0x03, 0xe8, static_cast<u8>(valid_reason ? 'A' : 0xff) };
    implementation->receive(MUST(ByteBuffer::copy(frame)));
    if (websocket->ready_state() != WebSocket::ReadyState::Closed)
        implementation->on_connection_error();
    return result;
}

FrameResult receive_frame_with_nonminimal_length(bool nonminimal)
{
    auto implementation = adopt_ref(*new TestWebSocketImpl);
    auto url = URL::Parser::basic_parse("ws://localhost/"sv).release_value();
    auto websocket = WebSocket::WebSocket::create(WebSocket::ConnectionInfo(move(url)), implementation);

    FrameResult result;
    websocket->on_message = [&](auto message) { result.received_text = message.is_text(); };
    websocket->on_error = [&](auto) { result.reported_error = true; };
    websocket->on_close = [&](auto code, auto, auto) { result.close_code = code; };
    websocket->start();
    EXPECT(websocket->ready_state() == WebSocket::ReadyState::Open);

    if (nonminimal) {
        u8 frame[] { 0x81, 127, 0, 0, 0, 0, 0, 0, 0, 1, 'A' };
        implementation->receive(MUST(ByteBuffer::copy(frame)));
    } else {
        u8 frame[] { 0x81, 1, 'A' };
        implementation->receive(MUST(ByteBuffer::copy(frame)));
    }
    result.closed = websocket->ready_state() == WebSocket::ReadyState::Closed;
    return result;
}

}

TEST_CASE(reserved_opcode_fails_connection)
{
    Core::EventLoop event_loop;

    auto result = run_frame_sequence(0x83);
    EXPECT(!result.received_text);
    EXPECT(result.reported_error);
    EXPECT_EQ(result.close_code, to_underlying(WebSocket::CloseStatusCode::ProtocolError));
    EXPECT(result.closed);

    auto control = run_frame_sequence(0x8a);
    EXPECT(control.received_text);
    EXPECT(!control.reported_error);
    EXPECT(!control.closed);
}

TEST_CASE(frame_length_with_most_significant_bit_set_fails_connection)
{
    Core::EventLoop event_loop;

    // 2^64 - 10 is the smallest length that, added to the 10 header bytes already consumed, wraps around to 0.
    u8 const wrapping_length[] { 0x82, 127, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf6 };
    auto wrapping = receive_frame(wrapping_length);
    EXPECT(wrapping.messages.is_empty());
    EXPECT(wrapping.reported_error);
    EXPECT_EQ(wrapping.close_code, to_underlying(WebSocket::CloseStatusCode::ProtocolError));
    EXPECT(wrapping.closed);

    // 2^63 is the smallest length with the bit set.
    u8 const smallest_length[] { 0x82, 127, 0x80, 0, 0, 0, 0, 0, 0, 0 };
    auto smallest = receive_frame(smallest_length);
    EXPECT(smallest.messages.is_empty());
    EXPECT(smallest.reported_error);
    EXPECT_EQ(smallest.close_code, to_underlying(WebSocket::CloseStatusCode::ProtocolError));
    EXPECT(smallest.closed);
}

TEST_CASE(frame_length_over_maximum_fails_connection)
{
    Core::EventLoop event_loop;

    // 2^63 - 1 is the longest length the protocol allows.
    u8 const longest_length[] { 0x82, 127, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    auto longest = receive_frame(longest_length);
    EXPECT(longest.messages.is_empty());
    EXPECT(longest.reported_error);
    EXPECT_EQ(longest.close_code, to_underlying(WebSocket::CloseStatusCode::MessageTooBig));
    EXPECT(longest.closed);

    // 2^31 is the shortest length over the maximum.
    u8 const shortest_length[] { 0x82, 127, 0, 0, 0, 0, 0x80, 0, 0, 0 };
    auto shortest = receive_frame(shortest_length);
    EXPECT(shortest.messages.is_empty());
    EXPECT(shortest.reported_error);
    EXPECT_EQ(shortest.close_code, to_underlying(WebSocket::CloseStatusCode::MessageTooBig));
    EXPECT(shortest.closed);

    // 2^31 - 1 is the maximum itself, so the connection stays open and waits for the payload.
    u8 const maximum_length[] { 0x82, 127, 0, 0, 0, 0, 0x7f, 0xff, 0xff, 0xff };
    auto control = receive_frame(maximum_length);
    EXPECT(control.messages.is_empty());
    EXPECT(!control.reported_error);
    EXPECT(!control.closed);
}

TEST_CASE(fragmented_message_over_maximum_fails_connection)
{
    Core::EventLoop event_loop;

    // A 1-byte first fragment and then a continuation frame of the maximum length make a message that's 1 byte over.
    u8 const over_maximum[] { 0x02, 1, 0xaa, 0x80, 127, 0, 0, 0, 0, 0x7f, 0xff, 0xff, 0xff };
    auto over = receive_frame(over_maximum);
    EXPECT(over.messages.is_empty());
    EXPECT(over.reported_error);
    EXPECT_EQ(over.close_code, to_underlying(WebSocket::CloseStatusCode::MessageTooBig));
    EXPECT(over.closed);

    // With a continuation frame that's 1 byte shorter, the message is the maximum itself — so the connection stays
    // open and waits for the payload.
    u8 const maximum[] { 0x02, 1, 0xaa, 0x80, 127, 0, 0, 0, 0, 0x7f, 0xff, 0xff, 0xfe };
    auto control = receive_frame(maximum);
    EXPECT(control.messages.is_empty());
    EXPECT(!control.reported_error);
    EXPECT(!control.closed);
}

TEST_CASE(frame_split_across_reads_is_delivered_intact)
{
    Core::EventLoop event_loop;

    // A 300-byte binary frame: the first read ends inside the extended length, and the second inside the payload.
    ByteBuffer frame;
    frame.append(0x82);
    frame.append(126);
    frame.append(300 >> 8);
    frame.append(300 & 0xff);
    for (size_t i = 0; i < 300; ++i)
        frame.append(static_cast<u8>(i));

    ReadonlyBytes const chunks[] { frame.bytes().slice(0, 3), frame.bytes().slice(3, 101), frame.bytes().slice(104) };
    auto result = receive_chunks(chunks);
    EXPECT(!result.reported_error);
    EXPECT(!result.closed);
    EXPECT_EQ(result.messages.size(), 1u);
    if (result.messages.size() == 1)
        EXPECT(result.messages[0].bytes() == frame.bytes().slice(4));
}

TEST_CASE(frame_header_split_after_first_byte_is_delivered_intact)
{
    Core::EventLoop event_loop;

    // The first read ends 1 byte into the frame's header.
    u8 const frame[] { 0x81, 1, 'A' };
    ReadonlyBytes const split_frame[] { ReadonlyBytes { frame }.slice(0, 1), ReadonlyBytes { frame }.slice(1) };
    auto result = receive_chunks(split_frame);
    EXPECT(!result.reported_error);
    EXPECT(!result.closed);
    EXPECT_EQ(result.messages.size(), 1u);

    // The first read holds a whole frame, and 1 byte of the next frame's header.
    u8 const frames[] { 0x81, 1, 'A', 0x81, 1, 'B' };
    ReadonlyBytes const split_frames[] { ReadonlyBytes { frames }.slice(0, 4), ReadonlyBytes { frames }.slice(4) };
    result = receive_chunks(split_frames);
    EXPECT(!result.reported_error);
    EXPECT(!result.closed);
    EXPECT_EQ(result.messages.size(), 2u);
}

TEST_CASE(server_closing_connection_closes_websocket)
{
    Core::EventLoop event_loop;

    auto result = receive_chunks({}, CloseConnection::Yes);
    EXPECT(result.closed);
    EXPECT(result.closed_cleanly);
    EXPECT_EQ(result.close_code, to_underlying(WebSocket::CloseStatusCode::NoStatusReceived));

    // The server closes the connection 1 byte into a frame's header.
    u8 const partial_header[] { 0x81 };
    ReadonlyBytes const chunks[] { ReadonlyBytes { partial_header } };
    result = receive_chunks(chunks, CloseConnection::Yes);
    EXPECT(result.messages.is_empty());
    EXPECT(result.closed);
    EXPECT(result.closed_cleanly);
    EXPECT_EQ(result.close_code, to_underlying(WebSocket::CloseStatusCode::NoStatusReceived));
}

TEST_CASE(everything_the_socket_has_buffered_is_read)
{
    Core::EventLoop event_loop;

    // Four 60 KiB binary frames that the socket reports as ready to read just once.
    size_t const payload_size = 60 * KiB;
    auto payload = MUST(ByteBuffer::create_zeroed(payload_size));
    ByteBuffer frames;
    for (size_t i = 0; i < 4; ++i) {
        frames.append(0x82);
        frames.append(126);
        frames.append(payload_size >> 8);
        frames.append(payload_size & 0xff);
        frames.append(payload);
    }

    auto result = receive_frame(frames);
    EXPECT(!result.reported_error);
    EXPECT(!result.closed);
    EXPECT_EQ(result.messages.size(), 4u);
}

TEST_CASE(ping_is_answered_only_while_open)
{
    Core::EventLoop event_loop;

    auto implementation = adopt_ref(*new TestWebSocketImpl);
    auto url = URL::Parser::basic_parse("ws://localhost/"sv).release_value();
    auto websocket = WebSocket::WebSocket::create(WebSocket::ConnectionInfo(move(url)), implementation);
    websocket->start();
    EXPECT(websocket->ready_state() == WebSocket::ReadyState::Open);

    u8 const ping[] { 0x89, 0 };
    implementation->receive(MUST(ByteBuffer::copy(ping)));
    EXPECT_EQ(implementation->sent_frames().size(), 1u);
    EXPECT_EQ(implementation->sent_frames().last()[0], 0x8a);

    websocket->close(1000, {});
    EXPECT(websocket->ready_state() == WebSocket::ReadyState::Closing);
    EXPECT_EQ(implementation->sent_frames().size(), 2u);
    EXPECT_EQ(implementation->sent_frames().last()[0], 0x88);

    // A ping that arrives before the server's reply to our Close frame.
    implementation->receive(MUST(ByteBuffer::copy(ping)));
    EXPECT(websocket->ready_state() == WebSocket::ReadyState::Closing);
    EXPECT_EQ(implementation->sent_frames().size(), 2u);
}

TEST_CASE(oversized_ping_fails_connection)
{
    Core::EventLoop event_loop;

    auto result = receive_ping(126);
    EXPECT(!result.sent_frame);
    EXPECT(result.reported_error);
    EXPECT_EQ(result.close_code, to_underlying(WebSocket::CloseStatusCode::ProtocolError));
    EXPECT(result.closed);

    auto control = receive_ping(125);
    EXPECT(control.sent_frame);
    EXPECT(!control.reported_error);
    EXPECT(!control.closed);
}

TEST_CASE(masked_server_frame_fails_connection)
{
    Core::EventLoop event_loop;

    auto result = receive_masked_frame(true);
    EXPECT(!result.received_text);
    EXPECT(result.reported_error);
    EXPECT_EQ(result.close_code, to_underlying(WebSocket::CloseStatusCode::ProtocolError));
    EXPECT(result.closed);

    auto control = receive_masked_frame(false);
    EXPECT(control.received_text);
    EXPECT(!control.reported_error);
    EXPECT(!control.closed);
}

TEST_CASE(reserved_frame_bit_fails_connection)
{
    Core::EventLoop event_loop;

    auto result = receive_frame_with_reserved_bit(true);
    EXPECT(!result.received_text);
    EXPECT(result.reported_error);
    EXPECT_EQ(result.close_code, to_underlying(WebSocket::CloseStatusCode::ProtocolError));
    EXPECT(result.closed);

    auto control = receive_frame_with_reserved_bit(false);
    EXPECT(control.received_text);
    EXPECT(!control.reported_error);
    EXPECT(!control.closed);
}

TEST_CASE(invalid_close_reason_fails_connection)
{
    Core::EventLoop event_loop;

    auto result = receive_close(false);
    EXPECT(result.reported_error);
    EXPECT_EQ(result.close_code, to_underlying(WebSocket::CloseStatusCode::InvalidPayload));
    EXPECT(!result.reflected_invalid_reason);

    auto control = receive_close(true);
    EXPECT(!control.reported_error);
    EXPECT_EQ(control.close_code, to_underlying(WebSocket::CloseStatusCode::Normal));
    EXPECT(!control.reflected_invalid_reason);
}

TEST_CASE(nonminimal_frame_length_fails_connection)
{
    Core::EventLoop event_loop;

    auto result = receive_frame_with_nonminimal_length(true);
    EXPECT(!result.received_text);
    EXPECT(result.reported_error);
    EXPECT_EQ(result.close_code, to_underlying(WebSocket::CloseStatusCode::ProtocolError));
    EXPECT(result.closed);

    auto control = receive_frame_with_nonminimal_length(false);
    EXPECT(control.received_text);
    EXPECT(!control.reported_error);
    EXPECT(!control.closed);
}

TEST_CASE(close_reason_with_lone_surrogate_fails_connection)
{
    Core::EventLoop event_loop;

    // ED A0 80 is what U+D800 would come out as — but UTF-8 has no encoding for a surrogate.
    u8 const lone_surrogate[] { 0x88, 5, 0x03, 0xe8, 0xed, 0xa0, 0x80 };
    auto result = receive_frame(lone_surrogate);
    EXPECT(result.reported_error);
    EXPECT_EQ(result.close_code, to_underlying(WebSocket::CloseStatusCode::InvalidPayload));
    EXPECT(result.closed);

    // U+FFFD takes 3 bytes too, and it's valid.
    u8 const replacement_character[] { 0x88, 5, 0x03, 0xe8, 0xef, 0xbf, 0xbd };
    auto control = receive_frame(replacement_character);
    EXPECT(!control.reported_error);
    EXPECT(!control.closed);
}

namespace {

struct ServerConnection {
    NonnullRefPtr<TestWebSocketImpl> transport = adopt_ref(*new TestWebSocketImpl);
    NonnullRefPtr<WebSocket::WebSocket> socket;
    Vector<WebSocket::Message> messages;
    u16 close_code { 0 };
    bool was_clean { false };

    explicit ServerConnection(WebSocket::WebSocket::ServerOptions options = {})
        : socket(WebSocket::WebSocket::create_server(NonnullRefPtr<WebSocket::WebSocketImpl> { transport }, options))
    {
        socket->on_message = [this](auto message) { messages.append(move(message)); };
        socket->on_close = [this](auto code, auto, auto clean) {
            close_code = code;
            was_clean = clean;
        };
        socket->start();
    }

    ~ServerConnection()
    {
        socket->on_message = nullptr;
        socket->on_close = nullptr;
        socket->on_client_handshake = nullptr;
    }

    void handshake(StringView extra_headers = {})
    {
        auto request = ByteString::formatted("GET /echo HTTP/1.1\r\nHost: example.test\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n{}\r\n", extra_headers);
        transport->receive(MUST(ByteBuffer::copy(request.bytes())));
    }

    void receive(u8 opcode, ReadonlyBytes payload, bool final = true, bool masked = true)
    {
        VERIFY(payload.size() < 126);
        auto frame = MUST(ByteBuffer::create_uninitialized(2 + (masked ? 4 : 0) + payload.size()));
        frame[0] = opcode | (final ? 0x80 : 0);
        frame[1] = payload.size() | (masked ? 0x80 : 0);
        Array<u8, 4> key { 0x12, 0x34, 0x56, 0x78 };
        size_t offset = 2;
        if (masked) {
            frame.overwrite(offset, key.data(), key.size());
            offset += key.size();
        }
        for (size_t i = 0; i < payload.size(); ++i)
            frame[offset + i] = payload[i] ^ (masked ? key[i % 4] : 0);
        transport->receive(move(frame));
    }
};

}

TEST_CASE(server_handshake_uses_http_validation_even_when_transport_has_client_shortcut)
{
    Core::EventLoop event_loop;
    ServerConnection server;
    EXPECT(server.socket->ready_state() == WebSocket::ReadyState::Connecting);
    server.handshake("Origin: https://example.test\r\n"sv);
    EXPECT(server.socket->ready_state() == WebSocket::ReadyState::Open);
    EXPECT_EQ(server.transport->sent_frames().size(), 1u);
    auto response = StringView { server.transport->sent_frames()[0].bytes() };
    EXPECT(response.starts_with("HTTP/1.1 101 Switching Protocols\r\n"sv));
    EXPECT(response.contains("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"sv));
}

TEST_CASE(server_handshake_policy_receives_headers_and_resource)
{
    Core::EventLoop event_loop;
    ServerConnection server;
    server.socket->on_client_handshake = [](auto const& request) -> Optional<ByteString> {
        EXPECT_EQ(request.resource_name, "/echo");
        EXPECT_EQ(request.headers->get("Host"sv).value(), "example.test");
        return "403 Forbidden";
    };
    server.handshake();
    EXPECT(server.socket->ready_state() == WebSocket::ReadyState::Closed);
    EXPECT(StringView { server.transport->sent_frames()[0].bytes() }.starts_with("HTTP/1.1 403 Forbidden\r\n"sv));
}

TEST_CASE(server_handshake_and_masked_frame_can_share_a_read)
{
    Core::EventLoop event_loop;
    ServerConnection server;
    auto bytes = MUST(ByteBuffer::copy("GET /echo HTTP/1.1\r\nHost: example.test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"sv.bytes()));
    Array<u8, 7> frame { 0x81, 0x81, 1, 2, 3, 4, 'A' ^ 1 };
    bytes.append(frame);
    server.transport->receive(move(bytes));
    EXPECT_EQ(server.messages.size(), 1u);
    EXPECT_EQ(StringView { server.messages[0].data().bytes() }, "A"sv);
}

TEST_CASE(server_receives_fragments_and_answers_interleaved_ping_without_masking)
{
    Core::EventLoop event_loop;
    ServerConnection server;
    server.handshake();
    server.receive(1, "hel"sv.bytes(), false);
    server.receive(9, "ping"sv.bytes());
    server.receive(0, "lo"sv.bytes());
    EXPECT_EQ(server.messages.size(), 1u);
    EXPECT(server.messages[0].is_text());
    EXPECT_EQ(StringView { server.messages[0].data().bytes() }, "hello"sv);
    auto const& pong = server.transport->sent_frames()[1];
    EXPECT_EQ(pong[0], 0x8a);
    EXPECT_EQ(pong[1], 4);
    EXPECT_EQ(StringView { pong.bytes().slice(2) }, "ping"sv);
    server.socket->send(WebSocket::Message("reply"));
    auto const& reply = server.transport->sent_frames()[2];
    EXPECT_EQ(reply[0], 0x81);
    EXPECT_EQ(reply[1], 5);
    EXPECT_EQ(StringView { reply.bytes().slice(2) }, "reply"sv);
}

TEST_CASE(server_rejects_unmasked_frames_and_unexpected_fragments)
{
    Core::EventLoop event_loop;
    for (auto opcode : { 0, 1 }) {
        ServerConnection server;
        server.handshake();
        server.receive(opcode, "A"sv.bytes(), true, opcode == 0);
        EXPECT_EQ(server.close_code, 1002);
        EXPECT(!server.was_clean);
        auto const& close = server.transport->sent_frames()[1];
        EXPECT_EQ(close[0], 0x88);
        EXPECT_EQ(close[1], 2);
        EXPECT_EQ(close[2], 0x03);
        EXPECT_EQ(close[3], 0xea);
    }
}

TEST_CASE(server_rejects_fragmented_control_and_overlapping_data_messages)
{
    Core::EventLoop event_loop;
    for (auto opcode : { 1, 9 }) {
        ServerConnection server;
        server.handshake();
        server.receive(1, "A"sv.bytes(), false);
        server.receive(opcode, "B"sv.bytes(), false);
        EXPECT_EQ(server.close_code, 1002);
    }
}

TEST_CASE(server_rejects_nonminimal_and_unrepresentable_lengths_before_allocating)
{
    Core::EventLoop event_loop;
    Array<u8, 4> nonminimal { 0x81, 0xfe, 0, 1 };
    Array<u8, 10> high_bit { 0x81, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0 };
    for (ReadonlyBytes frame : { nonminimal.span(), high_bit.span() }) {
        ServerConnection server;
        server.handshake();
        server.transport->receive(MUST(ByteBuffer::copy(frame)));
        EXPECT_EQ(server.close_code, 1002);
    }
}

TEST_CASE(server_bounds_fragmented_messages_and_handshake_bytes)
{
    Core::EventLoop event_loop;
    ServerConnection server({ .maximum_message_size = 4 });
    server.handshake();
    server.receive(2, "abc"sv.bytes(), false);
    server.receive(0, "de"sv.bytes());
    EXPECT_EQ(server.close_code, 1009);

    ServerConnection handshake_server({ .maximum_handshake_size = 16 });
    handshake_server.transport->receive(MUST(ByteBuffer::copy("GET /too-long HTTP/1.1"sv.bytes())));
    EXPECT(handshake_server.socket->ready_state() == WebSocket::ReadyState::Closed);
}

TEST_CASE(server_validates_text_and_close_payloads)
{
    Core::EventLoop event_loop;
    Array<u8, 2> invalid_text { 0xc0, 0x80 };
    Array<u8, 1> one_byte_close { 0x03 };
    Array<u8, 2> reserved_close_code { 0x03, 0xed };
    Array<u8, 4> invalid_close_reason { 0x03, 0xe8, 0xc0, 0x80 };
    for (ReadonlyBytes payload : { one_byte_close.span(), reserved_close_code.span(), invalid_close_reason.span() }) {
        ServerConnection server;
        server.handshake();
        server.receive(8, payload);
        EXPECT_EQ(server.close_code, payload.size() == 4 ? 1007 : 1002);
    }
    ServerConnection text_server;
    text_server.handshake();
    text_server.receive(1, invalid_text);
    EXPECT_EQ(text_server.close_code, 1007);
}

TEST_CASE(server_close_handshake_finishes_and_stops_delivering_messages)
{
    Core::EventLoop event_loop;
    ServerConnection server;
    server.handshake();
    server.socket->close(1000);
    server.receive(1, "ignored"sv.bytes());
    EXPECT(server.messages.is_empty());
    Array<u8, 2> status { 0x03, 0xe8 };
    server.receive(8, status);
    EXPECT(server.socket->ready_state() == WebSocket::ReadyState::Closed);
    EXPECT_EQ(server.close_code, 1000);
    EXPECT(server.was_clean);
    server.socket->close();
}

TEST_CASE(server_empty_and_default_close_never_send_reserved_status)
{
    Core::EventLoop event_loop;
    ServerConnection server;
    server.handshake();
    server.receive(8, {});
    EXPECT_EQ(server.close_code, 1000);
    EXPECT(server.was_clean);
    auto const& reply = server.transport->sent_frames()[1];
    EXPECT_EQ(reply.size(), 4u);
    EXPECT_EQ(reply[2], 0x03);
    EXPECT_EQ(reply[3], 0xe8);

    ServerConnection default_close;
    default_close.handshake();
    default_close.socket->close();
    auto const& frame = default_close.transport->sent_frames()[1];
    EXPECT_EQ(frame[0], 0x88);
    EXPECT_EQ(frame[1], 0);
    EXPECT_EQ(frame.size(), 2u);
}

TEST_CASE(server_close_remains_safe_after_deferred_transport_discard)
{
    Core::EventLoop event_loop;
    ServerConnection server;
    server.handshake();
    server.receive(8, {});
    // NB: The spin below runs the one-shot callback before this flag leaves scope.
    IGNORE_USE_IN_ESCAPING_LAMBDA bool discarded = false;
    Core::deferred_invoke([&] { discarded = true; });
    event_loop.spin_until([&] { return discarded; });
    server.socket->close();
    EXPECT(server.socket->ready_state() == WebSocket::ReadyState::Closed);
}

TEST_CASE(server_stops_parsing_after_malformed_frame_buffered_with_handshake)
{
    Core::EventLoop event_loop;
    ServerConnection server;
    auto bytes = MUST(ByteBuffer::copy("GET /echo HTTP/1.1\r\nHost: example.test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"sv.bytes()));
    // An unmasked text frame followed by enough data to leave another transport chunk available.
    bytes.append(Array<u8, 3> { 0x81, 1, 'A' });
    auto following_data = MUST(ByteBuffer::create_zeroed(65536));
    bytes.append(following_data);
    server.transport->receive(move(bytes));
    EXPECT_EQ(server.close_code, 1002);
    EXPECT(server.messages.is_empty());
}

TEST_CASE(server_transport_failures_do_not_send_abnormal_closure_code)
{
    Core::EventLoop event_loop;
    ServerConnection server;
    server.handshake();
    server.transport->on_connection_error();
    EXPECT_EQ(server.close_code, 1006);
    EXPECT_EQ(server.transport->sent_frames().size(), 1u);

    ServerConnection failed_write;
    failed_write.handshake();
    failed_write.transport->fail_writes = true;
    failed_write.socket->send(WebSocket::Message("cannot send"));
    EXPECT_EQ(failed_write.close_code, 1006);
    EXPECT(!failed_write.was_clean);
    EXPECT(failed_write.socket->ready_state() == WebSocket::ReadyState::Closed);
    EXPECT_EQ(failed_write.transport->sent_frames().size(), 1u);
}

TEST_CASE(server_closing_handshake_timeout_closes_the_transport)
{
    Core::EventLoop event_loop;
    ServerConnection server({ .handshake_timeout = AK::Duration::from_milliseconds(1) });
    server.handshake();
    server.socket->close(1000);
    bool deadline_expired = false;
    auto deadline = Core::Timer::create_single_shot(AK::Duration::from_seconds(1), [&] { deadline_expired = true; });
    deadline->start();
    event_loop.spin_until([&] { return server.close_code != 0 || deadline_expired; });
    EXPECT(!deadline_expired);
    EXPECT_EQ(server.close_code, 1006);
    EXPECT(!server.was_clean);
    EXPECT(server.socket->ready_state() == WebSocket::ReadyState::Closed);
    // Only the initial 1000 close was written. A timeout never sends internal-only 1006.
    EXPECT_EQ(server.transport->sent_frames().size(), 2u);
}

TEST_CASE(server_opening_handshake_timeout_closes_without_sending_a_frame)
{
    Core::EventLoop event_loop;
    ServerConnection server({ .handshake_timeout = AK::Duration::from_milliseconds(1) });
    bool deadline_expired = false;
    auto deadline = Core::Timer::create_single_shot(AK::Duration::from_seconds(1), [&] { deadline_expired = true; });
    deadline->start();
    event_loop.spin_until([&] { return server.close_code != 0 || deadline_expired; });
    EXPECT(!deadline_expired);
    EXPECT_EQ(server.close_code, 1006);
    EXPECT(server.transport->sent_frames().is_empty());
}

TEST_CASE(server_close_reply_write_failure_reports_one_unclean_close)
{
    Core::EventLoop event_loop;
    ServerConnection server;
    server.handshake();
    size_t close_count = 0;
    server.socket->on_close = [&](auto code, auto, auto clean) {
        ++close_count;
        server.close_code = code;
        server.was_clean = clean;
    };
    server.transport->fail_writes = true;
    Array<u8, 2> status { 0x03, 0xe8 };
    server.receive(8, status);
    EXPECT_EQ(close_count, 1u);
    EXPECT_EQ(server.close_code, 1006);
    EXPECT(!server.was_clean);
    EXPECT_EQ(server.transport->sent_frames().size(), 1u);
}

TEST_CASE(client_empty_close_preserves_status_and_masks_its_reply)
{
    Core::EventLoop event_loop;
    auto transport = adopt_ref(*new TestWebSocketImpl);
    auto socket = WebSocket::WebSocket::create(WebSocket::ConnectionInfo(URL::Parser::basic_parse("ws://localhost/"sv).release_value()), transport);
    u16 close_code = 0;
    socket->on_close = [&](auto code, auto, auto) { close_code = code; };
    socket->start();
    Array<u8, 2> close { 0x88, 0 };
    transport->receive(MUST(ByteBuffer::copy(close)));
    transport->close_connection();
    EXPECT_EQ(close_code, 1000);
    auto const& reply = transport->sent_frames()[0];
    EXPECT_EQ(reply[0], 0x88);
    EXPECT_EQ(reply[1], 0x82);
    EXPECT_EQ(reply[6] ^ reply[2], 0x03);
    EXPECT_EQ(reply[7] ^ reply[3], 0xe8);
}
