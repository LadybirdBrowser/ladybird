/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCore/System.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Forward.h>
#include <LibIPC/Limits.h>
#include <LibIPC/Message.h>
#include <LibIPC/TransportHandle.h>
#include <LibIPC/TransportMachPort.h>
#include <LibTest/TestCase.h>
#include <fcntl.h>
#include <unistd.h>

static void post_transport_handle(IPC::TransportMachPort& carrier, IPC::TransportHandle const& handle)
{
    IPC::MessageBuffer buffer;
    IPC::Encoder encoder(buffer);
    MUST(encoder.encode(handle));
    auto attachments = buffer.take_attachments();
    MUST(carrier.post_message(buffer.take_data(), attachments));
    carrier.flush();
}

static OwnPtr<IPC::TransportMachPort> receive_transport(IPC::TransportMachPort& carrier)
{
    OwnPtr<IPC::TransportMachPort> transport;
    carrier.wait_until_readable();
    (void)carrier.read_as_many_messages_as_possible_without_blocking([&](IPC::TransportMachPort::Message&& message) {
        FixedMemoryStream stream { message.bytes.bytes() };
        IPC::Decoder decoder { stream, message.attachments };
        transport = MUST(MUST(decoder.decode<IPC::TransportHandle>()).create_transport());
    });
    return transport;
}

static size_t read_until_eof(IPC::TransportMachPort& transport, u8 expected_byte)
{
    size_t received = 0;
    auto should_shutdown = IPC::TransportMachPort::ShouldShutdown::No;
    while (should_shutdown == IPC::TransportMachPort::ShouldShutdown::No) {
        transport.wait_until_readable();
        should_shutdown = transport.read_as_many_messages_as_possible_without_blocking([&](IPC::TransportMachPort::Message&& message) {
            auto bytes = message.bytes.bytes();
            EXPECT_EQ(bytes.size(), 1uz);
            if (bytes.size() == 1)
                EXPECT_EQ(bytes[0], expected_byte);
            ++received;
        });
    }
    return received;
}

TEST_CASE(receive_barrier_publishes_preceding_messages)
{
    auto paired = TRY_OR_FAIL(IPC::TransportMachPort::create_paired());
    auto peer = TRY_OR_FAIL(paired.remote_handle.create_transport());

    for (size_t i = 0; i < 32; ++i) {
        IPC::MessageDataType payload;
        payload.append(static_cast<u8>(i));
        Vector<IPC::Attachment> attachments;
        TRY_OR_FAIL(paired.local->post_message(move(payload), attachments));
        paired.local->flush();
        peer->wait_until_incoming_is_current();

        size_t received = 0;
        (void)peer->read_as_many_messages_as_possible_without_blocking([&](IPC::TransportMachPort::Message&& message) {
            EXPECT_EQ(message.bytes.bytes()[0], static_cast<u8>(i));
            ++received;
        });
        EXPECT_EQ(received, 1uz);
    }

    // A barrier with no preceding messages must also complete.
    peer->wait_until_incoming_is_current();
    OwnPtr<IPC::TransportMachPort> sender = move(paired.local);
    sender.clear();
    peer->wait_until_readable();
    EXPECT_EQ(peer->read_as_many_messages_as_possible_without_blocking([](auto&&) { }),
        IPC::TransportMachPort::ShouldShutdown::Yes);
}

TEST_CASE(burst_to_not_yet_started_peer_is_delivered)
{
    constexpr size_t message_count = 32;

    auto paired = TRY_OR_FAIL(IPC::TransportMachPort::create_paired());

    for (size_t i = 0; i < message_count; ++i) {
        IPC::MessageDataType payload;
        payload.append(static_cast<u8>(i));
        Vector<IPC::Attachment> attachments;
        TRY_OR_FAIL(paired.local->post_message(move(payload), attachments));
    }

    paired.local->close_after_sending_all_pending_messages();

    // Construct the peer only after sending the startup burst.
    auto peer_transport = TRY_OR_FAIL(paired.remote_handle.create_transport());

    size_t received = 0;
    while (received < message_count) {
        peer_transport->wait_until_readable();
        (void)peer_transport->read_as_many_messages_as_possible_without_blocking([&](IPC::TransportMachPort::Message&& message) {
            auto bytes = message.bytes.bytes();
            EXPECT_EQ(bytes.size(), 1uz);
            if (bytes.size() == 1)
                EXPECT_EQ(bytes[0], static_cast<u8>(received));
            ++received;
        });
    }

    EXPECT_EQ(received, message_count);
}

TEST_CASE(endpoint_whose_peer_closed_in_flight_reads_what_was_sent_then_eof)
{
    auto carrier = TRY_OR_FAIL(IPC::TransportMachPort::create_paired());
    auto channel = TRY_OR_FAIL(IPC::TransportMachPort::create_paired());

    IPC::MessageDataType payload;
    payload.append(42);
    Vector<IPC::Attachment> attachments;
    TRY_OR_FAIL(channel.local->post_message(move(payload), attachments));

    post_transport_handle(*carrier.local, channel.remote_handle);

    // Close the channel's local end while its other end is still queued on the carrier, so the send right in that
    // message names a dead port by the time the carrier's peer receives it.
    channel.local->close_after_sending_all_pending_messages();
    OwnPtr<IPC::TransportMachPort> closed_end = move(channel.local);
    closed_end.clear();

    auto carrier_peer = TRY_OR_FAIL(carrier.remote_handle.create_transport());
    auto endpoint = receive_transport(*carrier_peer);
    EXPECT(endpoint);
    if (!endpoint)
        return;

    EXPECT_EQ(read_until_eof(*endpoint, 42), 1uz);
}

TEST_CASE(endpoint_whose_peer_closed_can_be_transferred_again)
{
    auto first_carrier = TRY_OR_FAIL(IPC::TransportMachPort::create_paired());
    auto second_carrier = TRY_OR_FAIL(IPC::TransportMachPort::create_paired());
    auto channel = TRY_OR_FAIL(IPC::TransportMachPort::create_paired());

    post_transport_handle(*first_carrier.local, channel.remote_handle);

    OwnPtr<IPC::TransportMachPort> closed_end = move(channel.local);
    closed_end.clear();

    auto first_carrier_peer = TRY_OR_FAIL(first_carrier.remote_handle.create_transport());
    auto endpoint = receive_transport(*first_carrier_peer);
    EXPECT(endpoint);
    if (!endpoint)
        return;

    post_transport_handle(*second_carrier.local, TRY_OR_FAIL(endpoint->release_for_transfer()));
    endpoint.clear();

    auto second_carrier_peer = TRY_OR_FAIL(second_carrier.remote_handle.create_transport());
    auto transferred_endpoint = receive_transport(*second_carrier_peer);
    EXPECT(transferred_endpoint);
    if (!transferred_endpoint)
        return;

    EXPECT_EQ(read_until_eof(*transferred_endpoint, 0), 0uz);
}

TEST_CASE(explicit_close_notifies_peer_while_sender_is_alive)
{
    auto paired = TRY_OR_FAIL(IPC::TransportMachPort::create_paired());
    auto peer = TRY_OR_FAIL(paired.remote_handle.create_transport());
    paired.local->close();
    EXPECT_EQ(read_until_eof(*peer, 0), 0uz);
}

TEST_CASE(close_after_sending_delivers_queued_messages_then_eof)
{
    auto paired = TRY_OR_FAIL(IPC::TransportMachPort::create_paired());
    for (size_t i = 0; i < 32; ++i) {
        IPC::MessageDataType payload;
        payload.append(42);
        Vector<IPC::Attachment> attachments;
        TRY_OR_FAIL(paired.local->post_message(move(payload), attachments));
    }
    paired.local->close_after_sending_all_pending_messages();
    auto peer = TRY_OR_FAIL(paired.remote_handle.create_transport());
    EXPECT_EQ(read_until_eof(*peer, 42), 32uz);
}

TEST_CASE(eof_during_read_callback_preserves_pending_messages)
{
    auto paired = TRY_OR_FAIL(IPC::TransportMachPort::create_paired());
    auto peer = TRY_OR_FAIL(paired.remote_handle.create_transport());

    IPC::MessageDataType payload;
    payload.append(42);
    Vector<IPC::Attachment> attachments;
    TRY_OR_FAIL(paired.local->post_message(move(payload), attachments));
    paired.local->flush();
    peer->wait_until_incoming_is_current();

    size_t received = 0;
    auto should_shutdown = peer->read_as_many_messages_as_possible_without_blocking([&](auto&&) {
        ++received;
        IPC::MessageDataType next_payload;
        next_payload.append(43);
        MUST(paired.local->post_message(move(next_payload), attachments));
        paired.local->close_after_sending_all_pending_messages();

        // Publish the second message and EOF while the first message's callback is running.
        // The receive barrier follows both on the same port, so no timing assumptions are needed.
        peer->wait_until_incoming_is_current();
    });
    EXPECT_EQ(received, 1uz);
    EXPECT_EQ(should_shutdown, IPC::TransportMachPort::ShouldShutdown::No);
    EXPECT_EQ(read_until_eof(*peer, 43), 1uz);
}

static Vector<IPC::Attachment> make_attachments(size_t count)
{
    Vector<IPC::Attachment> attachments;
    attachments.ensure_capacity(count);
    for (size_t i = 0; i < count; ++i)
        attachments.unchecked_append(IPC::Attachment::from_fd(MUST(Core::System::open("/dev/null"sv, O_RDONLY))));
    return attachments;
}

TEST_CASE(message_with_the_maximum_number_of_attachments_arrives_whole)
{
    auto paired = TRY_OR_FAIL(IPC::TransportMachPort::create_paired());
    auto peer = TRY_OR_FAIL(paired.remote_handle.create_transport());

    IPC::MessageDataType payload;
    payload.append(42);
    auto attachments = make_attachments(IPC::MAX_MESSAGE_FD_COUNT);
    TRY_OR_FAIL(paired.local->post_message(move(payload), attachments));
    paired.local->flush();
    peer->wait_until_incoming_is_current();

    size_t received = 0;
    auto should_shutdown = peer->read_as_many_messages_as_possible_without_blocking([&](IPC::TransportMachPort::Message&& message) {
        ++received;
        EXPECT_EQ(message.bytes.bytes().size(), 1uz);
        EXPECT_EQ(message.attachments.size(), IPC::MAX_MESSAGE_FD_COUNT);
        auto fd = message.attachments.dequeue().to_fd();
        EXPECT(fd >= 0);
        if (fd >= 0)
            MUST(Core::System::close(fd));
    });
    EXPECT_EQ(received, 1uz);
    EXPECT_EQ(should_shutdown, IPC::TransportMachPort::ShouldShutdown::No);
}

TEST_CASE(message_with_too_many_attachments_disconnects_the_peer)
{
    auto paired = TRY_OR_FAIL(IPC::TransportMachPort::create_paired());
    auto peer = TRY_OR_FAIL(paired.remote_handle.create_transport());

    IPC::MessageDataType payload;
    payload.append(42);
    auto attachments = make_attachments(IPC::MAX_MESSAGE_FD_COUNT + 1);
    TRY_OR_FAIL(paired.local->post_message(move(payload), attachments));
    paired.local->flush();

    size_t received = 0;
    auto should_shutdown = IPC::TransportMachPort::ShouldShutdown::No;
    while (should_shutdown == IPC::TransportMachPort::ShouldShutdown::No) {
        peer->wait_until_readable();
        should_shutdown = peer->read_as_many_messages_as_possible_without_blocking([&](auto&&) { ++received; });
    }
    EXPECT_EQ(received, 0uz);
    EXPECT(!peer->is_open());
}

// Two peers that each post more messages than the kernel queue holds, before either reads: a transport whose IO thread
// blocks in a send can't drain its own queue, so two of them facing each other would wait on each other for good.
TEST_CASE(a_flood_in_both_directions_does_not_deadlock)
{
    auto paired = TRY_OR_FAIL(IPC::TransportMachPort::create_paired());
    auto peer = TRY_OR_FAIL(paired.remote_handle.create_transport());

    // Well past MACH_PORT_QLIMIT_LARGE (1024) in each direction.
    constexpr size_t message_count = 4096;
    for (size_t i = 0; i < message_count; ++i) {
        IPC::MessageDataType to_peer;
        to_peer.append(1);
        Vector<IPC::Attachment> no_attachments;
        TRY_OR_FAIL(paired.local->post_message(move(to_peer), no_attachments));

        IPC::MessageDataType to_local;
        to_local.append(2);
        TRY_OR_FAIL(peer->post_message(move(to_local), no_attachments));
    }

    size_t received_by_peer = 0;
    size_t received_by_local = 0;
    while (received_by_peer < message_count || received_by_local < message_count) {
        if (received_by_peer < message_count) {
            peer->wait_until_readable();
            (void)peer->read_as_many_messages_as_possible_without_blocking([&](IPC::TransportMachPort::Message&& message) {
                EXPECT_EQ(message.bytes.bytes()[0], 1u);
                ++received_by_peer;
            });
        }
        if (received_by_local < message_count) {
            paired.local->wait_until_readable();
            (void)paired.local->read_as_many_messages_as_possible_without_blocking([&](IPC::TransportMachPort::Message&& message) {
                EXPECT_EQ(message.bytes.bytes()[0], 2u);
                ++received_by_local;
            });
        }
    }
    EXPECT_EQ(received_by_peer, message_count);
    EXPECT_EQ(received_by_local, message_count);
}

// The same flood with payloads too large to send inline, which travel out of line.
// A send that times out on a full peer queue comes back pseudo-received, with a send right of its own on the
// destination. Every one of them has to be released again, or the peer never hears that its last sender has gone.
TEST_CASE(closing_after_timed_out_sends_still_reaches_the_peer_as_eof)
{
    auto paired = TRY_OR_FAIL(IPC::TransportMachPort::create_paired());
    auto peer = TRY_OR_FAIL(paired.remote_handle.create_transport());

    // Well past MACH_PORT_QLIMIT_LARGE (1024) while the peer reads nothing, so most of these time out at least once;
    // some inline, some out of line.
    constexpr size_t message_count = 4096;
    for (size_t i = 0; i < message_count; ++i) {
        IPC::MessageDataType to_peer;
        to_peer.resize(i % 2 == 0 ? 1 : 8192);
        to_peer[0] = 7;
        Vector<IPC::Attachment> no_attachments;
        TRY_OR_FAIL(paired.local->post_message(move(to_peer), no_attachments));
    }
    paired.local->flush();

    size_t received = 0;
    while (received < message_count) {
        peer->wait_until_readable();
        (void)peer->read_as_many_messages_as_possible_without_blocking([&](IPC::TransportMachPort::Message&& message) {
            EXPECT_EQ(message.bytes.bytes()[0], 7u);
            ++received;
        });
    }
    EXPECT_EQ(received, message_count);

    OwnPtr<IPC::TransportMachPort> closed_end = move(paired.local);
    closed_end.clear();

    // Polled, rather than waited for, so a leaked right fails the test instead of hanging it.
    auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(5);
    auto should_shutdown = IPC::TransportMachPort::ShouldShutdown::No;
    while (should_shutdown == IPC::TransportMachPort::ShouldShutdown::No && MonotonicTime::now() < deadline) {
        should_shutdown = peer->read_as_many_messages_as_possible_without_blocking([&](IPC::TransportMachPort::Message&&) {
            FAIL("nothing else was sent");
        });
        if (should_shutdown == IPC::TransportMachPort::ShouldShutdown::No)
            usleep(10'000);
    }
    EXPECT_EQ(should_shutdown, IPC::TransportMachPort::ShouldShutdown::Yes);
}

TEST_CASE(a_flood_of_out_of_line_messages_in_both_directions_does_not_deadlock)
{
    auto paired = TRY_OR_FAIL(IPC::TransportMachPort::create_paired());
    auto peer = TRY_OR_FAIL(paired.remote_handle.create_transport());

    constexpr size_t message_count = 2048;
    constexpr size_t payload_size = 8192;
    for (size_t i = 0; i < message_count; ++i) {
        IPC::MessageDataType to_peer;
        to_peer.resize(payload_size);
        to_peer[0] = 1;
        Vector<IPC::Attachment> no_attachments;
        TRY_OR_FAIL(paired.local->post_message(move(to_peer), no_attachments));

        IPC::MessageDataType to_local;
        to_local.resize(payload_size);
        to_local[0] = 2;
        TRY_OR_FAIL(peer->post_message(move(to_local), no_attachments));
    }

    size_t received_by_peer = 0;
    size_t received_by_local = 0;
    while (received_by_peer < message_count || received_by_local < message_count) {
        if (received_by_peer < message_count) {
            peer->wait_until_readable();
            (void)peer->read_as_many_messages_as_possible_without_blocking([&](IPC::TransportMachPort::Message&& message) {
                EXPECT_EQ(message.bytes.bytes().size(), payload_size);
                EXPECT_EQ(message.bytes.bytes()[0], 1u);
                ++received_by_peer;
            });
        }
        if (received_by_local < message_count) {
            paired.local->wait_until_readable();
            (void)paired.local->read_as_many_messages_as_possible_without_blocking([&](IPC::TransportMachPort::Message&& message) {
                EXPECT_EQ(message.bytes.bytes().size(), payload_size);
                EXPECT_EQ(message.bytes.bytes()[0], 2u);
                ++received_by_local;
            });
        }
    }
    EXPECT_EQ(received_by_peer, message_count);
    EXPECT_EQ(received_by_local, message_count);
}
