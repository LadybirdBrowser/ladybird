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

FrameResult receive_server_frame(bool nonminimal)
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

TEST_CASE(nonminimal_frame_length_fails_connection)
{
    Core::EventLoop event_loop;

    auto result = receive_server_frame(true);
    EXPECT(!result.received_text);
    EXPECT(result.reported_error);
    EXPECT_EQ(result.close_code, to_underlying(WebSocket::CloseStatusCode::ProtocolError));
    EXPECT(result.closed);

    auto control = receive_server_frame(false);
    EXPECT(control.received_text);
    EXPECT(!control.reported_error);
    EXPECT(!control.closed);
}
