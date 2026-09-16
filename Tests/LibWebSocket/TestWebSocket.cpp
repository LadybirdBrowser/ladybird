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
