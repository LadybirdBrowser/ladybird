/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/NativeTextEditingBindings.mm>

#include <QKeyEvent>

static bool custom_bindings()
{
    auto* mode = getenv("LADYBIRD_TEST_BINDINGS_MODE");
    return mode && StringView { mode, strlen(mode) } == "custom"sv;
}

static Ladybird::NativeTextEditingBindingResult interpret(unichar letter, unsigned short key_code, NSEventModifierFlags flags = NSEventModifierFlagControl, NSEventType type = NSEventTypeKeyDown, bool repeat = false)
{
    unichar character = letter & 0x1f;
    unichar unmodified = flags & NSEventModifierFlagShift ? toupper(letter) : letter;
    auto* event = [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:flags timestamp:42 windowNumber:0 context:nil characters:[NSString stringWithCharacters:&character length:1] charactersIgnoringModifiers:[NSString stringWithCharacters:&unmodified length:1] isARepeat:repeat keyCode:key_code];
    auto* recorder = [LadybirdNativeTextEditingRecorder new];
    auto result = [recorder commandsForEvent:event];
    [recorder release];
    return result;
}

static void expect_command(unichar letter, unsigned short key_code, Web::TextEditingCommand::Type type, bool extend_selection = false, NSEventModifierFlags flags = NSEventModifierFlagControl)
{
    auto result = interpret(letter, key_code, flags);
    EXPECT(result.binding_resolved);
    EXPECT_EQ(result.commands.size(), 1u);
    if (result.commands.size() != 1)
        return;
    EXPECT_EQ(result.commands[0].type, type);
    EXPECT_EQ(result.commands[0].extend_selection, extend_selection);
}

TEST_CASE(native_paragraph_and_line_bindings)
{
    using Type = Web::TextEditingCommand::Type;
    expect_command('a', 0, Type::MoveToBeginningOfParagraph);
    expect_command('e', 14, Type::MoveToEndOfParagraph);
    expect_command('p', 35, Type::MoveUp);
    expect_command('n', 45, Type::MoveDown);
    expect_command('v', 9, Type::PageDown);
    expect_command('k', 40, Type::DeleteToEndOfParagraph);
    expect_command('h', 4, Type::DeleteBackward);
    expect_command('d', 2, Type::DeleteForward);
}

TEST_CASE(native_character_bindings_honor_user_overrides)
{
    using Type = Web::TextEditingCommand::Type;
    expect_command('b', 11, custom_bindings() ? Type::MoveWordBackward : Type::MoveBackward);
    expect_command('f', 3, custom_bindings() ? Type::MoveWordForward : Type::MoveForward);
}

TEST_CASE(repeated_native_keys_preserve_bindings)
{
    using Type = Web::TextEditingCommand::Type;
    auto result = interpret('b', 11, NSEventModifierFlagControl, NSEventTypeKeyDown, true);
    EXPECT(result.binding_resolved);
    EXPECT_EQ(result.commands.size(), 1u);
    if (result.commands.size() == 1)
        EXPECT_EQ(result.commands[0].type, custom_bindings() ? Type::MoveWordBackward : Type::MoveBackward);
}

TEST_CASE(native_selection_and_option_bindings)
{
    using Type = Web::TextEditingCommand::Type;
    auto shift = NSEventModifierFlagControl | NSEventModifierFlagShift;
    expect_command('a', 0, Type::MoveToBeginningOfParagraph, true, shift);
    expect_command('e', 14, Type::MoveToEndOfParagraph, true, shift);
    expect_command('b', 11, Type::MoveBackward, true, shift);
    expect_command('f', 3, Type::MoveForward, true, shift);
    expect_command('p', 35, Type::MoveUp, true, shift);
    expect_command('n', 45, Type::MoveDown, true, shift);
    expect_command('v', 9, Type::PageDown, true, shift);
    auto option = NSEventModifierFlagControl | NSEventModifierFlagOption;
    expect_command('b', 11, Type::MoveWordBackward, false, option);
    expect_command('f', 3, Type::MoveWordForward, false, option);
    expect_command('b', 11, Type::MoveWordBackward, true, option | NSEventModifierFlagShift);
    expect_command('f', 3, Type::MoveWordForward, true, option | NSEventModifierFlagShift);
}

TEST_CASE(custom_shortcuts_are_interpreted_by_appkit)
{
    using Type = Web::TextEditingCommand::Type;
    if (custom_bindings()) {
        expect_command('w', 13, Type::DeleteWordBackward);
        expect_command('g', 5, Type::DeleteWordBackward);
        expect_command('s', 1, Type::MoveForward, true);
        auto sequence = interpret('z', 6);
        EXPECT(sequence.binding_resolved);
        EXPECT_EQ(sequence.commands.size(), 2u);
        if (sequence.commands.size() == 2) {
            EXPECT_EQ(sequence.commands[0].type, Type::MoveToBeginningOfParagraph);
            EXPECT_EQ(sequence.commands[1].type, Type::MoveForward);
        }
        auto unsupported_sequence = interpret('u', 32);
        EXPECT(unsupported_sequence.binding_resolved);
        EXPECT(unsupported_sequence.commands.is_empty());
    } else {
        auto result = interpret('w', 13);
        EXPECT(result.binding_resolved);
        EXPECT(result.commands.is_empty());
    }
}

TEST_CASE(unsupported_commands_do_not_become_partial_edits)
{
    auto result = interpret('t', 17);
    EXPECT(result.binding_resolved);
    EXPECT(result.commands.is_empty());
}

TEST_CASE(non_control_keys_and_keyups_are_not_interpreted)
{
    EXPECT(!interpret('a', 0, 0).binding_resolved);
    EXPECT(!interpret('a', 0, NSEventModifierFlagControl | NSEventModifierFlagCommand).binding_resolved);
    EXPECT(!interpret('a', 0, NSEventModifierFlagControl, NSEventTypeKeyUp).binding_resolved);
}

TEST_CASE(synthetic_qt_keys_do_not_borrow_the_current_native_event)
{
    Ladybird::NativeTextEditingBindings bindings;
    QKeyEvent key { QEvent::KeyPress, Qt::Key_A, Qt::MetaModifier };
    auto result = bindings.commands_for_key_event(key);
    EXPECT(!result.binding_resolved);
    EXPECT(result.commands.is_empty());
}

TEST_CASE(test_home_is_isolated_before_appkit_initializes)
{
    auto* expected = getenv("CFFIXED_USER_HOME");
    EXPECT(expected);
    EXPECT(expected && strcmp(NSHomeDirectory().UTF8String, expected) == 0);
}
