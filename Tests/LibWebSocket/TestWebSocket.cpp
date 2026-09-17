/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
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
