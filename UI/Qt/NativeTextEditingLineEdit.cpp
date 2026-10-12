/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <QKeyEvent>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <UI/Qt/NativeTextEditingLineEdit.h>

#ifdef AK_OS_MACOS
#    include <UI/Qt/MacWindow.h>
#endif

namespace Ladybird {

NativeTextEditingLineEdit::NativeTextEditingLineEdit(QWidget* parent)
    : QLineEdit(parent)
{
}

void NativeTextEditingLineEdit::focusInEvent(QFocusEvent* event)
{
#ifdef AK_OS_MACOS
    make_appkit_window_first_responder(*this);
#endif
    QLineEdit::focusInEvent(event);
}

void NativeTextEditingLineEdit::keyPressEvent(QKeyEvent* event)
{
    if (!handle_native_text_editing_key(*event))
        QLineEdit::keyPressEvent(event);
}

bool NativeTextEditingLineEdit::handle_native_text_editing_key(QKeyEvent& event)
{
#ifdef AK_OS_MACOS
    auto binding = m_native_text_editing_bindings.commands_for_key_event(event);
    if (!binding.binding_resolved)
        return false;
    for (auto const& command : binding.commands) {
        native_text_editing_command_will_execute(command);
        apply_text_editing_command(command);
    }
    event.accept();
    return true;
#else
    (void)event;
    return false;
#endif
}

void NativeTextEditingLineEdit::apply_text_editing_command(Web::TextEditingCommand const& command)
{
    if (!isEnabled())
        return;
    auto extend = command.extend_selection;
    auto saved_move_style = cursorMoveStyle();
    setCursorMoveStyle(Qt::LogicalMoveStyle);
    using Type = Web::TextEditingCommand::Type;
    switch (command.type) {
    case Type::MoveBackward:
        cursorBackward(extend);
        break;
    case Type::MoveForward:
        cursorForward(extend);
        break;
    case Type::MoveWordBackward:
        cursorWordBackward(extend);
        break;
    case Type::MoveWordForward:
        cursorWordForward(extend);
        break;
    case Type::MoveUp:
    case Type::MoveToBeginningOfParagraph:
    case Type::MoveToBeginningOfLine:
    case Type::PageUp:
        home(extend);
        break;
    case Type::MoveDown:
    case Type::MoveToEndOfParagraph:
    case Type::MoveToEndOfLine:
    case Type::PageDown:
        end(extend);
        break;
    case Type::MoveToLeftEndOfLine:
    case Type::MoveToRightEndOfLine: {
        QTextLayout layout { displayText(), font() };
        QTextOption option;
        option.setTextDirection(layoutDirection());
        option.setFlags(QTextOption::IncludeTrailingSpaces);
        layout.setTextOption(option);
        layout.beginLayout();
        auto line = layout.createLine();
        layout.endLayout();
        auto position = line.isValid() ? line.xToCursor(command.type == Type::MoveToLeftEndOfLine ? -1 : line.horizontalAdvance() + 1) : 0;
        if (extend) {
            auto anchor = hasSelectedText() ? (cursorPosition() == selectionStart() ? selectionEnd() : selectionStart()) : cursorPosition();
            setSelection(anchor, position - anchor);
        } else {
            setCursorPosition(position);
        }
        break;
    }
    case Type::DeleteBackward:
    case Type::DeleteForward:
    case Type::DeleteWordBackward:
    case Type::DeleteWordForward:
    case Type::DeleteToEndOfParagraph:
        if (isReadOnly())
            break;
        if (!hasSelectedText()) {
            if (command.type == Type::DeleteWordBackward) {
                cursorWordBackward(true);
            } else if (command.type == Type::DeleteWordForward) {
                cursorWordForward(true);
            } else if (command.type == Type::DeleteToEndOfParagraph) {
                end(true);
            } else {
                QTextBoundaryFinder finder { QTextBoundaryFinder::Grapheme, text() };
                auto cursor = cursorPosition();
                finder.setPosition(cursor);
                auto boundary = command.type == Type::DeleteBackward ? finder.toPreviousBoundary() : finder.toNextBoundary();
                if (boundary >= 0)
                    setSelection(cursor, boundary - cursor);
            }
        }
        if (hasSelectedText())
            del();
        break;
    }
    setCursorMoveStyle(saved_move_style);
}

}
