/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ByteReader.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Connection.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibIPC/Stub.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

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
        IPC::MessageBuffer buffer;
        u32 id = static_cast<u32>(m_id);
        TRY(buffer.append_data(reinterpret_cast<u8 const*>(&id), sizeof(id)));
        return buffer;
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
    OwnPtr<IPC::Message> try_parse_message(ReadonlyBytes bytes, Queue<IPC::Attachment>&) override
    {
        if (bytes.size() != sizeof(u32))
            return nullptr;
        return make<TestMessage>(static_cast<int>(AK::ByteReader::load32(bytes.data())));
    }

private:
    TestConnection(IPC::Stub& stub, NonnullOwnPtr<IPC::Transport> transport)
        : IPC::ConnectionBase(stub, move(transport), TEST_MAGIC)
    {
    }
};

}

TEST_CASE(anonymous_buffer_size_uses_64_bits)
{
    constexpr size_t buffer_size = 4096;
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(buffer_size));

    IPC::MessageBuffer message_buffer;
    IPC::Encoder encoder { message_buffer };
    MUST(encoder.encode(buffer));
    EXPECT_EQ(message_buffer.data().size(), sizeof(bool) + sizeof(u64));

    auto data = message_buffer.take_data();
    FixedMemoryStream stream { data.span() };

    Queue<IPC::Attachment> attachments;
    for (auto& attachment : message_buffer.take_attachments())
        attachments.enqueue(move(attachment));

    IPC::Decoder decoder { stream, attachments };
    auto decoded_buffer = MUST(decoder.decode<Core::AnonymousBuffer>());
    EXPECT_EQ(decoded_buffer.size(), buffer_size);
}

// Regression test for #9582. wait_for_specific_endpoint_message_impl is reachable from any sync IPC call, including
// from inside a constructor whose members are still being initialized (e.g., PageHost's construction issues a sync
// allocate_compositor_context_id IPC call before ConnectionFromClient::m_page_host has been assigned). If the peer
// disconnects before responding, the failure path must not synchronously dispatch unrelated queued messages. That can
// reenter arbitrary handlers from a context where caller state is only partially built.
TEST_CASE(sync_wait_failure_does_not_dispatch_queued_messages)
{
    Core::EventLoop loop;

    auto pair = TRY_OR_FAIL(IPC::Transport::create_paired());

    CountingStub stub;
    auto connection = TestConnection::construct(stub, move(pair.local));

    connection->inject_unprocessed_message(make<TestMessage>(OTHER_MESSAGE_ID));

    // Hang up the peer before waiting for the response.
    {
        auto hung_up_peer = move(pair.remote_handle);
    }

    auto response = connection->call_wait_for_specific_endpoint_message_impl(TEST_MAGIC, TARGET_MESSAGE_ID);

    EXPECT(!response);
    EXPECT_EQ(stub.handle_count(), 0u);

    // Drain the deferred invocations queued by the EOF drain; the thread's event queue outlives this
    // test's scope, and the stub must not be dispatched to after it is destroyed.
    while (loop.pump(Core::EventLoop::WaitMode::PollForEvents) != 0)
        ;
}

TEST_CASE(async_posts_from_many_threads_all_arrive)
{
    Core::EventLoop loop;

    auto pair = TRY_OR_FAIL(IPC::Transport::create_paired());
    auto receiver_transport = TRY_OR_FAIL(pair.remote_handle.create_transport());

    CountingStub sender_stub;
    auto sender = TestConnection::construct(sender_stub, move(pair.local));
    CountingStub receiver_stub;
    auto receiver = TestConnection::construct(receiver_stub, move(receiver_transport));

    constexpr size_t thread_count = 8;
    constexpr size_t messages_per_thread = 250;

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<size_t> failed_post_count { 0 };
    Vector<NonnullRefPtr<Threading::Thread>> threads;
    for (size_t i = 0; i < thread_count; i++) {
        auto thread = Threading::Thread::construct("Sender"sv, [&sender = *sender, &failed_post_count]() -> intptr_t {
            for (size_t j = 0; j < messages_per_thread; j++) {
                if (sender.post_message(TestMessage { TARGET_MESSAGE_ID }).is_error())
                    failed_post_count++;
            }
            return 0;
        });
        thread->start();
        threads.append(move(thread));
    }
    for (auto& thread : threads)
        MUST(thread->join());

    auto deadline = Core::ElapsedTimer::start_new();
    while (receiver_stub.handle_count() < thread_count * messages_per_thread && deadline.elapsed_milliseconds() < 10'000)
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    EXPECT_EQ(failed_post_count.load(), 0u);
    EXPECT_EQ(receiver_stub.handle_count(), thread_count * messages_per_thread);

    while (loop.pump(Core::EventLoop::WaitMode::PollForEvents) != 0)
        ;
}

TEST_CASE(async_posts_racing_close_are_safe)
{
    Core::EventLoop loop;

    // A live receiver keeps the transport's queue draining, so posts only fail once we shut the
    // connection down ourselves.
    auto pair = TRY_OR_FAIL(IPC::Transport::create_paired());
    auto receiver_transport = TRY_OR_FAIL(pair.remote_handle.create_transport());

    CountingStub stub;
    auto sender = TestConnection::construct(stub, move(pair.local));
    CountingStub receiver_stub;
    auto receiver = TestConnection::construct(receiver_stub, move(receiver_transport));

    constexpr size_t thread_count = 4;
    constexpr size_t posts_per_thread = 2000;

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<size_t> successful_post_count { 0 };
    Vector<NonnullRefPtr<Threading::Thread>> threads;
    for (size_t i = 0; i < thread_count; i++) {
        auto thread = Threading::Thread::construct("Sender"sv, [&sender = *sender, &successful_post_count]() -> intptr_t {
            for (size_t j = 0; j < posts_per_thread; j++) {
                if (!sender.post_message(TestMessage { TARGET_MESSAGE_ID }).is_error())
                    successful_post_count++;
            }
            return 0;
        });
        thread->start();
        threads.append(move(thread));
    }

    // Close only once sends are provably in flight, so the shutdown overlaps them.
    auto deadline = Core::ElapsedTimer::start_new();
    while (successful_post_count.load() < 100 && deadline.elapsed_milliseconds() < 10'000)
        ;
    sender->shutdown();

    for (auto& thread : threads)
        MUST(thread->join());

    EXPECT(!sender->is_open());
    EXPECT(sender->post_message(TestMessage { TARGET_MESSAGE_ID }).is_error());

    while (loop.pump(Core::EventLoop::WaitMode::PollForEvents) != 0)
        ;
}
