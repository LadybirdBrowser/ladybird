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
    virtual ErrorOr<ByteBuffer> read(int) override { return move(m_pending_data); }
    virtual bool send(ReadonlyBytes) override { return true; }
    virtual bool eof() override { return false; }
    virtual void discard_connection() override { }
    virtual bool handshake_complete_when_connected() const override { return true; }

    void receive(ByteBuffer data)
    {
        m_pending_data = move(data);
        on_ready_to_read();
    }

private:
    ByteBuffer m_pending_data;
};

struct FrameResult {
    bool received_text { false };
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

struct ReceiveResult {
    Vector<ByteBuffer> messages;
    bool reported_error { false };
    u16 close_code { 0 };
    bool closed { false };
};

ReceiveResult receive_chunks(ReadonlySpan<ReadonlyBytes> chunks)
{
    auto implementation = adopt_ref(*new TestWebSocketImpl);
    auto url = URL::Parser::basic_parse("ws://localhost/"sv).release_value();
    auto websocket = WebSocket::WebSocket::create(WebSocket::ConnectionInfo(move(url)), implementation);

    ReceiveResult result;
    websocket->on_message = [&](auto message) { result.messages.append(message.data()); };
    websocket->on_error = [&](auto) { result.reported_error = true; };
    websocket->on_close = [&](auto code, auto, auto) { result.close_code = code; };
    websocket->start();
    EXPECT(websocket->ready_state() == WebSocket::ReadyState::Open);

    for (auto chunk : chunks)
        implementation->receive(MUST(ByteBuffer::copy(chunk)));
    result.closed = websocket->ready_state() == WebSocket::ReadyState::Closed;
    return result;
}

ReceiveResult receive_frame(ReadonlyBytes frame)
{
    ReadonlyBytes const chunks[] { frame };
    return receive_chunks(chunks);
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
