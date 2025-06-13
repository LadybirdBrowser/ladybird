/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EditingHostManager.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/CommandNames.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/UIEvents/InputTypes.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(EditingHostManager);

GC::Ref<EditingHostManager> EditingHostManager::create(JS::Realm& realm, GC::Ref<Document> document)
{
    return realm.create<EditingHostManager>(document);
}

EditingHostManager::EditingHostManager(GC::Ref<Document> document)
    : m_document(document)
{
}

void EditingHostManager::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_active_contenteditable_element);
}

void EditingHostManager::handle_insert(String const& value)
{
    // https://w3c.github.io/editing/docs/execCommand/#additional-requirements
    // When the user instructs the user agent to insert text inside an editing host, such as by typing on the keyboard
    // while the cursor is in an editable node, the user agent must call execCommand("inserttext", false, value) on the
    // relevant document, with value equal to the text the user provided. If the user inserts multiple characters at
    // once or in quick succession, this specification does not define whether it is treated as one insertion or several
    // consecutive insertions.

    auto editing_result = m_document->exec_command(Editing::CommandNames::insertText, false, value);
    if (editing_result.is_exception())
        dbgln("handle_insert(): editing resulted in exception: {}", editing_result.exception());
}

void EditingHostManager::select_all()
{
    if (!m_active_contenteditable_element) {
        return;
    }
    auto selection = m_document->get_selection();
    if (!selection->anchor_node() || !selection->focus_node()) {
        return;
    }
    MUST(selection->set_base_and_extent(*selection->anchor_node(), 0, *selection->focus_node(), selection->focus_node()->length()));
}

void EditingHostManager::set_selection_anchor(GC::Ref<DOM::Node> anchor_node, size_t anchor_offset)
{
    auto selection = m_document->get_selection();
    MUST(selection->collapse(*anchor_node, anchor_offset));
    m_document->reset_cursor_blink_cycle();
}

void EditingHostManager::set_selection_focus(GC::Ref<DOM::Node> focus_node, size_t focus_offset)
{
    if (!m_active_contenteditable_element || !m_active_contenteditable_element->is_ancestor_of(*focus_node))
        return;
    auto selection = m_document->get_selection();
    if (!selection->anchor_node())
        return;
    MUST(selection->set_base_and_extent(*selection->anchor_node(), selection->anchor_offset(), *focus_node, focus_offset));
    m_document->reset_cursor_blink_cycle();
}

void EditingHostManager::move_cursor_to_start(CollapseSelection collapse)
{
    auto selection = m_document->get_selection();
    auto node = selection->anchor_node();
    if (!node || !is<DOM::Text>(*node))
        return;

    if (collapse == CollapseSelection::Yes) {
        MUST(selection->collapse(node, 0));
        m_document->reset_cursor_blink_cycle();
        return;
    }
    MUST(selection->set_base_and_extent(*node, selection->anchor_offset(), *node, 0));
}

void EditingHostManager::move_cursor_to_end(CollapseSelection collapse)
{
    auto selection = m_document->get_selection();
    auto node = selection->anchor_node();
    if (!node || !is<DOM::Text>(*node))
        return;

    if (collapse == CollapseSelection::Yes) {
        m_document->reset_cursor_blink_cycle();
        MUST(selection->collapse(node, node->length()));
        return;
    }
    MUST(selection->set_base_and_extent(*node, selection->anchor_offset(), *node, node->length()));
}

void EditingHostManager::increment_cursor_position_offset(CollapseSelection collapse)
{
    auto selection = m_document->get_selection();
    if (!selection)
        return;
    selection->move_offset_to_next_character(collapse == CollapseSelection::Yes);
}

void EditingHostManager::decrement_cursor_position_offset(CollapseSelection collapse)
{
    auto selection = m_document->get_selection();
    if (!selection)
        return;
    selection->move_offset_to_previous_character(collapse == CollapseSelection::Yes);
}

void EditingHostManager::increment_cursor_position_to_next_word(CollapseSelection collapse)
{
    auto selection = m_document->get_selection();
    if (!selection)
        return;
    selection->move_offset_to_next_character(collapse == CollapseSelection::Yes);
}

void EditingHostManager::decrement_cursor_position_to_previous_word(CollapseSelection collapse)
{
    auto selection = m_document->get_selection();
    if (!selection)
        return;
    selection->move_offset_to_previous_word(collapse == CollapseSelection::Yes);
}

void EditingHostManager::handle_delete(DeleteDirection direction)
{
    // https://w3c.github.io/editing/docs/execCommand/#additional-requirements
    // When the user instructs the user agent to delete the previous character inside an editing host, such as by
    // pressing the Backspace key while the cursor is in an editable node, the user agent must call
    // execCommand("delete") on the relevant document.
    // When the user instructs the user agent to delete the next character inside an editing host, such as by pressing
    // the Delete key while the cursor is in an editable node, the user agent must call execCommand("forwarddelete") on
    // the relevant document.

    auto editing_result = [&] {
        if (direction == DeleteDirection::Backward)
            return m_document->exec_command(Editing::CommandNames::delete_, false, {});
        if (direction == DeleteDirection::Forward)
            return m_document->exec_command(Editing::CommandNames::forwardDelete, false, {});
        VERIFY_NOT_REACHED();
    }();

    if (editing_result.is_exception())
        dbgln("handle_delete(): editing resulted in exception: {}", editing_result.exception());
}

EventResult EditingHostManager::handle_return_key(FlyString const& ui_input_type)
{
    // https://w3c.github.io/editing/docs/execCommand/#additional-requirements
    // When the user instructs the user agent to insert a line break inside an editing host, such as by pressing the
    // Enter key while the cursor is in an editable node, the user agent must call execCommand("insertparagraph") on the
    // relevant document.
    // When the user instructs the user agent to insert a line break inside an editing host without breaking out of the
    // current block, such as by pressing Shift-Enter or Option-Enter while the cursor is in an editable node, the user
    // agent must call execCommand("insertlinebreak") on the relevant document.
    auto editing_result = [&] {
        if (ui_input_type == UIEvents::InputTypes::insertParagraph)
            return m_document->exec_command(Editing::CommandNames::insertParagraph, false, {});
        if (ui_input_type == UIEvents::InputTypes::insertLineBreak)
            return m_document->exec_command(Editing::CommandNames::insertLineBreak, false, {});
        VERIFY_NOT_REACHED();
    }();

    if (editing_result.is_exception()) {
        dbgln("handle_return_key(): editing resulted in exception: {}", editing_result.exception());
        return EventResult::Dropped;
    }

    return editing_result.value() ? EventResult::Handled : EventResult::Dropped;
}

}
