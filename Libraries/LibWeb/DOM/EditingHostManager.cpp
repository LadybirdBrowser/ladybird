/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EditingHostManager.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Selection/Selection.h>

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

void EditingHostManager::handle_insert(String const& data)
{
    auto selection = m_document->get_selection();

    auto selection_range = selection->range();
    if (!selection_range)
        return;

    auto node = selection->anchor_node();
    if (!node || !node->is_editable_or_editing_host())
        return;

    if (!is<DOM::Text>(*node)) {
        auto& realm = node->realm();
        auto text = realm.create<DOM::Text>(node->document(), data);
        MUST(node->append_child(*text));
        MUST(selection->collapse(*text, 1));
        return;
    }

    auto& text_node = static_cast<DOM::Text&>(*node);

    MUST(selection_range->delete_contents());
    MUST(text_node.insert_data(selection->anchor_offset(), data));
    VERIFY(selection->is_collapsed());

    auto utf16_data = MUST(AK::utf8_to_utf16(data));
    Utf16View const utf16_view { utf16_data };
    auto length = utf16_view.length_in_code_units();
    MUST(selection->collapse(*node, selection->anchor_offset() + length));

    text_node.invalidate_style(DOM::StyleInvalidationReason::EditingInsertion);
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
    auto selection = m_document->get_selection();
    auto selection_range = selection->range();
    if (!selection_range) {
        return;
    }

    if (selection->is_collapsed()) {
        auto node = selection->anchor_node();
        if (!node || !is<DOM::Text>(*node)) {
            return;
        }

        auto& text_node = static_cast<DOM::Text&>(*node);
        if (direction == DeleteDirection::Backward) {
            if (selection->anchor_offset() > 0) {
                MUST(text_node.delete_data(selection->anchor_offset() - 1, 1));
                text_node.invalidate_style(DOM::StyleInvalidationReason::EditingInsertion);
            }
        } else {
            if (selection->anchor_offset() < text_node.data().bytes_as_string_view().length()) {
                MUST(text_node.delete_data(selection->anchor_offset(), 1));
                text_node.invalidate_style(DOM::StyleInvalidationReason::EditingInsertion);
            }
        }
        m_document->reset_cursor_blink_cycle();
        return;
    }

    MUST(selection_range->delete_contents());
}

EventResult EditingHostManager::handle_return_key()
{
    dbgln("FIXME: Implement EditingHostManager::handle_return_key()");
    return EventResult::Dropped;
}

}
