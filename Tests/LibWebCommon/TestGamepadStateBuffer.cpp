/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/Gamepad/GamepadSharedState.h>
#include <LibWebCommon/Gamepad/GamepadStateBuffer.h>

#ifdef AK_OS_MACOS
#    include <LibIPC/TransportMachPort.h>
#    include <mach/mach_vm.h>
#endif

using namespace Web::Gamepad;

static Core::AnonymousBuffer create_producer_buffer()
{
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(sizeof(SharedGamepadStateBuffer), Core::AnonymousBuffer::Sealability::Sealable));
    new (buffer.data<void>()) SharedGamepadStateBuffer {};
    return buffer;
}

static GamepadStateBuffer round_trip(GamepadStateBuffer const& buffer)
{
    IPC::MessageBuffer message;
    IPC::Encoder encoder { message };
    MUST(encoder.encode(buffer));
    Queue<IPC::Attachment> attachments;
    for (auto& attachment : message.take_attachments())
        attachments.enqueue(move(attachment));
    FixedMemoryStream stream { message.data().span() };
    IPC::Decoder decoder { stream, attachments };
    return MUST(decoder.decode<GamepadStateBuffer>());
}

TEST_CASE(invalid_buffer_round_trip)
{
    auto reader = round_trip(GamepadStateBuffer {});
    EXPECT(!reader.is_valid());
    EXPECT_EQ(reader.data(), nullptr);
}

TEST_CASE(reject_incorrect_buffer_size)
{
    EXPECT(GamepadStateBuffer::create(Core::AnonymousBuffer {}).is_error());
    auto too_small = MUST(Core::AnonymousBuffer::create_with_size(sizeof(SharedGamepadStateBuffer) - 1));
    EXPECT(GamepadStateBuffer::create(too_small).is_error());
    auto too_large = MUST(Core::AnonymousBuffer::create_with_size(sizeof(SharedGamepadStateBuffer) + 1));
    EXPECT(GamepadStateBuffer::create(too_large).is_error());
}

TEST_CASE(producer_updates_remain_visible_after_transfer)
{
    auto producer = create_producer_buffer();
    auto& slot = reinterpret_cast<SharedGamepadStateBuffer*>(producer.data<void>())->slots[0];
    auto reader = round_trip(MUST(GamepadStateBuffer::create(producer)));
    u32 sequence = 0;

    GamepadState state { 7, { -32768, 0, 32767 }, { 0, 1 } };
    publish_gamepad_state_to_slot(slot, state);
    auto observed = read_gamepad_state_from_slot(reader.data()->slots[0], sequence);
    EXPECT(observed.has_value());
    EXPECT_EQ(*observed, state);

    state.axis_values[0] = 100;
    publish_gamepad_state_to_slot(slot, state);
    observed = read_gamepad_state_from_slot(reader.data()->slots[0], sequence);
    EXPECT(observed.has_value());
    EXPECT_EQ(*observed, state);

    producer = {};
    sequence = 0;
    observed = read_gamepad_state_from_slot(reader.data()->slots[0], sequence);
    EXPECT(observed.has_value());
    EXPECT_EQ(*observed, state);
}

#ifdef AK_OS_MACOS
TEST_CASE(transferred_memory_entry_cannot_be_mapped_writable)
{
    auto producer = create_producer_buffer();
    auto sent_buffer = MUST(GamepadStateBuffer::create(producer));
    IPC::MessageBuffer message;
    IPC::Encoder encoder { message };
    MUST(encoder.encode(sent_buffer));

    auto paired = MUST(IPC::TransportMachPort::create_paired());
    auto peer = MUST(paired.remote_handle.create_transport());
    auto attachments = message.take_attachments();
    MUST(paired.local->post_message(message.take_data(), attachments));
    paired.local->flush();
    peer->wait_until_incoming_is_current();

    size_t received = 0;
    (void)peer->read_as_many_messages_as_possible_without_blocking([&](IPC::TransportMachPort::Message&& incoming) {
        ++received;
        EXPECT_EQ(incoming.attachments.size(), 1uz);
        auto attachment = incoming.attachments.dequeue();
        mach_vm_address_t writable_address = 0;
        auto result = mach_vm_map(mach_task_self(), &writable_address, sizeof(SharedGamepadStateBuffer), 0,
            VM_FLAGS_ANYWHERE, attachment.mach_port().port(), 0, false,
            VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
        EXPECT_EQ(result, KERN_INVALID_RIGHT);
        if (result == KERN_SUCCESS)
            EXPECT_EQ(mach_vm_deallocate(mach_task_self(), writable_address, sizeof(SharedGamepadStateBuffer)), KERN_SUCCESS);

        incoming.attachments.enqueue(move(attachment));
        FixedMemoryStream stream { incoming.bytes.bytes() };
        IPC::Decoder decoder { stream, incoming.attachments };
        auto received_buffer = MUST(decoder.decode<GamepadStateBuffer>());
        EXPECT_NE(mach_vm_protect(mach_task_self(), reinterpret_cast<mach_vm_address_t>(received_buffer.data()),
                      sizeof(SharedGamepadStateBuffer), false, VM_PROT_READ | VM_PROT_WRITE),
            KERN_SUCCESS);

        auto& slot = reinterpret_cast<SharedGamepadStateBuffer*>(producer.data<void>())->slots[0];
        GamepadState state { 3, { 42 }, { 1 } };
        publish_gamepad_state_to_slot(slot, state);
        u32 sequence = 0;
        auto observed = read_gamepad_state_from_slot(received_buffer.data()->slots[0], sequence);
        EXPECT(observed.has_value());
        EXPECT_EQ(*observed, state);
    });
    EXPECT_EQ(received, 1uz);
}

TEST_CASE(reject_missing_memory_entry)
{
    Array<u8, 1> bytes { 1 };
    FixedMemoryStream stream { bytes.span() };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    EXPECT(decoder.decode<GamepadStateBuffer>().is_error());
}
#endif
