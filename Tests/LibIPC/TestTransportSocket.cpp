/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/Function.h>
#include <AK/ScopeGuard.h>
#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/AutoCloseFileDescriptor.h>
#include <LibIPC/Forward.h>
#include <LibIPC/TransportSocket.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <fcntl.h>

using namespace AK::TimeLiterals;

static void spin_until(Core::EventLoop& loop, Function<bool()> condition, AK::Duration timeout = 2000_ms)
{
    i64 const timeout_ms = timeout.to_milliseconds();
    for (i64 elapsed_ms = 0; elapsed_ms < timeout_ms; elapsed_ms += 5) {
        (void)loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        if (condition())
            return;
        MUST(Core::System::sleep_ms(5));
    }

    FAIL("Timed out waiting for condition");
}

TEST_CASE(receive_barrier_does_not_wait_for_an_incomplete_frame)
{
    int fds[2] {};
    TRY_OR_FAIL(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));
    ScopeGuard close_sender = [&] { MUST(Core::System::close(fds[1])); };
    auto socket = TRY_OR_FAIL(Core::LocalSocket::adopt_fd(fds[0]));
    MUST(socket->set_blocking(false));
    IPC::TransportSocket transport(move(socket));

    IPC::SocketMessageHeader header {
        .type = IPC::SocketMessageHeader::Type::Payload,
        .payload_size = 1,
        .fd_count = 0,
    };
    auto header_bytes = ReadonlyBytes { reinterpret_cast<u8 const*>(&header), sizeof(header) };
    EXPECT_EQ(TRY_OR_FAIL(Core::System::write(fds[1], header_bytes.slice(0, 1))), 1uz);
    transport.wait_until_incoming_is_current();
    transport.wait_until_incoming_is_current();

    EXPECT_EQ(TRY_OR_FAIL(Core::System::write(fds[1], header_bytes.slice(1))), sizeof(header) - 1);
    Array<u8, 1> payload { 'A' };
    EXPECT_EQ(TRY_OR_FAIL(Core::System::write(fds[1], payload)), 1uz);
    transport.wait_until_incoming_is_current();
    size_t received = 0;
    (void)transport.read_as_many_messages_as_possible_without_blocking([&](auto&& message) {
        EXPECT_EQ(message.bytes.bytes()[0], static_cast<u8>('A'));
        ++received;
    });
    EXPECT_EQ(received, 1uz);
}

TEST_CASE(send_queue_does_not_send_message_bytes_without_fds)
{
    auto queue = adopt_ref(*new IPC::SendQueue);

    IPC::MessageDataType first_payload;
    first_payload.append('A');

    IPC::MessageDataType second_payload;
    second_payload.append('B');

    auto open_descriptor = [] {
        return adopt_ref(*new IPC::AutoCloseFileDescriptor(MUST(Core::System::open("/dev/null"sv, O_RDONLY))));
    };

    Vector<NonnullRefPtr<IPC::AutoCloseFileDescriptor>> first_fds;
    first_fds.ensure_capacity(Core::LocalSocket::MAX_TRANSFER_FDS);
    for (size_t i = 0; i < Core::LocalSocket::MAX_TRANSFER_FDS; ++i)
        first_fds.unchecked_append(open_descriptor());

    Vector<NonnullRefPtr<IPC::AutoCloseFileDescriptor>> second_fds;
    second_fds.append(open_descriptor());

    queue->enqueue_message({}, move(first_payload), move(first_fds));
    queue->enqueue_message({}, move(second_payload), move(second_fds));

    auto first_batch = queue->peek(4096);
    EXPECT_EQ(first_batch.bytes.size(), sizeof(IPC::SocketMessageHeader) + 1);
    EXPECT_EQ(first_batch.bytes[sizeof(IPC::SocketMessageHeader)], static_cast<u8>('A'));
    EXPECT_EQ(first_batch.fds.size(), Core::LocalSocket::MAX_TRANSFER_FDS);

    queue->discard(first_batch.bytes.size(), first_batch.fds.size());

    auto second_batch = queue->peek(4096);
    EXPECT_EQ(second_batch.bytes.size(), sizeof(IPC::SocketMessageHeader) + 1);
    EXPECT_EQ(second_batch.bytes[sizeof(IPC::SocketMessageHeader)], static_cast<u8>('B'));
    EXPECT_EQ(second_batch.fds.size(), 1u);
}

struct TransportPair {
    Core::EventLoop loop;
    OwnPtr<IPC::TransportSocket> sender;
    OwnPtr<IPC::TransportSocket> receiver;

    TransportPair()
    {
        int fds[2] = {};
        MUST(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));
        auto sender_socket = MUST(Core::LocalSocket::adopt_fd(fds[0]));
        auto receiver_socket = MUST(Core::LocalSocket::adopt_fd(fds[1]));
        MUST(sender_socket->set_blocking(false));
        MUST(receiver_socket->set_blocking(false));
        sender = make<IPC::TransportSocket>(move(sender_socket));
        receiver = make<IPC::TransportSocket>(move(receiver_socket));
    }

    // Posts a one-byte message carrying a fresh descriptor, a pipe holding the same byte, and returns that
    // descriptor's number in the sender.
    int post_with_descriptor(u8 tag)
    {
        IPC::MessageDataType payload;
        payload.append(tag);
        auto pipe_fds = MUST(Core::System::pipe2(0));
        MUST(Core::System::write(pipe_fds[1], { &tag, 1 }));
        MUST(Core::System::close(pipe_fds[1]));
        Vector<IPC::Attachment> attachments;
        attachments.append(IPC::Attachment::from_fd(pipe_fds[0]));
        MUST(sender->post_message(move(payload), attachments));
        return pipe_fds[0];
    }
};

static bool descriptor_is_open(int fd)
{
    return fcntl(fd, F_GETFD) >= 0;
}

// The kernel holds its own reference to a file once its descriptor is written, so the sender's copy is closed then.
TEST_CASE(a_sent_descriptor_is_closed_once_written)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA TransportPair pair;

    IGNORE_USE_IN_ESCAPING_LAMBDA size_t received = 0;
    pair.receiver->set_up_read_hook([&] {
        (void)pair.receiver->read_as_many_messages_as_possible_without_blocking([&](auto&& message) {
            ++received;
            while (!message.attachments.is_empty())
                MUST(Core::System::close(message.attachments.dequeue().to_fd()));
        });
    });

    IGNORE_USE_IN_ESCAPING_LAMBDA auto fd = pair.post_with_descriptor('A');
    spin_until(pair.loop, [&] {
        return received == 1 && !descriptor_is_open(fd);
    });
    EXPECT(!descriptor_is_open(fd));
}

TEST_CASE(messages_posted_from_two_threads_arrive_with_their_descriptors)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA TransportPair pair;

    IGNORE_USE_IN_ESCAPING_LAMBDA Vector<u8> received_tags;
    IGNORE_USE_IN_ESCAPING_LAMBDA bool every_message_carried_its_own_descriptor = true;
    pair.receiver->set_up_read_hook([&] {
        (void)pair.receiver->read_as_many_messages_as_possible_without_blocking([&](auto&& message) {
            auto tag = message.bytes.bytes()[0];
            received_tags.append(tag);
            if (message.attachments.size() != 1) {
                every_message_carried_its_own_descriptor = false;
                return;
            }
            auto fd = message.attachments.dequeue().to_fd();
            u8 byte_in_descriptor = 0;
            if (MUST(Core::System::read(fd, { &byte_in_descriptor, 1 })) != 1 || byte_in_descriptor != tag)
                every_message_carried_its_own_descriptor = false;
            MUST(Core::System::close(fd));
        });
    });

    auto other_poster = Threading::Thread::construct("Other poster"sv, [&] {
        for (u8 tag = 'a'; tag <= 'j'; ++tag)
            (void)pair.post_with_descriptor(tag);
        return 0;
    });
    other_poster->start();
    for (u8 tag = 'A'; tag <= 'J'; ++tag)
        (void)pair.post_with_descriptor(tag);
    (void)other_poster->join();

    spin_until(pair.loop, [&] {
        return received_tags.size() == 20;
    });
    EXPECT_EQ(received_tags.size(), 20u);
    EXPECT(every_message_carried_its_own_descriptor);
}

TEST_CASE(read_hook_is_notified_on_peer_hangup)
{
    Core::EventLoop loop;

    int fds[2] = {};
    MUST(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));

    auto reader_socket = TRY_OR_FAIL(Core::LocalSocket::adopt_fd(fds[0]));
    auto peer_socket = TRY_OR_FAIL(Core::LocalSocket::adopt_fd(fds[1]));

    MUST(reader_socket->set_blocking(false));
    MUST(peer_socket->set_blocking(false));

    IPC::TransportSocket transport(move(reader_socket));

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> observed_shutdown = false;

    transport.set_up_read_hook([&] {
        auto should_shutdown = transport.read_as_many_messages_as_possible_without_blocking([](auto&&) {
        });
        if (should_shutdown == IPC::TransportSocket::ShouldShutdown::Yes)
            observed_shutdown.store(true, AK::MemoryOrder::memory_order_relaxed);
    });

    peer_socket->close();

    spin_until(loop, [&] {
        return observed_shutdown.load(AK::MemoryOrder::memory_order_relaxed);
    });

    EXPECT(observed_shutdown.load(AK::MemoryOrder::memory_order_relaxed));
}

TEST_CASE(read_hook_is_notified_when_io_thread_exits_on_close)
{
    Core::EventLoop loop;

    int fds[2] = {};
    MUST(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));

    auto reader_socket = TRY_OR_FAIL(Core::LocalSocket::adopt_fd(fds[0]));
    auto peer_socket = TRY_OR_FAIL(Core::LocalSocket::adopt_fd(fds[1]));

    MUST(reader_socket->set_blocking(false));
    MUST(peer_socket->set_blocking(false));

    IPC::TransportSocket transport(move(reader_socket));

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> observed_shutdown = false;

    transport.set_up_read_hook([&] {
        auto should_shutdown = transport.read_as_many_messages_as_possible_without_blocking([](auto&&) {
        });
        if (should_shutdown == IPC::TransportSocket::ShouldShutdown::Yes)
            observed_shutdown.store(true, AK::MemoryOrder::memory_order_relaxed);
    });

    transport.close();

    spin_until(loop, [&] {
        return observed_shutdown.load(AK::MemoryOrder::memory_order_relaxed);
    });

    EXPECT(observed_shutdown.load(AK::MemoryOrder::memory_order_relaxed));
}

// A message that arrives immediately before EOF must be delivered to the consumer — even when the consumer drains in
// the narrow window between the IO thread observing EOF and that message becoming available. Otherwise, the consumer
// (e.g. a MessagePort) can tear itself down on the EOF, and drop the final message. See the EOF/message ordering in
// read_incoming_messages and read_as_many_messages_as_possible_without_blocking.
TEST_CASE(message_arriving_just_before_eof_is_not_dropped_on_shutdown)
{
    Core::EventLoop loop;

    int fds[2] = {};
    MUST(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));

    // Queue one message and hang up the peer before the reading transport (and its IO thread) exists — so the IO
    // thread's first read sees the message bytes and EOF together.
    {
        auto peer_socket = TRY_OR_FAIL(Core::LocalSocket::adopt_fd(fds[1]));
        MUST(peer_socket->set_blocking(false));
        IPC::TransportSocket peer(move(peer_socket));

        auto hello = "hello"sv.bytes();
        IPC::MessageDataType payload;
        payload.append(hello.data(), hello.size());
        Vector<IPC::Attachment> no_attachments;
        MUST(peer.post_message(move(payload), no_attachments));
        peer.close_after_sending_all_pending_messages();
    }

    // Force the IO thread to wake the consumer and pause on EOF before it parses and appends the message it read — so
    // the consumer's first drain falls inside the window that would otherwise drop the message.
    IPC::TransportSocket::set_eof_drain_window_for_test(200);
    ScopeGuard reset_window = [] { IPC::TransportSocket::set_eof_drain_window_for_test(0); };

    auto reader_socket = TRY_OR_FAIL(Core::LocalSocket::adopt_fd(fds[0]));
    MUST(reader_socket->set_blocking(false));
    IPC::TransportSocket transport(move(reader_socket));

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<u32> delivered = 0;
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> observed_shutdown = false;

    transport.set_up_read_hook([&] {
        // Model a consumer that tears itself down once it observes shutdown (as MessagePort does): A message not
        // delivered before shutdown is observed is lost for good.
        if (observed_shutdown.load(AK::MemoryOrder::memory_order_relaxed))
            return;
        auto should_shutdown = transport.read_as_many_messages_as_possible_without_blocking([&](auto&&) {
            delivered.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
        });
        if (should_shutdown == IPC::TransportSocket::ShouldShutdown::Yes)
            observed_shutdown.store(true, AK::MemoryOrder::memory_order_relaxed);
    });

    spin_until(loop, [&] {
        return observed_shutdown.load(AK::MemoryOrder::memory_order_relaxed);
    });

    EXPECT_EQ(delivered.load(AK::MemoryOrder::memory_order_relaxed), 1u);
}

// A message already buffered on the socket when the IO thread stops must still be delivered — even if the loop stops on
// a path that doesn't run its in-loop read. That happens when a send fails because the peer closed (transfer_data
// returns SocketClosed): The loop ends without draining the receive side. The loop-exit drain is the backstop. Without
// it, the message is lost, and a consumer that tears down on EOF (like MessagePort, e.g. a cross-realm transform stream
// receiving a final "error") hangs its peer forever.
TEST_CASE(buffered_message_is_drained_when_io_thread_stops_without_reading_it)
{
    Core::EventLoop loop;

    int fds[2] = {};
    MUST(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));

    // Queue one message, and hang up the peer before the reading transport exists — so the reader's first poll sees the
    // message bytes and EOF together.
    {
        auto peer_socket = TRY_OR_FAIL(Core::LocalSocket::adopt_fd(fds[1]));
        MUST(peer_socket->set_blocking(false));
        IPC::TransportSocket peer(move(peer_socket));

        auto hello = "hello"sv.bytes();
        IPC::MessageDataType payload;
        payload.append(hello.data(), hello.size());
        Vector<IPC::Attachment> no_attachments;
        MUST(peer.post_message(move(payload), no_attachments));
        peer.close_after_sending_all_pending_messages();
    }

    // Model a stop path that doesn't run the in-loop read (e.g. SocketClosed from a failed send): The loop reaches its
    // exit with the message still buffered on the socket — so only the loop-exit drain can deliver it.
    IPC::TransportSocket::set_skip_inloop_read_for_test(true);
    ScopeGuard reset_skip = [] { IPC::TransportSocket::set_skip_inloop_read_for_test(false); };

    auto reader_socket = TRY_OR_FAIL(Core::LocalSocket::adopt_fd(fds[0]));
    MUST(reader_socket->set_blocking(false));
    IPC::TransportSocket transport(move(reader_socket));

    // The receive barrier must include the loop-exit drain, even after the IO thread stops.
    transport.wait_until_incoming_is_current();

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<u32> delivered = 0;
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> observed_shutdown = false;

    transport.set_up_read_hook([&] {
        if (observed_shutdown.load(AK::MemoryOrder::memory_order_relaxed))
            return;
        auto should_shutdown = transport.read_as_many_messages_as_possible_without_blocking([&](auto&&) {
            delivered.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
        });
        if (should_shutdown == IPC::TransportSocket::ShouldShutdown::Yes)
            observed_shutdown.store(true, AK::MemoryOrder::memory_order_relaxed);
    });

    spin_until(loop, [&] {
        return observed_shutdown.load(AK::MemoryOrder::memory_order_relaxed);
    });

    EXPECT_EQ(delivered.load(AK::MemoryOrder::memory_order_relaxed), 1u);
}
