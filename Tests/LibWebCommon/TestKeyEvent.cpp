/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/UIEvents/PhysicalKey.h>

static Web::KeyEvent round_trip(Web::KeyEvent const& event)
{
    IPC::MessageBuffer message;
    IPC::Encoder encoder { message };
    MUST(encoder.encode(event));
    Queue<IPC::Attachment> attachments;
    FixedMemoryStream stream { message.data().span() };
    IPC::Decoder decoder { stream, attachments };
    return MUST(decoder.decode<Web::KeyEvent>());
}

TEST_CASE(editing_commands_survive_ipc_and_cloning)
{
    using Type = Web::TextEditingCommand::Type;
    Web::KeyEvent event;
    event.type = Web::KeyEvent::Type::KeyDown;
    event.key = Web::UIEvents::Key_A;
    event.modifiers = Web::UIEvents::Mod_Ctrl;
    event.code_point = 'a';
    event.repeat = true;
    event.id = 71;
    event.editing_commands = { { Type::MoveToBeginningOfParagraph, false }, { Type::MoveForward, true } };
    event.native_editing_binding_resolved = true;
    event.physical_key = Web::UIEvents::Key_Q;

    for (auto const& copy : { round_trip(event), event.clone_without_browser_data() }) {
        EXPECT(copy.editing_commands == event.editing_commands);
        EXPECT(copy.native_editing_binding_resolved);
        EXPECT_EQ(copy.key, Web::UIEvents::Key_A);
        EXPECT_EQ(copy.modifiers, Web::UIEvents::Mod_Ctrl);
        EXPECT_EQ(copy.code_point, static_cast<u32>('a'));
        EXPECT(copy.repeat);
        EXPECT_EQ(copy.id, 71u);
        EXPECT_EQ(copy.physical_key, Web::UIEvents::Key_Q);
        EXPECT(!copy.browser_data);
    }
}

TEST_CASE(unsupported_native_bindings_remain_distinct_from_raw_keys)
{
    Web::KeyEvent native;
    native.type = Web::KeyEvent::Type::KeyDown;
    native.native_editing_binding_resolved = true;
    auto copy = round_trip(native);
    EXPECT(copy.native_editing_binding_resolved);
    EXPECT(copy.editing_commands.is_empty());

    Web::KeyEvent raw;
    raw.type = Web::KeyEvent::Type::KeyDown;
    EXPECT(!round_trip(raw).native_editing_binding_resolved);
}

TEST_CASE(physical_keys_follow_key_positions)
{
    using namespace Web::UIEvents;

    EXPECT_EQ(physical_key_from_scan_code(0x15), Key_Y);
    EXPECT_EQ(physical_key_from_scan_code(0x2C), Key_Z);
    EXPECT_EQ(physical_key_from_scan_code(0x21), Key_F);
    EXPECT_EQ(physical_key_from_scan_code(0x02), Key_1);
    EXPECT_EQ(physical_key_from_scan_code(0x0B), Key_0);
    EXPECT_EQ(physical_key_from_scan_code(0x29), Key_Backtick);
    EXPECT_EQ(physical_key_from_scan_code(0x27), Key_Semicolon);
    EXPECT_EQ(physical_key_from_scan_code(0x35), Key_Slash);
    EXPECT_EQ(physical_key_from_scan_code(0x1C), Key_Invalid);
    EXPECT_EQ(physical_key_from_scan_code(0x39), Key_Invalid);
    EXPECT_EQ(physical_key_from_scan_code(0x52), Key_Invalid);

    EXPECT_EQ(physical_key_from_macos_virtual_key(0x10), Key_Y);
    EXPECT_EQ(physical_key_from_macos_virtual_key(0x06), Key_Z);
    EXPECT_EQ(physical_key_from_macos_virtual_key(0x03), Key_F);
    EXPECT_EQ(physical_key_from_macos_virtual_key(0x00), Key_A);
    EXPECT_EQ(physical_key_from_macos_virtual_key(0x12), Key_1);
    EXPECT_EQ(physical_key_from_macos_virtual_key(0x1D), Key_0);
    EXPECT_EQ(physical_key_from_macos_virtual_key(0x0A), Key_Backtick);
    EXPECT_EQ(physical_key_from_macos_virtual_key(0x32), Key_Invalid);
    EXPECT_EQ(physical_key_from_macos_virtual_key(0x24), Key_Invalid);
    EXPECT_EQ(physical_key_from_macos_virtual_key(0x31), Key_Invalid);
    EXPECT_EQ(physical_key_from_macos_virtual_key(0x52), Key_Invalid);
}
