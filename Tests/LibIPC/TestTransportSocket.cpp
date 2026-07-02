/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Function.h>
#include <AK/ScopeGuard.h>
#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Forward.h>
#include <LibIPC/TransportSocket.h>
#include <LibTest/TestCase.h>
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
        peer.post_message(move(payload), no_attachments);
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
        peer.post_message(move(payload), no_attachments);
        peer.close_after_sending_all_pending_messages();
    }

    // Model a stop path that doesn't run the in-loop read (e.g. SocketClosed from a failed send): The loop reaches its
    // exit with the message still buffered on the socket — so only the loop-exit drain can deliver it.
    IPC::TransportSocket::set_skip_inloop_read_for_test(true);
    ScopeGuard reset_skip = [] { IPC::TransportSocket::set_skip_inloop_read_for_test(false); };

    auto reader_socket = TRY_OR_FAIL(Core::LocalSocket::adopt_fd(fds[0]));
    MUST(reader_socket->set_blocking(false));
    IPC::TransportSocket transport(move(reader_socket));

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

// SendQueue must hand each message's bytes and its file descriptors to the transport together and in order,
// however the kernel socket buffer ends up chunking the writes. This drives peek()/discard() directly, with no
// I/O thread and no real socket, simulating partial writes of many sizes and confirming that every message's
// payload byte and its descriptor come back out, all present and in order.
TEST_CASE(send_queue_drains_all_bytes_and_fds_across_partial_sends)
{
    constexpr u32 message_count = 1000;
    for (size_t write_chunk : { 0u, 1u, 2u, 3u, 5u, 7u, 13u, 40u, 100u, 400u }) {
        auto queue = adopt_ref(*new IPC::SendQueue);
        for (u32 i = 0; i < message_count; ++i) {
            IPC::SocketMessageHeader header { IPC::SocketMessageHeader::Type::Payload, 1u, 1u };
            IPC::MessageDataType payload;
            u8 byte = static_cast<u8>(i & 0xff);
            payload.append(&byte, 1);
            Vector<int> fds;
            fds.append(static_cast<int>(100000 + i));
            queue->enqueue_message(header, move(payload), move(fds));
        }

        // Drain the queue the way the I/O thread does: peek, "write" a chunk of bytes (0 meaning the whole peek),
        // then discard what was written. Descriptors ride the first byte of any write, so a non-empty write
        // carries every descriptor the peek reported.
        Vector<u8> byte_stream;
        Vector<int> fd_stream;
        for (;;) {
            auto peeked = queue->peek(4096);
            if (peeked.bytes.is_empty() && peeked.fds.is_empty())
                break;
            // A peek must never surface descriptors with no byte to carry them, or they would be stranded.
            EXPECT(!peeked.bytes.is_empty());
            size_t bytes_to_write = write_chunk == 0 ? peeked.bytes.size() : min(write_chunk, peeked.bytes.size());
            for (size_t k = 0; k < bytes_to_write; ++k)
                byte_stream.append(peeked.bytes[k]);
            for (size_t k = 0; k < peeked.fds.size(); ++k)
                fd_stream.append(peeked.fds[k]);
            queue->discard(bytes_to_write, peeked.fds.size());
        }

        // Reconstruct the messages from the drained streams and confirm each payload byte and descriptor, in order.
        size_t index = 0;
        size_t fd_index = 0;
        u32 reconstructed = 0;
        while (index + sizeof(IPC::SocketMessageHeader) <= byte_stream.size()) {
            IPC::SocketMessageHeader header;
            memcpy(&header, byte_stream.data() + index, sizeof(header));
            index += sizeof(header);
            EXPECT_EQ(header.fd_count, 1u);
            if (index + header.payload_size > byte_stream.size() || fd_index + header.fd_count > fd_stream.size()) {
                EXPECT(false);
                break;
            }
            EXPECT_EQ(byte_stream[index], static_cast<u8>(reconstructed & 0xff));
            EXPECT_EQ(fd_stream[fd_index], static_cast<int>(100000 + reconstructed));
            index += header.payload_size;
            fd_index += header.fd_count;
            ++reconstructed;
        }
        EXPECT_EQ(reconstructed, message_count);
        EXPECT_EQ(fd_index, fd_stream.size());
    }
}
