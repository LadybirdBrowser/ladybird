/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/Vector.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Forward.h>
#include <LibIPC/Message.h>
#include <LibIPC/TransportHandle.h>
#include <LibIPC/TransportMachPort.h>
#include <LibTest/TestCase.h>

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
