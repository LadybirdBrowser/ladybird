/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Forward.h>
#include <LibIPC/TransportMachPort.h>
#include <LibTest/TestCase.h>

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
