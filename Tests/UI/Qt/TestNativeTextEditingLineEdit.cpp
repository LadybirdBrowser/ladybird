/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/NativeTextEditingLineEdit.h>

using Type = Web::TextEditingCommand::Type;

class TestLineEdit : public Ladybird::NativeTextEditingLineEdit {
public:
    using NativeTextEditingLineEdit::apply_text_editing_command;
};

TEST_CASE(word_deletion_is_single_undoable_edit)
{
    TestLineEdit edit;
    edit.setText("hello world");
    edit.setCursorPosition(11);
    edit.apply_text_editing_command({ Type::DeleteWordBackward });
    EXPECT(edit.text() == "hello ");
    EXPECT_EQ(edit.cursorPosition(), 6);
    edit.undo();
    EXPECT(edit.text() == "hello world");
    edit.redo();
    EXPECT(edit.text() == "hello ");
}

TEST_CASE(paragraph_deletion_deletes_selection_directly)
{
    TestLineEdit edit;
    edit.setText("abcdef");
    edit.setSelection(1, 3);
    edit.apply_text_editing_command({ Type::DeleteToEndOfParagraph });
    EXPECT(edit.text() == "aef");
    EXPECT_EQ(edit.cursorPosition(), 1);
    edit.undo();
    EXPECT(edit.text() == "abcdef");
}

TEST_CASE(selector_selection_preserves_anchor)
{
    TestLineEdit edit;
    edit.setText("abcdef");
    edit.setCursorPosition(3);
    edit.apply_text_editing_command({ Type::MoveToBeginningOfParagraph, true });
    EXPECT(edit.selectedText() == "abc");
    EXPECT_EQ(edit.cursorPosition(), 0);
    edit.apply_text_editing_command({ Type::MoveToEndOfParagraph, true });
    EXPECT(edit.selectedText() == "def");
    EXPECT_EQ(edit.cursorPosition(), 6);
}

TEST_CASE(character_deletion_preserves_graphemes)
{
    TestLineEdit edit;
    edit.setText(QString::fromUtf8("a\xF0\x9F\x98\x80"));
    edit.setCursorPosition(3);
    edit.apply_text_editing_command({ Type::DeleteBackward });
    EXPECT(edit.text() == "a");
    EXPECT_EQ(edit.cursorPosition(), 1);
}

TEST_CASE(read_only_navigation_does_not_delete)
{
    TestLineEdit edit;
    edit.setText("abcdef");
    edit.setCursorPosition(3);
    edit.setReadOnly(true);
    edit.apply_text_editing_command({ Type::MoveToBeginningOfParagraph });
    EXPECT_EQ(edit.cursorPosition(), 0);
    edit.apply_text_editing_command({ Type::DeleteToEndOfParagraph });
    EXPECT(edit.text() == "abcdef");
}

TEST_CASE(no_op_deletion_keeps_history_empty)
{
    TestLineEdit edit;
    edit.setText("abc");
    edit.setCursorPosition(3);
    edit.apply_text_editing_command({ Type::DeleteToEndOfParagraph });
    EXPECT(!edit.isUndoAvailable());
    edit.setCursorPosition(0);
    edit.apply_text_editing_command({ Type::DeleteWordBackward });
    EXPECT(!edit.isUndoAvailable());
}
