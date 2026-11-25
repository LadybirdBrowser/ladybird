/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/CharacterTypes.h>
#include <AK/Function.h>
#include <AK/ScopeGuard.h>
#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Forward.h>
#include <LibIPC/Limits.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <fcntl.h>
#include <sys/resource.h>

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

// A message can carry far more descriptors than the default soft limit lets a process hold, so raise it as LibMain
// does for every process.
static void raise_descriptor_limit()
{
    rlimit limit {};
    VERIFY(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    limit.rlim_cur = min<rlim_t>(limit.rlim_max, 65536);
    VERIFY(setrlimit(RLIMIT_NOFILE, &limit) == 0);
}

struct TransportPair {
    Core::EventLoop loop;
    OwnPtr<IPC::Transport> sender;
    OwnPtr<IPC::Transport> receiver;

    TransportPair()
    {
        int fds[2] = {};
        MUST(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));
        auto sender_socket = MUST(Core::LocalSocket::adopt_fd(fds[0]));
        auto receiver_socket = MUST(Core::LocalSocket::adopt_fd(fds[1]));
        MUST(sender_socket->set_blocking(false));
        MUST(receiver_socket->set_blocking(false));
        sender = MUST(IPC::Transport::from_socket(move(sender_socket)));
        receiver = MUST(IPC::Transport::from_socket(move(receiver_socket)));
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

TEST_CASE(a_socket_adopted_with_an_invalid_attachment_is_closed)
{
    int fds[2] {};
    MUST(Core::System::socketpair(AF_LOCAL, SOCK_STREAM | SOCK_NONBLOCK, 0, fds));
    ScopeGuard close_peer = [&] { MUST(Core::System::close(fds[1])); };
    Vector<IPC::Attachment> unprocessed_attachments;
    unprocessed_attachments.append(IPC::Attachment::from_fd(-1));
    EXPECT(IPC::Transport::adopt_fd(fds[0], {}, move(unprocessed_attachments)).is_error());
    char byte = 0;
    EXPECT_EQ(TRY_OR_FAIL(Core::System::read(fds[1], { &byte, 1 })), 0uz);
}

TEST_CASE(an_interrupted_wait_returns_with_nothing_to_read)
{
    TransportPair pair;
    pair.receiver->interrupt_wait_until_readable();
    pair.receiver->wait_until_readable();
}

// A worker's owner aborts its script from the observer, on the IO thread, and that interrupts what the worker waits for.
TEST_CASE(an_incoming_message_observer_can_interrupt_a_wait)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA TransportPair pair;

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> observed { false };
    pair.receiver->set_incoming_message_observer([&](ReadonlyBytes) {
        pair.receiver->interrupt_wait_until_readable();
        observed.store(true);
    });

    IGNORE_USE_IN_ESCAPING_LAMBDA size_t received = 0;
    pair.receiver->set_up_read_hook([&] {
        (void)pair.receiver->read_as_many_messages_as_possible_without_blocking([&](auto&& message) {
            ++received;
            while (!message.attachments.is_empty())
                MUST(Core::System::close(message.attachments.dequeue().to_fd()));
        });
    });

    (void)pair.post_with_descriptor('A');
    spin_until(pair.loop, [&] {
        return observed.load() && received == 1;
    });
    pair.receiver->set_incoming_message_observer({});
}

// A descriptor that another process sends us must not leak into a program that we exec() later.
TEST_CASE(a_received_descriptor_is_close_on_exec)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA TransportPair pair;

    IGNORE_USE_IN_ESCAPING_LAMBDA Optional<int> received_fd_flags;
    pair.receiver->set_up_read_hook([&] {
        (void)pair.receiver->read_as_many_messages_as_possible_without_blocking([&](auto&& message) {
            VERIFY(message.attachments.size() == 1);
            auto fd = message.attachments.dequeue().to_fd();
            received_fd_flags = MUST(Core::System::fcntl(fd, F_GETFD));
            MUST(Core::System::close(fd));
        });
    });

    (void)pair.post_with_descriptor('A');
    spin_until(pair.loop, [&] {
        return received_fd_flags.has_value();
    });
    EXPECT(received_fd_flags.has_value() && (*received_fd_flags & FD_CLOEXEC));
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

    // Each thread's messages arrive exactly once and in the order it posted them; only their interleaving is free.
    Vector<u8> tags_from_other_poster;
    Vector<u8> tags_from_this_thread;
    for (auto tag : received_tags) {
        if (is_ascii_lower_alpha(tag))
            tags_from_other_poster.append(tag);
        else
            tags_from_this_thread.append(tag);
    }
    Vector<u8> expected_from_other_poster;
    Vector<u8> expected_from_this_thread;
    for (u8 tag = 'a'; tag <= 'j'; ++tag)
        expected_from_other_poster.append(tag);
    for (u8 tag = 'A'; tag <= 'J'; ++tag)
        expected_from_this_thread.append(tag);
    EXPECT(tags_from_other_poster == expected_from_other_poster);
    EXPECT(tags_from_this_thread == expected_from_this_thread);
}

static size_t open_descriptor_count()
{
    rlimit limit {};
    VERIFY(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    size_t count = 0;
    for (int fd = 0; static_cast<rlim_t>(fd) < limit.rlim_cur; ++fd) {
        if (descriptor_is_open(fd))
            ++count;
    }
    return count;
}

// A peer that sends descriptors without ever completing a message gets disconnected. The descriptors of the read that
// went over the limit are ours by then, so they must be closed along with the rest.
TEST_CASE(descriptors_received_past_the_limit_are_closed)
{
    raise_descriptor_limit();
    Core::EventLoop loop;
    // NB: The first transport of the process starts the IO thread, which keeps descriptors of its own.
    (void)TRY_OR_FAIL(IPC::Transport::create_paired());
    auto descriptor_count_before = open_descriptor_count();

    {
        int fds[2] {};
        TRY_OR_FAIL(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));
        auto peer = TRY_OR_FAIL(Core::LocalSocket::adopt_fd(fds[1]));

        auto reader_socket = TRY_OR_FAIL(Core::LocalSocket::adopt_fd(fds[0]));
        MUST(reader_socket->set_blocking(false));
        auto transport_owner = TRY_OR_FAIL(IPC::Transport::from_socket(move(reader_socket)));
        IGNORE_USE_IN_ESCAPING_LAMBDA auto& transport = *transport_owner;

        IGNORE_USE_IN_ESCAPING_LAMBDA bool observed_shutdown = false;
        transport.set_up_read_hook([&] {
            if (transport.read_as_many_messages_as_possible_without_blocking([](auto&&) { }) == IPC::Transport::ShouldShutdown::Yes)
                observed_shutdown = true;
        });

        // One byte is never a whole header, so none of these descriptors ever finds its message. Send more of them
        // than the largest message may carry; the reader disconnects at some point, after which sending fails.
        auto null_fd = TRY_OR_FAIL(Core::System::open("/dev/null"sv, O_RDONLY));
        ScopeGuard close_null_fd = [&] { MUST(Core::System::close(null_fd)); };
        Vector<int> descriptors;
        for (size_t i = 0; i < Core::LocalSocket::MAX_TRANSFER_FDS; ++i)
            descriptors.append(null_fd);
        Array<u8, 1> byte { 0 };
        static constexpr size_t batch_count = IPC::MAX_MESSAGE_FD_COUNT / Core::LocalSocket::MAX_TRANSFER_FDS + 2;
        for (size_t batch = 0; batch < batch_count; ++batch) {
            if (peer->send_message(byte, 0, descriptors).is_error())
                break;
        }

        spin_until(loop, [&] {
            return observed_shutdown;
        });
        EXPECT(observed_shutdown);
    }

    EXPECT_EQ(open_descriptor_count(), descriptor_count_before);
}

static int pipe_holding(u8 tag)
{
    auto pipe_fds = MUST(Core::System::pipe2(0));
    MUST(Core::System::write(pipe_fds[1], { &tag, 1 }));
    MUST(Core::System::close(pipe_fds[1]));
    return pipe_fds[0];
}

static Optional<u8> byte_in_descriptor(int fd)
{
    u8 byte = 0;
    if (MUST(Core::System::read(fd, { &byte, 1 })) != 1)
        return {};
    return byte;
}

TEST_CASE(message_with_the_maximum_number_of_descriptors_arrives_whole_and_in_order)
{
    raise_descriptor_limit();
    IGNORE_USE_IN_ESCAPING_LAMBDA TransportPair pair;

    // Tag the descriptors on either side of each sendmsg() boundary, so that a reordering or a lost descriptor shows.
    Array<size_t, 6> tagged_indices { 0, Core::LocalSocket::MAX_TRANSFER_FDS - 1, Core::LocalSocket::MAX_TRANSFER_FDS, IPC::MAX_MESSAGE_FD_COUNT - Core::LocalSocket::MAX_TRANSFER_FDS - 1, IPC::MAX_MESSAGE_FD_COUNT - Core::LocalSocket::MAX_TRANSFER_FDS, IPC::MAX_MESSAGE_FD_COUNT - 1 };
    auto tag_for_index = [](size_t index) { return static_cast<u8>(1 + index % 251); };
    Vector<IPC::Attachment> attachments;
    for (size_t i = 0; i < IPC::MAX_MESSAGE_FD_COUNT; ++i) {
        if (tagged_indices.contains_slow(i))
            attachments.append(IPC::Attachment::from_fd(pipe_holding(tag_for_index(i))));
        else
            attachments.append(IPC::Attachment::from_fd(MUST(Core::System::open("/dev/null"sv, O_RDONLY))));
    }
    IPC::MessageDataType payload;
    payload.append('A');
    MUST(pair.sender->post_message(move(payload), attachments));
    (void)pair.post_with_descriptor('B');

    IGNORE_USE_IN_ESCAPING_LAMBDA Vector<u8> received_tags;
    IGNORE_USE_IN_ESCAPING_LAMBDA bool descriptors_arrived_in_order = true;
    IGNORE_USE_IN_ESCAPING_LAMBDA bool observed_shutdown = false;
    pair.receiver->set_up_read_hook([&] {
        auto should_shutdown = pair.receiver->read_as_many_messages_as_possible_without_blocking([&](auto&& message) {
            auto tag = message.bytes.bytes()[0];
            received_tags.append(tag);
            auto expected_count = tag == 'A' ? IPC::MAX_MESSAGE_FD_COUNT : 1;
            if (message.attachments.size() != expected_count) {
                descriptors_arrived_in_order = false;
                return;
            }
            for (size_t i = 0; !message.attachments.is_empty(); ++i) {
                auto fd = message.attachments.dequeue().to_fd();
                if (tag == 'B' && byte_in_descriptor(fd) != 'B')
                    descriptors_arrived_in_order = false;
                if (tag == 'A' && tagged_indices.contains_slow(i) && byte_in_descriptor(fd) != tag_for_index(i))
                    descriptors_arrived_in_order = false;
                MUST(Core::System::close(fd));
            }
        });
        if (should_shutdown == IPC::Transport::ShouldShutdown::Yes)
            observed_shutdown = true;
    });

    spin_until(pair.loop, [&] { return received_tags.size() == 2 || observed_shutdown; }, 10000_ms);
    EXPECT_EQ(received_tags.size(), 2u);
    EXPECT(received_tags == Vector<u8>({ 'A', 'B' }));
    EXPECT(descriptors_arrived_in_order);
    EXPECT(!observed_shutdown);
}

TEST_CASE(message_with_the_maximum_number_of_descriptors_and_a_payload_in_shared_memory_arrives_whole)
{
    raise_descriptor_limit();
    IGNORE_USE_IN_ESCAPING_LAMBDA TransportPair pair;

    Vector<IPC::Attachment> attachments;
    attachments.append(IPC::Attachment::from_fd(pipe_holding('F')));
    for (size_t i = 2; i < IPC::MAX_MESSAGE_FD_COUNT; ++i)
        attachments.append(IPC::Attachment::from_fd(MUST(Core::System::open("/dev/null"sv, O_RDONLY))));
    attachments.append(IPC::Attachment::from_fd(pipe_holding('L')));
    // NB: A frame holds at most 64 KiB, so this payload goes in shared memory.
    static constexpr size_t payload_size = 64 * KiB + 1;
    IPC::MessageDataType payload;
    payload.resize(payload_size);
    payload.span().fill('A');
    MUST(pair.sender->post_message(move(payload), attachments));
    (void)pair.post_with_descriptor('B');

    IGNORE_USE_IN_ESCAPING_LAMBDA Vector<u8> received_tags;
    IGNORE_USE_IN_ESCAPING_LAMBDA bool arrived_whole = true;
    IGNORE_USE_IN_ESCAPING_LAMBDA bool observed_shutdown = false;
    pair.receiver->set_up_read_hook([&] {
        auto should_shutdown = pair.receiver->read_as_many_messages_as_possible_without_blocking([&](auto&& message) {
            auto bytes = message.bytes.bytes();
            received_tags.append(bytes[0]);
            Vector<int> fds;
            while (!message.attachments.is_empty())
                fds.append(message.attachments.dequeue().to_fd());
            if (bytes[0] == 'A') {
                if (bytes.size() != payload_size || any_of(bytes, [](u8 byte) { return byte != 'A'; }))
                    arrived_whole = false;
                if (fds.size() != IPC::MAX_MESSAGE_FD_COUNT || byte_in_descriptor(fds.first()) != 'F' || byte_in_descriptor(fds.last()) != 'L')
                    arrived_whole = false;
            } else if (fds.size() != 1 || byte_in_descriptor(fds.first()) != 'B') {
                arrived_whole = false;
            }
            for (auto fd : fds)
                MUST(Core::System::close(fd));
        });
        if (should_shutdown == IPC::Transport::ShouldShutdown::Yes)
            observed_shutdown = true;
    });

    spin_until(pair.loop, [&] { return received_tags.size() == 2 || observed_shutdown; }, 10000_ms);
    EXPECT(received_tags == Vector<u8>({ 'A', 'B' }));
    EXPECT(arrived_whole);
    EXPECT(!observed_shutdown);
}

TEST_CASE(transports_on_the_shared_io_thread_work_alone)
{
    Core::EventLoop loop;
    auto post = [](IPC::Transport& transport, u8 value) {
        IPC::MessageDataType payload;
        payload.append(value);
        Vector<IPC::Attachment> attachments;
        MUST(transport.post_message(move(payload), attachments));
    };
    auto read_one = [&](IPC::Transport& transport) -> Optional<u8> {
        Optional<u8> value;
        spin_until(loop, [&] {
            (void)transport.read_as_many_messages_as_possible_without_blocking([&](auto&& message) {
                value = message.bytes.bytes()[0];
            });
            return value.has_value();
        });
        return value;
    };

    Vector<NonnullOwnPtr<IPC::Transport>> local_ends;
    Vector<NonnullOwnPtr<IPC::Transport>> remote_ends;
    for (size_t i = 0; i < 8; ++i) {
        auto paired = TRY_OR_FAIL(IPC::Transport::create_paired());
        local_ends.append(move(paired.local));
        remote_ends.append(TRY_OR_FAIL(paired.remote_handle.create_transport()));
    }
    for (size_t i = 0; i < local_ends.size(); ++i)
        post(*local_ends[i], static_cast<u8>(i));
    for (size_t i = 0; i < remote_ends.size(); ++i) {
        EXPECT_EQ(read_one(*remote_ends[i]), static_cast<u8>(i));
        post(*remote_ends[i], static_cast<u8>(i + 100));
    }

    // One that goes must not take the others with it, and its peer finds out.
    local_ends.remove(3);
    bool should_shut_down = false;
    spin_until(loop, [&] {
        should_shut_down = remote_ends[3]->read_as_many_messages_as_possible_without_blocking([](auto&&) { }) == IPC::Transport::ShouldShutdown::Yes;
        return should_shut_down;
    });
    remote_ends.remove(3);
    for (size_t i = 0; i < local_ends.size(); ++i) {
        auto expected = i < 3 ? i : i + 1;
        EXPECT_EQ(read_one(*local_ends[i]), static_cast<u8>(expected + 100));
    }

    // A transport can leave its shared thread for another owner. The old owner hands on what it did read, and the new
    // one gets the rest, so that each message arrives once and in order.
    post(*local_ends[0], 42);
    auto handle = TRY_OR_FAIL(remote_ends[0]->release_for_transfer());
    Vector<u8> received;
    (void)remote_ends[0]->read_as_many_messages_as_possible_without_blocking([&](auto&& message) {
        received.append(message.bytes.bytes()[0]);
    });
    remote_ends.remove(0);
    auto new_owner = TRY_OR_FAIL(handle.create_transport());
    post(*local_ends[0], 43);
    spin_until(loop, [&] {
        (void)new_owner->read_as_many_messages_as_possible_without_blocking([&](auto&& message) {
            received.append(message.bytes.bytes()[0]);
        });
        return received.size() >= 2;
    });
    EXPECT_EQ(received, (Vector<u8> { 42, 43 }));
}

TEST_CASE(messages_arrive_in_order_while_the_reader_is_transferred_again_and_again)
{
    Core::EventLoop loop;

    auto paired = TRY_OR_FAIL(IPC::Transport::create_paired());
    IGNORE_USE_IN_ESCAPING_LAMBDA auto sender = move(paired.local);
    auto reader = TRY_OR_FAIL(paired.remote_handle.create_transport());

    // NB: The padding makes a message end anywhere in a read of the transport.
    static constexpr u32 message_count = 20000;
    static constexpr size_t padding = 701;
    auto sending_thread = Threading::Thread::construct("Sender"sv, [&sender] {
        for (u32 i = 0; i < message_count; ++i) {
            IPC::MessageDataType payload;
            payload.resize(sizeof(i) + padding);
            memcpy(payload.data(), &i, sizeof(i));
            Vector<IPC::Attachment> attachments;
            MUST(sender->post_message(move(payload), attachments));
        }
        return static_cast<intptr_t>(0);
    });
    sending_thread->start();

    u32 next = 0;
    bool in_order = true;
    auto read_what_is_there = [&](IPC::Transport& transport) {
        (void)transport.read_as_many_messages_as_possible_without_blocking([&](auto&& message) {
            u32 value = 0;
            memcpy(&value, message.bytes.bytes().data(), sizeof(value));
            in_order = in_order && value == next;
            ++next;
        });
    };
    spin_until(loop, [&] {
        read_what_is_there(*reader);
        auto handle = MUST(reader->release_for_transfer());
        read_what_is_there(*reader);
        reader = MUST(handle.create_transport());
        return next >= message_count || !in_order; }, 30000_ms);
    (void)sending_thread->join();

    EXPECT(in_order);
    EXPECT_EQ(next, message_count);
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

    auto transport_owner = TRY_OR_FAIL(IPC::Transport::from_socket(move(reader_socket)));
    auto& transport = *transport_owner;

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> observed_shutdown = false;

    transport.set_up_read_hook([&] {
        auto should_shutdown = transport.read_as_many_messages_as_possible_without_blocking([](auto&&) {
        });
        if (should_shutdown == IPC::Transport::ShouldShutdown::Yes)
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

    auto transport_owner = TRY_OR_FAIL(IPC::Transport::from_socket(move(reader_socket)));
    auto& transport = *transport_owner;

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> observed_shutdown = false;

    transport.set_up_read_hook([&] {
        auto should_shutdown = transport.read_as_many_messages_as_possible_without_blocking([](auto&&) {
        });
        if (should_shutdown == IPC::Transport::ShouldShutdown::Yes)
            observed_shutdown.store(true, AK::MemoryOrder::memory_order_relaxed);
    });

    transport.close();

    spin_until(loop, [&] {
        return observed_shutdown.load(AK::MemoryOrder::memory_order_relaxed);
    });

    EXPECT(observed_shutdown.load(AK::MemoryOrder::memory_order_relaxed));
}
