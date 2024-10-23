/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/DOM/EditingHostManager.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::DOM {

JS_DEFINE_ALLOCATOR(EditingHostManager);

void EditingHostManager::handle_insert(String const& data)
{
    auto selection = m_document->get_selection();

    auto selection_range = selection->range();
    if (!selection_range) {
        return;
    }

    auto node = selection->anchor_node();
    if (!node || !node->is_editable()) {
        return;
    }

    if (!is<DOM::Text>(*node)) {
        auto& realm = node->realm();
        auto text = realm.heap().allocate<DOM::Text>(realm, node->document(), data);
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

void EditingHostManager::set_selection_anchor(JS::NonnullGCPtr<DOM::Node> anchor_node, size_t anchor_offset)
{
    auto selection = m_document->get_selection();
    MUST(selection->collapse(*anchor_node, anchor_offset));
    m_document->reset_cursor_blink_cycle();
}

void EditingHostManager::set_selection_focus(JS::NonnullGCPtr<DOM::Node> focus_node, size_t focus_offset)
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
    auto node = selection->anchor_node();
    if (!node || !is<DOM::Text>(*node))
        return;

    auto& text_node = static_cast<DOM::Text&>(*node);
    if (auto offset = text_node.grapheme_segmenter().next_boundary(selection->focus_offset()); offset.has_value()) {
        if (collapse == CollapseSelection::Yes) {
            MUST(selection->collapse(*node, *offset));
            m_document->reset_cursor_blink_cycle();
        } else {
            MUST(selection->set_base_and_extent(*node, selection->anchor_offset(), *node, *offset));
        }
    }
}

void EditingHostManager::decrement_cursor_position_offset(CollapseSelection collapse)
{
    auto selection = m_document->get_selection();
    auto node = selection->anchor_node();
    if (!node || !is<DOM::Text>(*node)) {
        return;
    }

    auto& text_node = static_cast<DOM::Text&>(*node);
    if (auto offset = text_node.grapheme_segmenter().previous_boundary(selection->focus_offset()); offset.has_value()) {
        if (collapse == CollapseSelection::Yes) {
            MUST(selection->collapse(*node, *offset));
            m_document->reset_cursor_blink_cycle();
        } else {
            MUST(selection->set_base_and_extent(*node, selection->anchor_offset(), *node, *offset));
        }
    }
}

static bool should_continue_beyond_word(Utf8View const& word)
{
    for (auto code_point : word) {
        if (!Unicode::code_point_has_punctuation_general_category(code_point) && !Unicode::code_point_has_separator_general_category(code_point))
            return false;
    }

    return true;
}

void EditingHostManager::increment_cursor_position_to_next_word(CollapseSelection collapse)
{
    auto selection = m_document->get_selection();
    auto node = selection->anchor_node();
    if (!node || !is<DOM::Text>(*node)) {
        return;
    }

    auto& text_node = static_cast<DOM::Text&>(*node);

    while (true) {
        auto focus_offset = selection->focus_offset();
        if (focus_offset == text_node.data().bytes_as_string_view().length()) {
            return;
        }

        if (auto offset = text_node.word_segmenter().next_boundary(focus_offset); offset.has_value()) {
            auto word = text_node.data().code_points().substring_view(focus_offset, *offset - focus_offset);
            if (collapse == CollapseSelection::Yes) {
                MUST(selection->collapse(node, *offset));
                m_document->reset_cursor_blink_cycle();
            } else {
                MUST(selection->set_base_and_extent(*node, selection->anchor_offset(), *node, *offset));
            }
            if (should_continue_beyond_word(word))
                continue;
        }
        break;
    }
}

void EditingHostManager::decrement_cursor_position_to_previous_word(CollapseSelection collapse)
{
    auto selection = m_document->get_selection();
    auto node = selection->anchor_node();
    if (!node || !is<DOM::Text>(*node)) {
        return;
    }

    auto& text_node = static_cast<DOM::Text&>(*node);

    while (true) {
        auto focus_offset = selection->focus_offset();
        if (auto offset = text_node.word_segmenter().previous_boundary(focus_offset); offset.has_value()) {
            auto word = text_node.data().code_points().substring_view(focus_offset, focus_offset - *offset);
            if (collapse == CollapseSelection::Yes) {
                MUST(selection->collapse(node, *offset));
                m_document->reset_cursor_blink_cycle();
            } else {
                MUST(selection->set_base_and_extent(*node, selection->anchor_offset(), *node, *offset));
            }
            if (should_continue_beyond_word(word))
                continue;
        }
        break;
    }
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

void EditingHostManager::handle_return_key()
{
    dbgln("FIXME: Implement EditingHostManager::handle_return_key()");
}

}
