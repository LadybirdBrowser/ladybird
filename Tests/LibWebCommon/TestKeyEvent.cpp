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

    for (auto const& copy : { round_trip(event), event.clone_without_browser_data() }) {
        EXPECT(copy.editing_commands == event.editing_commands);
        EXPECT(copy.native_editing_binding_resolved);
        EXPECT_EQ(copy.key, Web::UIEvents::Key_A);
        EXPECT_EQ(copy.modifiers, Web::UIEvents::Mod_Ctrl);
        EXPECT_EQ(copy.code_point, static_cast<u32>('a'));
        EXPECT(copy.repeat);
        EXPECT_EQ(copy.id, 71u);
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
