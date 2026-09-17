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
    virtual bool send(ReadonlyBytes) override
    {
        m_sent_frame = true;
        return true;
    }
    virtual bool eof() override { return false; }
    virtual void discard_connection() override { }
    virtual bool handshake_complete_when_connected() const override { return true; }

    bool sent_frame() const { return m_sent_frame; }

    void receive(ByteBuffer data)
    {
        m_pending_data = move(data);
        on_ready_to_read();
    }

private:
    ByteBuffer m_pending_data;
    bool m_sent_frame { false };
};

struct FrameResult {
    bool sent_frame { false };
    bool reported_error { false };
    u16 close_code { 0 };
    bool closed { false };
};

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

    result.sent_frame = implementation->sent_frame();
    result.closed = websocket->ready_state() == WebSocket::ReadyState::Closed;
    return result;
}

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
