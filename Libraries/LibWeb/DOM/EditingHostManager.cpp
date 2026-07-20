/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025-2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EditingHostManager.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/ClipboardSanitizer.h>
#include <LibWeb/Editing/CommandNames.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/Selection/SelectionModifier.h>
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

void EditingHostManager::handle_insert(Utf16FlyString const& input_type, Utf16View value)
{
    // https://w3c.github.io/editing/docs/execCommand/#additional-requirements
    // When the user instructs the user agent to insert text inside an editing host, such as by typing on the keyboard
    // while the cursor is in an editable node, the user agent must call execCommand("inserttext", false, value) on the
    // relevant document, with value equal to the text the user provided. If the user inserts multiple characters at
    // once or in quick succession, this specification does not define whether it is treated as one insertion or several
    // consecutive insertions.
    //
    // NB: The user's input type is passed along so pastes fire an input event with inputType "insertFromPaste" and
    //     form their own undo unit, even though they run the insertText command.
    auto editing_result = m_document->exec_command_internal(Editing::CommandNames::insertText, false, value, Document::DispatchInputEvent::Yes, input_type);
    if (editing_result.is_exception())
        dbgln("handle_insert(): editing resulted in exception: {}", editing_result.exception());
}

void EditingHostManager::handle_insert_from_clipboard(Utf16FlyString const& input_type, Utf16View plain_text, Optional<Utf16View> html)
{
    auto range = m_document->get_selection()->range();
    auto editing_host = range ? range->start_container()->editing_host() : nullptr;
    bool accepts_rich_text = !editing_host || !is<HTML::HTMLElement>(*editing_host)
        || as<HTML::HTMLElement>(*editing_host).content_editable_state() != HTML::ContentEditableState::PlaintextOnly;

    if (!range || !html.has_value() || !accepts_rich_text) {
        handle_insert(input_type, plain_text);
        return;
    }

    auto sanitized_html = Editing::sanitize_clipboard_html(*range, *html);
    if (sanitized_html.is_exception() || sanitized_html.value().is_empty()) {
        handle_insert(input_type, plain_text);
        return;
    }

    // INTEROP: Rich editing hosts prefer text/html over text/plain when both clipboard representations are present.
    //          Run insertHTML as one user edit so its DOM mutations and final selection form one undo unit.
    auto editing_result = m_document->exec_command_internal(Editing::CommandNames::insertHTML, false, sanitized_html.value(), Document::DispatchInputEvent::Yes, input_type);
    if (editing_result.is_exception())
        dbgln("handle_insert_from_clipboard(): editing resulted in exception: {}", editing_result.exception());
}

void EditingHostManager::select_all()
{
    if (!m_active_contenteditable_element)
        return;
    auto selection = m_document->get_selection();
    Selection::SelectionModifier(*selection).select_all();
}

void EditingHostManager::set_selection_anchor(GC::Ref<DOM::Node> anchor_node, size_t anchor_offset, TextAffinity affinity)
{
    auto selection = m_document->get_selection();
    MUST(selection->collapse(*anchor_node, anchor_offset));
    selection->set_focus_affinity(affinity);
    m_document->reset_cursor_blink_cycle();
}

void EditingHostManager::set_selection_focus(GC::Ref<DOM::Node> focus_node, size_t focus_offset, TextAffinity affinity)
{
    if (!m_active_contenteditable_element || !m_active_contenteditable_element->is_ancestor_of(*focus_node))
        return;
    auto selection = m_document->get_selection();
    if (!selection->anchor_node())
        return;
    MUST(selection->set_base_and_extent(*selection->anchor_node(), selection->anchor_offset(), *focus_node, focus_offset));
    selection->set_focus_affinity(affinity);
    m_document->reset_cursor_blink_cycle();
}

GC::Ptr<Selection::Selection> EditingHostManager::get_selection_for_navigation(CollapseSelection collapse) const
{
    // In order for navigation to happen inside an editing host, the document must have a selection,
    auto selection = m_document->get_selection();
    if (!selection)
        return {};

    // and the focus node must be a text node or an element directly housing the caret (e.g. an empty line),
    auto focus_node = selection->focus_node();
    if (!focus_node || (!is<Text>(*focus_node) && !is<Element>(*focus_node)))
        return {};

    // and if we're performing collapsed navigation (i.e. moving the caret), the focus node must be editable.
    if (collapse == CollapseSelection::Yes && !focus_node->is_editable_or_editing_host())
        return {};

    return selection;
}

void EditingHostManager::move_cursor_to_start(CollapseSelection collapse)
{
    auto selection = get_selection_for_navigation(collapse);
    if (!selection)
        return;
    Selection::SelectionModifier(*selection).modify(collapse == CollapseSelection::Yes ? Selection::SelectionAlteration::Move : Selection::SelectionAlteration::Extend, Selection::SelectionDirection::Backward, Selection::SelectionGranularity::LineBoundary);
}

void EditingHostManager::move_cursor_to_end(CollapseSelection collapse)
{
    auto selection = get_selection_for_navigation(collapse);
    if (!selection)
        return;
    Selection::SelectionModifier(*selection).modify(collapse == CollapseSelection::Yes ? Selection::SelectionAlteration::Move : Selection::SelectionAlteration::Extend, Selection::SelectionDirection::Forward, Selection::SelectionGranularity::LineBoundary);
}

void EditingHostManager::move_cursor_to_start_of_document(CollapseSelection collapse)
{
    auto selection = get_selection_for_navigation(collapse);
    if (!selection)
        return;
    Selection::SelectionModifier(*selection).modify(collapse == CollapseSelection::Yes ? Selection::SelectionAlteration::Move : Selection::SelectionAlteration::Extend, Selection::SelectionDirection::Backward, Selection::SelectionGranularity::DocumentBoundary);
}

void EditingHostManager::move_cursor_to_end_of_document(CollapseSelection collapse)
{
    auto selection = get_selection_for_navigation(collapse);
    if (!selection)
        return;
    Selection::SelectionModifier(*selection).modify(collapse == CollapseSelection::Yes ? Selection::SelectionAlteration::Move : Selection::SelectionAlteration::Extend, Selection::SelectionDirection::Forward, Selection::SelectionGranularity::DocumentBoundary);
}

void EditingHostManager::move_cursor_to_previous_page(CollapseSelection collapse)
{
    auto selection = get_selection_for_navigation(collapse);
    if (!selection)
        return;
    Selection::SelectionModifier(*selection).modify(collapse == CollapseSelection::Yes ? Selection::SelectionAlteration::Move : Selection::SelectionAlteration::Extend, Selection::SelectionDirection::Backward, Selection::SelectionGranularity::Page);
}

void EditingHostManager::move_cursor_to_next_page(CollapseSelection collapse)
{
    auto selection = get_selection_for_navigation(collapse);
    if (!selection)
        return;
    Selection::SelectionModifier(*selection).modify(collapse == CollapseSelection::Yes ? Selection::SelectionAlteration::Move : Selection::SelectionAlteration::Extend, Selection::SelectionDirection::Forward, Selection::SelectionGranularity::Page);
}

void EditingHostManager::increment_cursor_position_offset(CollapseSelection collapse)
{
    auto selection = get_selection_for_navigation(collapse);
    if (!selection)
        return;
    selection->move_offset_to_next_character(collapse == CollapseSelection::Yes);
}

void EditingHostManager::decrement_cursor_position_offset(CollapseSelection collapse)
{
    auto selection = get_selection_for_navigation(collapse);
    if (!selection)
        return;
    selection->move_offset_to_previous_character(collapse == CollapseSelection::Yes);
}

void EditingHostManager::increment_cursor_position_to_next_word(CollapseSelection collapse)
{
    auto selection = get_selection_for_navigation(collapse);
    if (!selection)
        return;
    selection->move_offset_to_next_word(collapse == CollapseSelection::Yes);
}

void EditingHostManager::decrement_cursor_position_to_previous_word(CollapseSelection collapse)
{
    auto selection = get_selection_for_navigation(collapse);
    if (!selection)
        return;
    selection->move_offset_to_previous_word(collapse == CollapseSelection::Yes);
}

void EditingHostManager::increment_cursor_position_to_next_line(CollapseSelection collapse)
{
    if (auto selection = m_document->get_selection())
        selection->move_offset_to_next_line(collapse == CollapseSelection::Yes);
}

void EditingHostManager::decrement_cursor_position_to_previous_line(CollapseSelection collapse)
{
    if (auto selection = m_document->get_selection())
        selection->move_offset_to_previous_line(collapse == CollapseSelection::Yes);
}

void EditingHostManager::handle_delete(Utf16FlyString const& input_type, [[maybe_unused]] DispatchInputEvent dispatch_input_event)
{
    // https://w3c.github.io/editing/docs/execCommand/#additional-requirements
    // When the user instructs the user agent to delete the previous character inside an editing host, such as by
    // pressing the Backspace key while the cursor is in an editable node, the user agent must call
    // execCommand("delete") on the relevant document.
    // When the user instructs the user agent to delete the next character inside an editing host, such as by pressing
    // the Delete key while the cursor is in an editable node, the user agent must call execCommand("forwarddelete") on
    // the relevant document.
    //
    // NB: A cut deletes the selection like Backspace does, and passes its input type along so the input event fires
    //     with inputType "deleteByCut" and the cut forms its own undo unit.
    auto command = input_type == UIEvents::InputTypes::deleteContentForward ? Editing::CommandNames::forwardDelete : Editing::CommandNames::delete_;
    auto editing_result = m_document->exec_command_internal(command, false, {}, Document::DispatchInputEvent::Yes, input_type);
    if (editing_result.is_exception())
        dbgln("handle_delete(): editing resulted in exception: {}", editing_result.exception());
}

EventResult EditingHostManager::handle_return_key(Utf16FlyString const& ui_input_type)
{
    VERIFY(ui_input_type == UIEvents::InputTypes::insertParagraph || ui_input_type == UIEvents::InputTypes::insertLineBreak);

    // https://w3c.github.io/editing/docs/execCommand/#additional-requirements
    // When the user instructs the user agent to insert a line break inside an editing host, such as by pressing the
    // Enter key while the cursor is in an editable node, the user agent must call execCommand("insertparagraph") on the
    // relevant document.
    // When the user instructs the user agent to insert a line break inside an editing host without breaking out of the
    // current block, such as by pressing Shift-Enter or Option-Enter while the cursor is in an editable node, the user
    // agent must call execCommand("insertlinebreak") on the relevant document.
    auto command = ui_input_type == UIEvents::InputTypes::insertParagraph
        ? Editing::CommandNames::insertParagraph
        : Editing::CommandNames::insertLineBreak;
    auto editing_result = m_document->exec_command(command, false, {});
    if (editing_result.is_exception()) {
        dbgln("handle_return_key(): editing resulted in exception: {}", editing_result.exception());
        return EventResult::Dropped;
    }
    return editing_result.value() ? EventResult::Handled : EventResult::Dropped;
}

bool EditingHostManager::is_within_active_contenteditable(Node const& node) const
{
    if (!m_active_contenteditable_element)
        return false;
    Node const* active = m_active_contenteditable_element.ptr();
    return node.find_in_shadow_including_ancestry([&](Node const& it) { return &it == active; });
}

}
