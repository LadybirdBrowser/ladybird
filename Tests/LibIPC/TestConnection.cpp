/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Queue.h>
#include <AK/RefPtr.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/Connection.h>
#include <LibIPC/Message.h>
#include <LibIPC/Stub.h>
#include <LibIPC/TransportSocket.h>
#include <LibTest/TestCase.h>

namespace {

constexpr u32 TEST_MAGIC = 0xCAFEF00D;
constexpr int TARGET_MESSAGE_ID = 1;
constexpr int OTHER_MESSAGE_ID = 2;

class TestMessage final : public IPC::Message {
public:
    explicit TestMessage(int id)
        : m_id(id)
    {
    }

    u32 endpoint_magic() const override { return TEST_MAGIC; }
    int message_id() const override { return m_id; }
    StringView message_name() const override { return "TestMessage"sv; }

    ErrorOr<IPC::MessageBuffer> encode() const override
    {
        return IPC::MessageBuffer {};
    }

private:
    int m_id { 0 };
};

class CountingStub final : public IPC::Stub {
public:
    u32 magic() const override { return TEST_MAGIC; }
    ByteString name() const override { return "CountingStub"; }

    ErrorOr<OwnPtr<IPC::MessageBuffer>> handle(NonnullOwnPtr<IPC::Message>) override
    {
        ++m_handle_count;
        return OwnPtr<IPC::MessageBuffer> {};
    }

    size_t handle_count() const { return m_handle_count; }

private:
    size_t m_handle_count { 0 };
};

class TestConnection final : public IPC::ConnectionBase {
    C_OBJECT(TestConnection);

public:
    void inject_unprocessed_message(NonnullOwnPtr<IPC::Message> message)
    {
        m_unprocessed_messages.append(move(message));
    }

    OwnPtr<IPC::Message> call_wait_for_specific_endpoint_message_impl(u32 endpoint_magic, int message_id)
    {
        return wait_for_specific_endpoint_message_impl(endpoint_magic, message_id);
    }

protected:
    OwnPtr<IPC::Message> try_parse_message(ReadonlyBytes, Queue<IPC::Attachment>&) override
    {
        return nullptr;
    }

private:
    TestConnection(IPC::Stub& stub, NonnullOwnPtr<IPC::Transport> transport)
        : IPC::ConnectionBase(stub, move(transport), TEST_MAGIC)
    {
    }
};

}

// Regression test for #9582. wait_for_specific_endpoint_message_impl is reachable from any sync IPC call, including
// from inside a constructor whose members are still being initialized (e.g., PageHost's construction issues a sync
// allocate_compositor_context_id IPC call before ConnectionFromClient::m_page_host has been assigned). If the peer
// disconnects before responding, the failure path must not synchronously dispatch unrelated queued messages. That can
// reenter arbitrary handlers from a context where caller state is only partially built.
TEST_CASE(sync_wait_failure_does_not_dispatch_queued_messages)
{
    Core::EventLoop loop;

    int fds[2] = {};
    MUST(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));

    auto local_socket = TRY_OR_FAIL(Core::LocalSocket::adopt_fd(fds[0]));
    auto peer_socket = TRY_OR_FAIL(Core::LocalSocket::adopt_fd(fds[1]));

    MUST(local_socket->set_blocking(false));
    MUST(peer_socket->set_blocking(false));

    auto transport = make<IPC::TransportSocket>(move(local_socket));

    CountingStub stub;
    auto connection = TestConnection::construct(stub, move(transport));

    connection->inject_unprocessed_message(make<TestMessage>(OTHER_MESSAGE_ID));

    peer_socket->close();

    auto response = connection->call_wait_for_specific_endpoint_message_impl(TEST_MAGIC, TARGET_MESSAGE_ID);

    EXPECT(!response);
    EXPECT_EQ(stub.handle_count(), 0u);
}
