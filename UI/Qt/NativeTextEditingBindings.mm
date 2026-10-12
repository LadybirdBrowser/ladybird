/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/NativeTextEditingBindings.h>

#include <AK/Optional.h>
#include <AK/ScopeGuard.h>

#include <QKeyEvent>

#import <AppKit/AppKit.h>

namespace Ladybird {

static Optional<Web::TextEditingCommand> command_for_selector(SEL selector)
{
    using Type = Web::TextEditingCommand::Type;
    struct SelectorCommand {
        SEL selector;
        SEL selection_selector;
        Type type;
    };
    static SelectorCommand const commands[] = {
        { @selector(moveBackward:), @selector(moveBackwardAndModifySelection:), Type::MoveBackward },
        { @selector(moveForward:), @selector(moveForwardAndModifySelection:), Type::MoveForward },
        { @selector(moveWordBackward:), @selector(moveWordBackwardAndModifySelection:), Type::MoveWordBackward },
        { @selector(moveWordForward:), @selector(moveWordForwardAndModifySelection:), Type::MoveWordForward },
        { @selector(moveUp:), @selector(moveUpAndModifySelection:), Type::MoveUp },
        { @selector(moveDown:), @selector(moveDownAndModifySelection:), Type::MoveDown },
        { @selector(moveToBeginningOfParagraph:), @selector(moveToBeginningOfParagraphAndModifySelection:), Type::MoveToBeginningOfParagraph },
        { @selector(moveToEndOfParagraph:), @selector(moveToEndOfParagraphAndModifySelection:), Type::MoveToEndOfParagraph },
        { @selector(moveToBeginningOfLine:), @selector(moveToBeginningOfLineAndModifySelection:), Type::MoveToBeginningOfLine },
        { @selector(moveToEndOfLine:), @selector(moveToEndOfLineAndModifySelection:), Type::MoveToEndOfLine },
        { @selector(moveToLeftEndOfLine:), @selector(moveToLeftEndOfLineAndModifySelection:), Type::MoveToLeftEndOfLine },
        { @selector(moveToRightEndOfLine:), @selector(moveToRightEndOfLineAndModifySelection:), Type::MoveToRightEndOfLine },
        { @selector(pageUp:), @selector(pageUpAndModifySelection:), Type::PageUp },
        { @selector(pageDown:), @selector(pageDownAndModifySelection:), Type::PageDown },
        { @selector(deleteBackward:), nullptr, Type::DeleteBackward },
        { @selector(deleteForward:), nullptr, Type::DeleteForward },
        { @selector(deleteWordBackward:), nullptr, Type::DeleteWordBackward },
        { @selector(deleteWordForward:), nullptr, Type::DeleteWordForward },
        { @selector(deleteToEndOfParagraph:), nullptr, Type::DeleteToEndOfParagraph },
    };
    for (auto const& command : commands) {
        if (selector == command.selector)
            return Web::TextEditingCommand { command.type, false };
        if (command.selection_selector && selector == command.selection_selector)
            return Web::TextEditingCommand { command.type, true };
    }
    return {};
}

struct NativeTextEditingRecording {
    NativeTextEditingBindingResult result;
    bool unsupported_command { false };
};

}

@interface LadybirdNativeTextEditingRecorder : NSResponder
{
    Ladybird::NativeTextEditingRecording* m_recording;
}
- (Ladybird::NativeTextEditingBindingResult)commandsForEvent:(NSEvent*)event;
@end

@implementation LadybirdNativeTextEditingRecorder
- (Ladybird::NativeTextEditingBindingResult)commandsForEvent:(NSEvent*)event
{
    if (event.type != NSEventTypeKeyDown || !(event.modifierFlags & NSEventModifierFlagControl) || (event.modifierFlags & NSEventModifierFlagCommand))
        return {};

    Ladybird::NativeTextEditingRecording recording;
    auto* previous_recording = m_recording;
    m_recording = &recording;
    ScopeGuard restore_recording = [&] { m_recording = previous_recording; };
    [self interpretKeyEvents:@[ event ]];
    if (recording.unsupported_command)
        recording.result.commands.clear();
    return move(recording.result);
}

- (void)doCommandBySelector:(SEL)selector
{
    if (!m_recording)
        return;
    m_recording->result.binding_resolved = true;
    if (auto command = Ladybird::command_for_selector(selector); command.has_value())
        m_recording->result.commands.append(*command);
    else
        m_recording->unsupported_command = true;
}

- (void)insertText:(id)text
{
    (void)text;
    if (m_recording) {
        m_recording->result.binding_resolved = true;
        m_recording->unsupported_command = true;
    }
}

@end

namespace Ladybird {

NativeTextEditingBindings::NativeTextEditingBindings()
    : m_recorder([LadybirdNativeTextEditingRecorder new])
{
}

NativeTextEditingBindings::~NativeTextEditingBindings()
{
    [static_cast<LadybirdNativeTextEditingRecorder*>(m_recorder) release];
}

NativeTextEditingBindingResult NativeTextEditingBindings::commands_for_key_event(QKeyEvent const& event)
{
    if (event.type() != QEvent::KeyPress || !(event.nativeModifiers() & NSEventModifierFlagControl) || (event.nativeModifiers() & NSEventModifierFlagCommand))
        return {};

    auto* native_event = NSApp.currentEvent;
    if (!native_event || native_event.type != NSEventTypeKeyDown
        || native_event.keyCode != event.nativeVirtualKey()
        || native_event.modifierFlags != event.nativeModifiers()
        || native_event.isARepeat != event.isAutoRepeat()
        || static_cast<quint64>(native_event.timestamp * 1000) != event.timestamp())
        return {};

    return [static_cast<LadybirdNativeTextEditingRecorder*>(m_recorder) commandsForEvent:native_event];
}

}
