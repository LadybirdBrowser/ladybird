/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/DocumentType.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/CommandNames.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLLIElement.h>
#include <LibWeb/HTML/HTMLOListElement.h>
#include <LibWeb/HTML/HTMLTableCellElement.h>
#include <LibWeb/HTML/HTMLTableRowElement.h>
#include <LibWeb/HTML/HTMLTableSectionElement.h>
#include <LibWeb/HTML/HTMLUListElement.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Layout/BreakNode.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Painting/TextPaintable.h>

namespace Web::Editing {

// https://w3c.github.io/editing/docs/execCommand/#block-extend
GC::Ref<DOM::Range> block_extend_a_range(DOM::Range& range)
{
    // 1. Let start node, start offset, end node, and end offset be the start and end nodes and offsets of range.
    GC::Ptr<DOM::Node> start_node = range.start_container();
    auto start_offset = range.start_offset();
    GC::Ptr<DOM::Node> end_node = range.end_container();
    auto end_offset = range.end_offset();

    // 2. If some inclusive ancestor of start node is an li, set start offset to the index of the last such li in tree
    //    order, and set start node to that li's parent.
    auto ancestor = start_node;
    while (ancestor) {
        if (is<HTML::HTMLLIElement>(*ancestor)) {
            start_offset = ancestor->index();
            start_node = ancestor->parent();
            break;
        }
        ancestor = ancestor->parent();
    }

    // 3. If (start node, start offset) is not a block start point, repeat the following steps:
    if (!is_block_start_point(*start_node, start_offset)) {
        do {
            // 1. If start offset is zero, set it to start node's index, then set start node to its parent.
            if (start_offset == 0) {
                start_offset = start_node->index();
                start_node = start_node->parent();
            }

            // 2. Otherwise, subtract one from start offset.
            else {
                --start_offset;
            }

            // 3. If (start node, start offset) is a block boundary point, break from this loop.
        } while (!is_block_boundary_point(*start_node, start_offset));
    }

    // 4. While start offset is zero and start node's parent is not null, set start offset to start node's index, then
    //    set start node to its parent.
    while (start_offset == 0 && start_node->parent()) {
        start_offset = start_node->index();
        start_node = start_node->parent();
    }

    // 5. If some inclusive ancestor of end node is an li, set end offset to one plus the index of the last such li in
    //    tree order, and set end node to that li's parent.
    ancestor = end_node;
    while (ancestor) {
        if (is<HTML::HTMLLIElement>(*ancestor)) {
            end_offset = ancestor->index() + 1;
            end_node = ancestor->parent();
            break;
        }
        ancestor = ancestor->parent();
    }

    // 6. If (end node, end offset) is not a block end point, repeat the following steps:
    if (!is_block_end_point(*end_node, end_offset)) {
        do {
            // 1. If end offset is end node's length, set it to one plus end node's index, then set end node to its
            //    parent.
            if (end_offset == end_node->length()) {
                end_offset = end_node->index() + 1;
                end_node = end_node->parent();
            }

            // 2. Otherwise, add one to end offset.
            else {
                ++end_offset;
            }

            // 3. If (end node, end offset) is a block boundary point, break from this loop.
        } while (!is_block_boundary_point(*end_node, end_offset));
    }

    // 7. While end offset is end node's length and end node's parent is not null, set end offset to one plus end node's
    //    index, then set end node to its parent.
    while (end_offset == end_node->length() && end_node->parent()) {
        end_offset = end_node->index() + 1;
        end_node = end_node->parent();
    }

    // 8. Let new range be a new range whose start and end nodes and offsets are start node, start offset, end node, and
    //    end offset.
    auto new_range = DOM::Range::create(*start_node, start_offset, *end_node, end_offset);

    // 9. Return new range.
    return new_range;
}

// https://w3c.github.io/editing/docs/execCommand/#block-node-of
GC::Ptr<DOM::Node> block_node_of_node(GC::Ref<DOM::Node> input_node)
{
    // 1. While node is an inline node, set node to its parent.
    GC::Ptr<DOM::Node> node = input_node;
    while (node && is_inline_node(*node))
        node = node->parent();

    // 2. Return node.
    return node;
}

// https://w3c.github.io/editing/docs/execCommand/#canonical-space-sequence
String canonical_space_sequence(u32 length, bool non_breaking_start, bool non_breaking_end)
{
    auto n = length;

    // 1. If n is zero, return the empty string.
    if (n == 0)
        return {};

    // 2. If n is one and both non-breaking start and non-breaking end are false, return a single
    //    space (U+0020).
    if (n == 1 && !non_breaking_start && !non_breaking_end)
        return " "_string;

    // 3. If n is one, return a single non-breaking space (U+00A0).
    if (n == 1)
        return "\u00A0"_string;

    // 4. Let buffer be the empty string.
    StringBuilder buffer;

    // 5. If non-breaking start is true, let repeated pair be U+00A0 U+0020. Otherwise, let it be
    //    U+0020 U+00A0.
    auto repeated_pair = non_breaking_start ? "\u00A0 "sv : " \u00A0"sv;

    // 6. While n is greater than three, append repeated pair to buffer and subtract two from n.
    while (n > 3) {
        buffer.append(repeated_pair);
        n -= 2;
    }

    // 7. If n is three, append a three-code unit string to buffer depending on non-breaking start
    //    and non-breaking end:
    if (n == 3) {
        // non-breaking start and non-breaking end false
        // U+0020 U+00A0 U+0020
        if (!non_breaking_start && !non_breaking_end)
            buffer.append(" \u00A0 "sv);

        // non-breaking start true, non-breaking end false
        // U+00A0 U+00A0 U+0020
        else if (non_breaking_start && !non_breaking_end)
            buffer.append("\u00A0\u00A0 "sv);

        // non-breaking start false, non-breaking end true
        // U+0020 U+00A0 U+00A0
        else if (!non_breaking_start)
            buffer.append(" \u00A0\u00A0"sv);

        // non-breaking start and non-breaking end both true
        // U+00A0 U+0020 U+00A0
        else
            buffer.append("\u00A0 \u00A0"sv);
    }

    // 8. Otherwise, append a two-code unit string to buffer depending on non-breaking start and
    //    non-breaking end:
    else {
        // non-breaking start and non-breaking end false
        // non-breaking start true, non-breaking end false
        // U+00A0 U+0020
        if (!non_breaking_start && !non_breaking_end)
            buffer.append("\u00A0 "sv);

        // non-breaking start false, non-breaking end true
        // U+0020 U+00A0
        else if (!non_breaking_start)
            buffer.append(" \u00A0"sv);

        // non-breaking start and non-breaking end both true
        // U+00A0 U+00A0
        else
            buffer.append("\u00A0\u00A0"sv);
    }

    // 9. Return buffer.
    return MUST(buffer.to_string());
}

// https://w3c.github.io/editing/docs/execCommand/#canonicalize-whitespace
void canonicalize_whitespace(GC::Ref<DOM::Node> node, u32 offset, bool fix_collapsed_space)
{
    // 1. If node is neither editable nor an editing host, abort these steps.
    if (!node->is_editable_or_editing_host())
        return;

    // 2. Let start node equal node and let start offset equal offset.
    auto start_node = node;
    auto start_offset = offset;

    // 3. Repeat the following steps:
    while (true) {
        // 1. If start node has a child in the same editing host with index start offset minus one,
        //    set start node to that child, then set start offset to start node's length.
        auto* offset_minus_one_child = start_node->child_at_index(start_offset - 1);
        if (offset_minus_one_child && is_in_same_editing_host(*start_node, *offset_minus_one_child)) {
            start_node = *offset_minus_one_child;
            start_offset = start_node->length();
            continue;
        }

        // 2. Otherwise, if start offset is zero and start node does not follow a line break and
        //    start node's parent is in the same editing host, set start offset to start node's
        //    index, then set start node to its parent.
        if (start_offset == 0 && !follows_a_line_break(start_node) && is_in_same_editing_host(*start_node, *start_node->parent())) {
            start_offset = start_node->index();
            start_node = *start_node->parent();
            continue;
        }

        // 3. Otherwise, if start node is a Text node and its parent's resolved value for
        //    "white-space" is neither "pre" nor "pre-wrap" and start offset is not zero and the
        //    (start offset − 1)st code unit of start node's data is a space (0x0020) or
        //    non-breaking space (0x00A0), subtract one from start offset.
        auto* layout_node = start_node->parent()->layout_node();
        if (layout_node && is<DOM::Text>(*start_node) && start_offset != 0) {
            auto parent_white_space = layout_node->computed_values().white_space();

            // FIXME: Find a way to get code points directly from the UTF-8 string
            auto start_node_data = *start_node->text_content();
            auto utf16_code_units = MUST(AK::utf8_to_utf16(start_node_data));
            auto offset_minus_one_code_point = Utf16View { utf16_code_units }.code_point_at(start_offset - 1);
            if (parent_white_space != CSS::WhiteSpace::Pre && parent_white_space != CSS::WhiteSpace::PreWrap
                && (offset_minus_one_code_point == 0x20 || offset_minus_one_code_point == 0xA0)) {
                --start_offset;
                continue;
            }
        }

        // 4. Otherwise, break from this loop.
        break;
    }

    // 4. Let end node equal start node and end offset equal start offset.
    auto end_node = start_node;
    auto end_offset = start_offset;

    // 5. Let length equal zero.
    auto length = 0;

    // 6. Let collapse spaces be true if start offset is zero and start node follows a line break,
    //    otherwise false.
    auto collapse_spaces = start_offset == 0 && follows_a_line_break(start_node);

    // 7. Repeat the following steps:
    while (true) {
        // 1. If end node has a child in the same editing host with index end offset, set end node
        //    to that child, then set end offset to zero.
        auto* offset_child = end_node->child_at_index(end_offset);
        if (offset_child && is_in_same_editing_host(*end_node, *offset_child)) {
            end_node = *offset_child;
            end_offset = 0;
            continue;
        }

        // 2. Otherwise, if end offset is end node's length and end node does not precede a line
        //    break and end node's parent is in the same editing host, set end offset to one plus
        //    end node's index, then set end node to its parent.
        if (end_offset == end_node->length() && !precedes_a_line_break(end_node) && is_in_same_editing_host(*end_node, *end_node->parent())) {
            end_offset = end_node->index() + 1;
            end_node = *end_node->parent();
            continue;
        }

        // 3. Otherwise, if end node is a Text node and its parent's resolved value for
        //    "white-space" is neither "pre" nor "pre-wrap" and end offset is not end node's length
        //    and the end offsetth code unit of end node's data is a space (0x0020) or non-breaking
        //    space (0x00A0):
        auto* layout_node = end_node->parent()->layout_node();
        if (layout_node && is<DOM::Text>(*end_node) && end_offset != end_node->length()) {
            auto parent_white_space = layout_node->computed_values().white_space();

            // FIXME: Find a way to get code points directly from the UTF-8 string
            auto end_node_data = *end_node->text_content();
            auto utf16_code_units = MUST(AK::utf8_to_utf16(end_node_data));
            auto offset_code_point = Utf16View { utf16_code_units }.code_point_at(end_offset);
            if (parent_white_space != CSS::WhiteSpace::Pre && parent_white_space != CSS::WhiteSpace::PreWrap
                && (offset_code_point == 0x20 || offset_code_point == 0xA0)) {
                // 1. If fix collapsed space is true, and collapse spaces is true, and the end offsetth
                //    code unit of end node's data is a space (0x0020): call deleteData(end offset, 1)
                //    on end node, then continue this loop from the beginning.
                if (fix_collapsed_space && collapse_spaces && offset_code_point == 0x20) {
                    MUST(static_cast<DOM::CharacterData&>(*end_node).delete_data(end_offset, 1));
                    continue;
                }

                // 2. Set collapse spaces to true if the end offsetth code unit of end node's data is a
                //    space (0x0020), false otherwise.
                collapse_spaces = offset_code_point == 0x20;

                // 3. Add one to end offset.
                ++end_offset;

                // 4. Add one to length.
                ++length;

                // NOTE: We continue the loop here since we matched every condition from step 7.3
                continue;
            }
        }

        // 4. Otherwise, break from this loop.
        break;
    }

    // 8. If fix collapsed space is true, then while (start node, start offset) is before (end node,
    //    end offset):
    if (fix_collapsed_space) {
        while (true) {
            auto relative_position = position_of_boundary_point_relative_to_other_boundary_point(*start_node, start_offset, *end_node, end_offset);
            if (relative_position != DOM::RelativeBoundaryPointPosition::Before)
                break;

            // 1. If end node has a child in the same editing host with index end offset − 1, set end
            //    node to that child, then set end offset to end node's length.
            auto offset_minus_one_child = end_node->child_at_index(end_offset - 1);
            if (offset_minus_one_child && is_in_same_editing_host(end_node, *offset_minus_one_child)) {
                end_node = *offset_minus_one_child;
                end_offset = end_node->length();
                continue;
            }

            // 2. Otherwise, if end offset is zero and end node's parent is in the same editing host,
            //    set end offset to end node's index, then set end node to its parent.
            if (end_offset == 0 && is_in_same_editing_host(end_node, *end_node->parent())) {
                end_offset = end_node->index();
                end_node = *end_node->parent();
                continue;
            }

            // 3. Otherwise, if end node is a Text node and its parent's resolved value for
            //    "white-space" is neither "pre" nor "pre-wrap" and end offset is end node's length and
            //    the last code unit of end node's data is a space (0x0020) and end node precedes a line
            //    break:
            auto* layout_node = end_node->parent()->layout_node();
            if (layout_node && is<DOM::Text>(*end_node) && end_offset == end_node->length() && precedes_a_line_break(end_node)) {
                auto parent_white_space = layout_node->computed_values().white_space();
                if (parent_white_space != CSS::WhiteSpace::Pre && parent_white_space != CSS::WhiteSpace::PreWrap
                    && end_node->text_content().value().ends_with_bytes(" "sv)) {
                    // 1. Subtract one from end offset.
                    --end_offset;

                    // 2. Subtract one from length.
                    --length;

                    // 3. Call deleteData(end offset, 1) on end node.
                    MUST(static_cast<DOM::CharacterData&>(*end_node).delete_data(end_offset, 1));

                    // NOTE: We continue the loop here since we matched every condition from step 8.3
                    continue;
                }
            }

            // 4. Otherwise, break from this loop.
            break;
        }
    }

    // 9. Let replacement whitespace be the canonical space sequence of length length. non-breaking
    //    start is true if start offset is zero and start node follows a line break, and false
    //    otherwise. non-breaking end is true if end offset is end node's length and end node
    //    precedes a line break, and false otherwise.
    auto replacement_whitespace = canonical_space_sequence(
        length,
        start_offset == 0 && follows_a_line_break(start_node),
        end_offset == end_node->length() && precedes_a_line_break(end_node));

    // 10. While (start node, start offset) is before (end node, end offset):
    while (true) {
        auto relative_position = position_of_boundary_point_relative_to_other_boundary_point(start_node, start_offset, end_node, end_offset);
        if (relative_position != DOM::RelativeBoundaryPointPosition::Before)
            break;

        // 1. If start node has a child with index start offset, set start node to that child, then
        //    set start offset to zero.
        if (start_node->child_at_index(start_offset)) {
            start_node = *start_node->child_at_index(start_offset);
            start_offset = 0;
        }

        // 2. Otherwise, if start node is not a Text node or if start offset is start node's length,
        //    set start offset to one plus start node's index, then set start node to its parent.
        else if (!is<DOM::Text>(*start_node) || start_offset == start_node->length()) {
            start_offset = start_node->index() + 1;
            start_node = *start_node->parent();
        }

        // 3. Otherwise:
        else {
            // 1. Remove the first code unit from replacement whitespace, and let element be that
            //    code unit.
            // FIXME: Find a way to get code points directly from the UTF-8 string
            auto replacement_whitespace_utf16 = MUST(AK::utf8_to_utf16(replacement_whitespace));
            auto replacement_whitespace_utf16_view = Utf16View { replacement_whitespace_utf16 };
            replacement_whitespace = MUST(String::from_utf16({ replacement_whitespace_utf16_view.substring_view(1) }));
            auto element = replacement_whitespace_utf16_view.code_point_at(0);

            // 2. If element is not the same as the start offsetth code unit of start node's data:
            auto start_node_data = *start_node->text_content();
            auto start_node_utf16 = MUST(AK::utf8_to_utf16(start_node_data));
            auto start_node_utf16_view = Utf16View { start_node_utf16 };
            auto start_node_code_point = start_node_utf16_view.code_point_at(start_offset);
            if (element != start_node_code_point) {
                // 1. Call insertData(start offset, element) on start node.
                auto& start_node_character_data = static_cast<DOM::CharacterData&>(*start_node);
                MUST(start_node_character_data.insert_data(start_offset, String::from_code_point(element)));

                // 2. Call deleteData(start offset + 1, 1) on start node.
                MUST(start_node_character_data.delete_data(start_offset + 1, 1));
            }

            // 3. Add one to start offset.
            ++start_offset;
        }
    }
}

// https://w3c.github.io/editing/docs/execCommand/#delete-the-selection
void delete_the_selection(Selection& selection, bool block_merging, bool strip_wrappers, Selection::Direction direction)
{
    auto& document = *selection.document();

    // 1. If the active range is null, abort these steps and do nothing.
    // NOTE: The selection is collapsed often in this algorithm, so we shouldn't store the active range in a variable.
    auto active_range = [&selection] { return selection.range(); };
    if (!active_range())
        return;

    // 2. Canonicalize whitespace at the active range's start.
    canonicalize_whitespace(active_range()->start_container(), active_range()->start_offset());

    // 3. Canonicalize whitespace at the active range's end.
    canonicalize_whitespace(active_range()->end_container(), active_range()->end_offset());

    // 4. Let (start node, start offset) be the last equivalent point for the active range's start.
    auto start = last_equivalent_point({ active_range()->start_container(), active_range()->start_offset() });

    // 5. Let (end node, end offset) be the first equivalent point for the active range's end.
    auto end = first_equivalent_point({ active_range()->end_container(), active_range()->end_offset() });

    // 6. If (end node, end offset) is not after (start node, start offset):
    auto relative_position = position_of_boundary_point_relative_to_other_boundary_point(end.node, end.offset, start.node, start.offset);
    if (relative_position != DOM::RelativeBoundaryPointPosition::After) {
        // 1. If direction is "forward", call collapseToStart() on the context object's selection.
        if (direction == Selection::Direction::Forwards) {
            MUST(selection.collapse_to_start());
        }

        // 2. Otherwise, call collapseToEnd() on the context object's selection.
        else {
            MUST(selection.collapse_to_end());
        }

        // 3. Abort these steps.
        return;
    }

    // 7. If start node is a Text node and start offset is 0, set start offset to the index of start node, then set
    //    start node to its parent.
    if (is<DOM::Text>(*start.node) && start.offset == 0 && start.node->parent()) {
        start = {
            *start.node->parent(),
            static_cast<WebIDL::UnsignedLong>(start.node->index()),
        };
    }

    // 8. If end node is a Text node and end offset is its length, set end offset to one plus the index of end node,
    //    then set end node to its parent.
    if (is<DOM::Text>(*end.node) && end.offset == end.node->length() && end.node->parent()) {
        end = {
            *end.node->parent(),
            static_cast<WebIDL::UnsignedLong>(end.node->index() + 1),
        };
    }

    // 9. Call collapse(start node, start offset) on the context object's selection.
    MUST(selection.collapse(start.node, start.offset));

    // 10. Call extend(end node, end offset) on the context object's selection.
    MUST(selection.extend(end.node, end.offset));

    // 12. Let start block be the active range's start node.
    GC::Ptr<DOM::Node> start_block = active_range()->start_container();

    // 13. While start block's parent is in the same editing host and start block is an inline node, set start block to
    //     its parent.
    while (start_block->parent() && is_in_same_editing_host(*start_block->parent(), *start_block) && is_inline_node(*start_block))
        start_block = *start_block->parent();

    // 14. If start block is neither a block node nor an editing host, or "span" is not an allowed child of start block,
    //     or start block is a td or th, set start block to null.
    if ((!is_block_node(*start_block) && !start_block->is_editing_host())
        || !is_allowed_child_of_node(HTML::TagNames::span, GC::Ref { *start_block })
        || is<HTML::HTMLTableCellElement>(*start_block))
        start_block = {};

    // 15. Let end block be the active range's end node.
    GC::Ptr<DOM::Node> end_block = active_range()->end_container();

    // 16. While end block's parent is in the same editing host and end block is an inline node, set end block to its
    //     parent.
    while (end_block->parent() && is_in_same_editing_host(*end_block->parent(), *end_block) && is_inline_node(*end_block))
        end_block = end_block->parent();

    // 17. If end block is neither a block node nor an editing host, or "span" is not an allowed child of end block, or
    //     end block is a td or th, set end block to null.
    if ((!is_block_node(*end_block) && !end_block->is_editing_host())
        || !is_allowed_child_of_node(HTML::TagNames::span, GC::Ref { *end_block })
        || is<HTML::HTMLTableCellElement>(*end_block))
        end_block = {};

    // 19. Record current states and values, and let overrides be the result.
    auto overrides = record_current_states_and_values(*active_range());

    // 21. If start node and end node are the same, and start node is an editable Text node:
    if (start.node == end.node && is<DOM::Text>(*start.node) && start.node->is_editable()) {
        // 1. Call deleteData(start offset, end offset − start offset) on start node.
        MUST(static_cast<DOM::Text&>(*start.node).delete_data(start.offset, end.offset - start.offset));

        // 2. Canonicalize whitespace at (start node, start offset), with fix collapsed space false.
        canonicalize_whitespace(start.node, start.offset, false);

        // 3. If direction is "forward", call collapseToStart() on the context object's selection.
        if (direction == Selection::Direction::Forwards) {
            MUST(selection.collapse_to_start());
        }

        // 4. Otherwise, call collapseToEnd() on the context object's selection.
        else {
            MUST(selection.collapse_to_end());
        }

        // 5. Restore states and values from overrides.
        restore_states_and_values(*selection.range(), overrides);

        // 6. Abort these steps.
        return;
    }

    // 22. If start node is an editable Text node, call deleteData() on it, with start offset as the first argument and
    //     (length of start node − start offset) as the second argument.
    if (is<DOM::Text>(*start.node) && start.node->is_editable())
        MUST(static_cast<DOM::Text&>(*start.node).delete_data(start.offset, start.node->length() - start.offset));

    // 23. Let node list be a list of nodes, initially empty.
    Vector<GC::Ref<DOM::Node>> node_list;

    // 24. For each node contained in the active range, append node to node list if the last member of node list (if
    //     any) is not an ancestor of node; node is editable; and node is not a thead, tbody, tfoot, tr, th, or td.
    auto common_ancestor = active_range()->common_ancestor_container();
    common_ancestor->for_each_in_subtree([&](GC::Ref<DOM::Node> node) {
        if (!active_range()->contains_node(node))
            return TraversalDecision::SkipChildrenAndContinue;

        if (!node_list.is_empty() && node_list.last()->is_ancestor_of(node))
            return TraversalDecision::SkipChildrenAndContinue;

        if (!node->is_editable())
            return TraversalDecision::Continue;

        if (!is<HTML::HTMLTableSectionElement>(*node) && !is<HTML::HTMLTableRowElement>(*node) && !is<HTML::HTMLTableCellElement>(*node))
            node_list.append(node);

        return TraversalDecision::Continue;
    });

    // 25. For each node in node list:
    for (auto node : node_list) {
        // 1. Let parent be the parent of node.
        // NOTE: All nodes in node_list are descendants of common_ancestor and as such, always have a parent.
        GC::Ptr<DOM::Node> parent = *node->parent();

        // 2. Remove node from parent.
        node->remove();

        // 3. If the block node of parent has no visible children, and parent is editable or an editing host, call
        //    createElement("br") on the context object and append the result as the last child of parent.
        auto block_node_of_parent = block_node_of_node(*parent);
        if (block_node_of_parent && !has_visible_children(*block_node_of_parent) && parent->is_editable_or_editing_host())
            MUST(parent->append_child(MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML))));

        // 4. If strip wrappers is true or parent is not an inclusive ancestor of start node, while parent is an
        //    editable inline node with length 0, let grandparent be the parent of parent, then remove parent from
        //    grandparent, then set parent to grandparent.
        if (strip_wrappers || !parent->is_inclusive_ancestor_of(start.node)) {
            while (parent->parent() && parent->is_editable() && is_inline_node(*parent) && parent->length() == 0) {
                auto grandparent = parent->parent();
                parent->remove();
                parent = grandparent;
            }
        }
    }

    // 26. If end node is an editable Text node, call deleteData(0, end offset) on it.
    if (end.node->is_editable() && is<DOM::Text>(*end.node))
        MUST(static_cast<DOM::Text&>(*end.node).delete_data(0, end.offset));

    // 27. Canonicalize whitespace at the active range's start, with fix collapsed space false.
    canonicalize_whitespace(active_range()->start_container(), active_range()->start_offset(), false);

    // 28. Canonicalize whitespace at the active range's end, with fix collapsed space false.
    canonicalize_whitespace(active_range()->end_container(), active_range()->end_offset(), false);

    // 30. If block merging is false, or start block or end block is null, or start block is not in the same editing
    //     host as end block, or start block and end block are the same:
    if (!block_merging || !start_block || !end_block || !is_in_same_editing_host(*start_block, *end_block) || start_block == end_block) {
        // 1. If direction is "forward", call collapseToStart() on the context object's selection.
        if (direction == Selection::Direction::Forwards) {
            MUST(selection.collapse_to_start());
        }

        // 2. Otherwise, call collapseToEnd() on the context object's selection.
        else {
            MUST(selection.collapse_to_end());
        }

        // 3. Restore states and values from overrides.
        restore_states_and_values(*selection.range(), overrides);

        // 4. Abort these steps.
        return;
    }

    // 31. If start block has one child, which is a collapsed block prop, remove its child from it.
    if (start_block->child_count() == 1 && is_collapsed_block_prop(*start_block->first_child()))
        start_block->first_child()->remove();

    // 32. If start block is an ancestor of end block:
    Vector<RecordedNodeValue> values;
    if (start_block->is_ancestor_of(*end_block)) {
        // 1. Let reference node be end block.
        auto reference_node = end_block;

        // 2. While reference node is not a child of start block, set reference node to its parent.
        while (reference_node->parent() && reference_node->parent() != start_block.ptr())
            reference_node = reference_node->parent();

        // 3. Call collapse() on the context object's selection, with first argument start block and second argument the
        //    index of reference node.
        MUST(selection.collapse(start_block, reference_node->index()));

        // 4. If end block has no children:
        if (!end_block->has_children()) {
            // 1. While end block is editable and is the only child of its parent and is not a child of start block, let
            //    parent equal end block, then remove end block from parent, then set end block to parent.
            while (end_block->parent() && end_block->is_editable() && end_block->parent()->child_count() == 1 && end_block->parent() != start_block.ptr()) {
                // AD-HOC: Set end_block's parent instead of end_block itself.
                //         See: https://github.com/w3c/editing/issues/473
                auto parent = end_block->parent();
                end_block->remove();
                end_block = parent;
            }

            // 2. If end block is editable and is not an inline node, and its previousSibling and nextSibling are both
            //    inline nodes, call createElement("br") on the context object and insert it into end block's parent
            //    immediately after end block.
            if (end_block->is_editable() && !is_inline_node(*end_block) && end_block->previous_sibling() && end_block->next_sibling()
                && is_inline_node(*end_block->previous_sibling()) && is_inline_node(*end_block->next_sibling())) {
                auto br = MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML));
                end_block->parent()->insert_before(br, end_block->next_sibling());
            }

            // 3. If end block is editable, remove it from its parent.
            if (end_block->is_editable())
                end_block->remove();

            // 4. Restore states and values from overrides.
            restore_states_and_values(*active_range(), overrides);

            // 5. Abort these steps.
            return;
        }

        // 5. If end block's firstChild is not an inline node, restore states and values from record, then abort these
        //    steps.
        if (!is_inline_node(*end_block->first_child())) {
            restore_states_and_values(*active_range(), overrides);
            return;
        }

        // 6. Let children be a list of nodes, initially empty.
        Vector<GC::Ref<DOM::Node>> children;

        // 7. Append the first child of end block to children.
        children.append(*end_block->first_child());

        // 8. While children's last member is not a br, and children's last member's nextSibling is an inline node,
        //    append children's last member's nextSibling to children.
        while (!is<HTML::HTMLBRElement>(*children.last()) && children.last()->next_sibling()) {
            GC::Ref<DOM::Node> next_sibling = *children.last()->next_sibling();
            if (!is_inline_node(next_sibling))
                break;
            children.append(next_sibling);
        }

        // 9. Record the values of children, and let values be the result.
        values = record_the_values_of_nodes(children);

        // 10. While children's first member's parent is not start block, split the parent of children.
        while (children.first()->parent() != start_block)
            split_the_parent_of_nodes(children);

        // 11. If children's first member's previousSibling is an editable br, remove that br from its parent.
        if (is<HTML::HTMLBRElement>(children.first()->previous_sibling()) && children.first()->previous_sibling()->is_editable())
            children.first()->previous_sibling()->remove();
    }

    // 33. Otherwise, if start block is a descendant of end block:
    else if (start_block->is_descendant_of(*end_block)) {
        // 1. Call collapse() on the context object's selection, with first argument start block and second argument
        //    start block's length.
        MUST(selection.collapse(start_block, start_block->length()));

        // 2. Let reference node be start block.
        auto reference_node = start_block;

        // 3. While reference node is not a child of end block, set reference node to its parent.
        while (reference_node->parent() && reference_node->parent() != end_block)
            reference_node = reference_node->parent();

        // 4. If reference node's nextSibling is an inline node and start block's lastChild is a br, remove start
        //    block's lastChild from it.
        if (reference_node->next_sibling() && is_inline_node(*reference_node->next_sibling())
            && is<HTML::HTMLBRElement>(start_block->last_child()))
            start_block->last_child()->remove();

        // 5. Let nodes to move be a list of nodes, initially empty.
        Vector<GC::Ref<DOM::Node>> nodes_to_move;

        // 6. If reference node's nextSibling is neither null nor a block node, append it to nodes to move.
        if (reference_node->next_sibling() && !is_block_node(*reference_node->next_sibling()))
            nodes_to_move.append(*reference_node->next_sibling());

        // 7. While nodes to move is nonempty and its last member isn't a br and its last member's nextSibling is
        //    neither null nor a block node, append its last member's nextSibling to nodes to move.
        while (!nodes_to_move.is_empty() && !is<HTML::HTMLBRElement>(*nodes_to_move.last())
            && nodes_to_move.last()->next_sibling() && !is_block_node(*nodes_to_move.last()->next_sibling()))
            nodes_to_move.append(*nodes_to_move.last()->next_sibling());

        // 8. Record the values of nodes to move, and let values be the result.
        values = record_the_values_of_nodes(nodes_to_move);

        // 9. For each node in nodes to move, append node as the last child of start block, preserving ranges.
        auto new_position = start_block->length();
        for (auto node : nodes_to_move)
            move_node_preserving_ranges(node, *start_block, new_position++);
    }

    // 34. Otherwise:
    else {
        // 1. Call collapse() on the context object's selection, with first argument start block and second argument
        //    start block's length.
        MUST(selection.collapse(start_block, start_block->length()));

        // 2. If end block's firstChild is an inline node and start block's lastChild is a br, remove start block's
        //    lastChild from it.
        if (end_block->first_child() && is_inline_node(*end_block->first_child())
            && start_block->last_child() && is<HTML::HTMLBRElement>(*start_block->last_child()))
            start_block->last_child()->remove();

        // 3. Record the values of end block's children, and let values be the result.
        Vector<GC::Ref<DOM::Node>> end_block_children;
        end_block_children.ensure_capacity(end_block->child_count());
        end_block->for_each_child([&end_block_children](auto& child) {
            end_block_children.append(child);
            return IterationDecision::Continue;
        });
        values = record_the_values_of_nodes(end_block_children);

        // 4. While end block has children, append the first child of end block to start block, preserving ranges.
        auto new_position = start_block->length();
        while (end_block->has_children())
            move_node_preserving_ranges(*end_block->first_child(), *start_block, new_position++);

        // 5. While end block has no children, let parent be the parent of end block, then remove end block from parent,
        //    then set end block to parent.
        while (end_block->parent() && !end_block->has_children()) {
            GC::Ptr<DOM::Node> parent = end_block->parent();
            end_block->remove();
            end_block = parent;
        }
    }

    // 36. Let ancestor be start block.
    auto ancestor = start_block;

    // 37. While ancestor has an inclusive ancestor ol in the same editing host whose nextSibling is also an ol in the
    //     same editing host, or an inclusive ancestor ul in the same editing host whose nextSibling is also a ul in the
    //     same editing host:
    while (true) {
        auto inclusive_ancestor = ancestor;
        bool has_valid_ol_or_ul_ancestor = false;
        while (inclusive_ancestor) {
            if (inclusive_ancestor->next_sibling() && is_in_same_editing_host(*ancestor, *inclusive_ancestor)
                && is_in_same_editing_host(*inclusive_ancestor, *inclusive_ancestor->next_sibling())
                && ((is<HTML::HTMLOListElement>(*inclusive_ancestor) && is<HTML::HTMLOListElement>(*inclusive_ancestor->next_sibling()))
                    || (is<HTML::HTMLUListElement>(*inclusive_ancestor) && is<HTML::HTMLUListElement>(*inclusive_ancestor->next_sibling())))) {
                has_valid_ol_or_ul_ancestor = true;
                break;
            }
            inclusive_ancestor = inclusive_ancestor->parent();
        }
        if (!has_valid_ol_or_ul_ancestor)
            break;

        // 1. While ancestor and its nextSibling are not both ols in the same editing host, and are also not both uls in
        //    the same editing host, set ancestor to its parent.
        while (ancestor->parent()) {
            if (ancestor->next_sibling() && is_in_same_editing_host(*ancestor, *ancestor->next_sibling())) {
                if (is<HTML::HTMLOListElement>(*ancestor) && is<HTML::HTMLOListElement>(*ancestor->next_sibling()))
                    break;
                if (is<HTML::HTMLUListElement>(*ancestor) && is<HTML::HTMLUListElement>(*ancestor->next_sibling()))
                    break;
            }
            ancestor = ancestor->parent();
        }

        // 2. While ancestor's nextSibling has children, append ancestor's nextSibling's firstChild as the last child of
        //    ancestor, preserving ranges.
        auto new_position = ancestor->length();
        while (ancestor->next_sibling()->has_children())
            move_node_preserving_ranges(*ancestor->next_sibling()->first_child(), *ancestor, new_position++);

        // 3. Remove ancestor's nextSibling from its parent.
        ancestor->next_sibling()->remove();
    }

    // 38. Restore the values from values.
    restore_the_values_of_nodes(values);

    // 39. If start block has no children, call createElement("br") on the context object and append the result as the
    //     last child of start block.
    if (!start_block->has_children())
        MUST(start_block->append_child(MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML))));

    // 40. Remove extraneous line breaks at the end of start block.
    remove_extraneous_line_breaks_at_the_end_of_node(*start_block);

    // 41. Restore states and values from overrides.
    restore_states_and_values(*active_range(), overrides);
}

// https://w3c.github.io/editing/docs/execCommand/#editing-host-of
GC::Ptr<DOM::Node> editing_host_of_node(GC::Ref<DOM::Node> node)
{
    // node itself, if node is an editing host;
    if (node->is_editing_host())
        return node;

    // or the nearest ancestor of node that is an editing host, if node is editable.
    if (node->is_editable()) {
        auto* ancestor = node->parent();
        while (ancestor) {
            if (ancestor->is_editing_host())
                return ancestor;
            ancestor = ancestor->parent();
        }
        VERIFY_NOT_REACHED();
    }

    // The editing host of node is null if node is neither editable nor an editing host;
    return {};
}

// https://w3c.github.io/editing/docs/execCommand/#first-equivalent-point
BoundaryPoint first_equivalent_point(BoundaryPoint boundary_point)
{
    // 1. While (node, offset)'s previous equivalent point is not null, set (node, offset) to its previous equivalent
    //    point.
    while (true) {
        auto previous_point = previous_equivalent_point(boundary_point);
        if (!previous_point.has_value())
            break;
        boundary_point = previous_point.release_value();
    }

    // 2. Return (node, offset).
    return boundary_point;
}

// https://w3c.github.io/editing/docs/execCommand/#fix-disallowed-ancestors
void fix_disallowed_ancestors_of_node(GC::Ref<DOM::Node> node)
{
    // 1. If node is not editable, abort these steps.
    if (!node->is_editable())
        return;

    // 2. If node is not an allowed child of any of its ancestors in the same editing host:
    bool allowed_child_of_any_ancestor = false;
    GC::Ptr<DOM::Node> ancestor = node->parent();
    while (ancestor) {
        if (is_in_same_editing_host(*ancestor, *node) && is_allowed_child_of_node(GC::Ref { *node }, GC::Ref { *ancestor })) {
            allowed_child_of_any_ancestor = true;
            break;
        }
        ancestor = ancestor->parent();
    }
    if (!allowed_child_of_any_ancestor) {
        // 1. If node is a dd or dt, wrap the one-node list consisting of node, with sibling criteria returning true for
        //    any dl with no attributes and false otherwise, and new parent instructions returning the result of calling
        //    createElement("dl") on the context object. Then abort these steps.
        if (is<DOM::Element>(*node) && static_cast<DOM::Element&>(*node).local_name().is_one_of(HTML::TagNames::dd, HTML::TagNames::dt)) {
            wrap(
                { node },
                [](GC::Ref<DOM::Node> sibling) {
                    if (!is<DOM::Element>(*sibling))
                        return false;
                    auto& sibling_element = static_cast<DOM::Element&>(*sibling);
                    return sibling_element.local_name() == HTML::TagNames::dl && !sibling_element.has_attributes();
                },
                [&node] { return MUST(DOM::create_element(node->document(), HTML::TagNames::dl, Namespace::HTML)); });
            return;
        }

        // 2. If "p" is not an allowed child of the editing host of node, abort these steps.
        if (!is_allowed_child_of_node(HTML::TagNames::p, GC::Ref { *editing_host_of_node(*node) }))
            return;

        // 3. If node is not a prohibited paragraph child, abort these steps.
        if (!is_prohibited_paragraph_child(*node))
            return;

        // 4. Set the tag name of node to the default single-line container name, and let node be the result.
        node = set_the_tag_name(static_cast<DOM::Element&>(*node), node->document().default_single_line_container_name());

        // 5. Fix disallowed ancestors of node.
        fix_disallowed_ancestors_of_node(node);

        // 6. Let children be node's children.
        // 7. For each child in children, if child is a prohibited paragraph child:
        node->for_each_child([](DOM::Node& child) {
            if (!is_prohibited_paragraph_child(child))
                return IterationDecision::Continue;

            // 1. Record the values of the one-node list consisting of child, and let values be the result.
            auto values = record_the_values_of_nodes({ child });

            // 2. Split the parent of the one-node list consisting of child.
            split_the_parent_of_nodes({ child });

            // 3. Restore the values from values.
            restore_the_values_of_nodes(values);

            return IterationDecision::Continue;
        });

        // 8. Abort these steps.
        return;
    }

    // 3. Record the values of the one-node list consisting of node, and let values be the result.
    auto values = record_the_values_of_nodes({ *node });

    // 4. While node is not an allowed child of its parent, split the parent of the one-node list consisting of node.
    while (!is_allowed_child_of_node(GC::Ref { *node }, GC::Ref { *node->parent() }))
        split_the_parent_of_nodes({ *node });

    // 5. Restore the values from values.
    restore_the_values_of_nodes(values);
}

// https://w3c.github.io/editing/docs/execCommand/#follows-a-line-break
bool follows_a_line_break(GC::Ref<DOM::Node> node)
{
    // 1. Let offset be zero.
    auto offset = 0;

    // 2. While (node, offset) is not a block boundary point:
    while (!is_block_boundary_point(node, offset)) {
        // 1. If node has a visible child with index offset minus one, return false.
        auto* offset_minus_one_child = node->child_at_index(offset - 1);
        if (offset_minus_one_child && is_visible_node(*offset_minus_one_child))
            return false;

        // 2. If offset is zero or node has no children, set offset to node's index, then set node
        //    to its parent.
        if (offset == 0 || node->child_count() == 0) {
            offset = node->index();
            node = *node->parent();
        }

        // 3. Otherwise, set node to its child with index offset minus one, then set offset to
        //    node's length.
        else {
            node = *node->child_at_index(offset - 1);
            offset = node->length();
        }
    }

    // 3. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#allowed-child
bool is_allowed_child_of_node(Variant<GC::Ref<DOM::Node>, FlyString> child, Variant<GC::Ref<DOM::Node>, FlyString> parent)
{
    GC::Ptr<DOM::Node> child_node;
    if (child.has<GC::Ref<DOM::Node>>())
        child_node = child.get<GC::Ref<DOM::Node>>();

    GC::Ptr<DOM::Node> parent_node;
    if (parent.has<GC::Ref<DOM::Node>>())
        parent_node = parent.get<GC::Ref<DOM::Node>>();

    if (parent.has<FlyString>() || is<DOM::Element>(parent_node.ptr())) {
        auto parent_local_name = parent.visit(
            [](FlyString local_name) { return local_name; },
            [](GC::Ref<DOM::Node> node) { return static_cast<DOM::Element&>(*node).local_name(); });

        // 1. If parent is "colgroup", "table", "tbody", "tfoot", "thead", "tr", or an HTML element with local name equal to
        //    one of those, and child is a Text node whose data does not consist solely of space characters, return false.
        auto parent_is_table_like = parent_local_name.is_one_of(HTML::TagNames::colgroup, HTML::TagNames::table,
            HTML::TagNames::tbody, HTML::TagNames::tfoot, HTML::TagNames::thead, HTML::TagNames::tr);
        if (parent_is_table_like && is<DOM::Text>(child_node.ptr())) {
            auto child_text_content = child_node->text_content().release_value();
            if (!all_of(child_text_content.bytes_as_string_view(), Infra::is_ascii_whitespace))
                return false;
        }

        // 2. If parent is "script", "style", "plaintext", or "xmp", or an HTML element with local name equal to one of
        //    those, and child is not a Text node, return false.
        if ((child.has<FlyString>() || !is<DOM::Text>(child_node.ptr()))
            && parent_local_name.is_one_of(HTML::TagNames::script, HTML::TagNames::style, HTML::TagNames::plaintext, HTML::TagNames::xmp))
            return false;
    }

    // 3. If child is a document, DocumentFragment, or DocumentType, return false.
    if (is<DOM::Document>(child_node.ptr()) || is<DOM::DocumentFragment>(child_node.ptr()) || is<DOM::DocumentType>(child_node.ptr()))
        return false;

    // 4. If child is an HTML element, set child to the local name of child.
    if (is<HTML::HTMLElement>(child_node.ptr()))
        child = static_cast<DOM::Element&>(*child_node).local_name();

    // 5. If child is not a string, return true.
    if (!child.has<FlyString>())
        return true;
    auto child_local_name = child.get<FlyString>();

    // 6. If parent is an HTML element:
    if (is<HTML::HTMLElement>(parent_node.ptr())) {
        auto& parent_html_element = static_cast<HTML::HTMLElement&>(*parent.get<GC::Ref<DOM::Node>>());

        // 1. If child is "a", and parent or some ancestor of parent is an a, return false.
        if (child_local_name == HTML::TagNames::a) {
            DOM::Node* ancestor = &parent_html_element;
            while (ancestor) {
                if (is<HTML::HTMLAnchorElement>(*ancestor))
                    return false;
                ancestor = ancestor->parent();
            }
        }

        // 2. If child is a prohibited paragraph child name and parent or some ancestor of parent is an element with
        //    inline contents, return false.
        if (is_prohibited_paragraph_child_name(child_local_name)) {
            DOM::Node* ancestor = &parent_html_element;
            while (ancestor) {
                if (is_element_with_inline_contents(*ancestor))
                    return false;
                ancestor = ancestor->parent();
            }
        }

        // 3. If child is "h1", "h2", "h3", "h4", "h5", or "h6", and parent or some ancestor of parent is an HTML
        //    element with local name "h1", "h2", "h3", "h4", "h5", or "h6", return false.
        if (is_heading(child_local_name)) {
            DOM::Node* ancestor = &parent_html_element;
            while (ancestor) {
                if (is<HTML::HTMLElement>(*ancestor) && is_heading(static_cast<DOM::Element&>(*ancestor).local_name()))
                    return false;
                ancestor = ancestor->parent();
            }
        }

        // 4. Let parent be the local name of parent.
        parent = parent_html_element.local_name();
        parent_node = {};
    }

    // 7. If parent is an Element or DocumentFragment, return true.
    if (is<DOM::Element>(parent_node.ptr()) || is<DOM::DocumentFragment>(parent_node.ptr()))
        return true;

    // 8. If parent is not a string, return false.
    if (!parent.has<FlyString>())
        return false;
    auto parent_local_name = parent.get<FlyString>();

    // 9. If parent is on the left-hand side of an entry on the following list, then return true if child is listed on
    //    the right-hand side of that entry, and false otherwise.

    // * colgroup: col
    if (parent_local_name == HTML::TagNames::colgroup)
        return child_local_name == HTML::TagNames::col;

    // * table: caption, col, colgroup, tbody, td, tfoot, th, thead, tr
    if (parent_local_name == HTML::TagNames::table) {
        return child_local_name.is_one_of(
            HTML::TagNames::caption,
            HTML::TagNames::col,
            HTML::TagNames::colgroup,
            HTML::TagNames::tbody,
            HTML::TagNames::td,
            HTML::TagNames::tfoot,
            HTML::TagNames::th,
            HTML::TagNames::thead,
            HTML::TagNames::tr);
    }

    // * tbody, tfoot, thead: td, th, tr
    if (parent_local_name.is_one_of(HTML::TagNames::tbody, HTML::TagNames::tfoot, HTML::TagNames::thead))
        return child_local_name.is_one_of(HTML::TagNames::td, HTML::TagNames::th, HTML::TagNames::tr);

    // * tr: td, th
    if (parent_local_name == HTML::TagNames::tr)
        return child_local_name.is_one_of(HTML::TagNames::td, HTML::TagNames::th);

    // * dl: dt, dd
    if (parent_local_name == HTML::TagNames::dl)
        return child_local_name.is_one_of(HTML::TagNames::dt, HTML::TagNames::dd);

    // * dir, ol, ul: dir, li, ol, ul
    if (parent_local_name.is_one_of(HTML::TagNames::dir, HTML::TagNames::ol, HTML::TagNames::ul))
        return child_local_name.is_one_of(HTML::TagNames::dir, HTML::TagNames::li, HTML::TagNames::ol, HTML::TagNames::ul);

    // * hgroup: h1, h2, h3, h4, h5, h6
    if (parent_local_name == HTML::TagNames::hgroup)
        return is_heading(child_local_name);

    // 10. If child is "body", "caption", "col", "colgroup", "frame", "frameset", "head", "html", "tbody", "td",
    //     "tfoot", "th", "thead", or "tr", return false.
    if (child_local_name.is_one_of(
            HTML::TagNames::body,
            HTML::TagNames::caption,
            HTML::TagNames::col,
            HTML::TagNames::colgroup,
            HTML::TagNames::frame,
            HTML::TagNames::frameset,
            HTML::TagNames::head,
            HTML::TagNames::html,
            HTML::TagNames::tbody,
            HTML::TagNames::td,
            HTML::TagNames::tfoot,
            HTML::TagNames::th,
            HTML::TagNames::thead,
            HTML::TagNames::tr))
        return false;

    // 11. If child is "dd" or "dt" and parent is not "dl", return false.
    if (child_local_name.is_one_of(HTML::TagNames::dd, HTML::TagNames::dt) && parent_local_name != HTML::TagNames::dl)
        return false;

    // 12. If child is "li" and parent is not "ol" or "ul", return false.
    if (child_local_name == HTML::TagNames::li && parent_local_name != HTML::TagNames::ol && parent_local_name != HTML::TagNames::ul)
        return false;

    // 13. If parent is on the left-hand side of an entry on the following list and child is listed on the right-hand
    //     side of that entry, return false.

    // * a: a
    if (parent_local_name == HTML::TagNames::a && child_local_name == HTML::TagNames::a)
        return false;

    // * dd, dt: dd, dt
    if (parent_local_name.is_one_of(HTML::TagNames::dd, HTML::TagNames::dt)
        && child_local_name.is_one_of(HTML::TagNames::dd, HTML::TagNames::dt))
        return false;

    // * h1, h2, h3, h4, h5, h6: h1, h2, h3, h4, h5, h6
    if (is_heading(parent_local_name) && is_heading(child_local_name))
        return false;

    // * li: li
    if (parent_local_name == HTML::TagNames::li && child_local_name == HTML::TagNames::li)
        return false;

    // * nobr: nobr
    if (parent_local_name == HTML::TagNames::nobr && child_local_name == HTML::TagNames::nobr)
        return false;

    // * All names of an element with inline contents: all prohibited paragraph child names
    if (is_name_of_an_element_with_inline_contents(parent_local_name) && is_prohibited_paragraph_child_name(child_local_name))
        return false;

    // * td, th: caption, col, colgroup, tbody, td, tfoot, th, thead, tr
    if (parent_local_name.is_one_of(HTML::TagNames::td, HTML::TagNames::th)
        && child_local_name.is_one_of(
            HTML::TagNames::caption,
            HTML::TagNames::col,
            HTML::TagNames::colgroup,
            HTML::TagNames::tbody,
            HTML::TagNames::td,
            HTML::TagNames::tfoot,
            HTML::TagNames::th,
            HTML::TagNames::thead,
            HTML::TagNames::tr))
        return false;

    // 14. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#block-boundary-point
bool is_block_boundary_point(GC::Ref<DOM::Node> node, u32 offset)
{
    // A boundary point is a block boundary point if it is either a block start point or a block end point.
    return is_block_start_point(node, offset) || is_block_end_point(node, offset);
}

// https://w3c.github.io/editing/docs/execCommand/#block-end-point
bool is_block_end_point(GC::Ref<DOM::Node> node, u32 offset)
{
    // A boundary point (node, offset) is a block end point if either node's parent is null and
    // offset is node's length;
    if (!node->parent() && offset == node->length())
        return true;

    // or node has a child with index offset, and that child is a visible block node.
    auto offset_child = node->child_at_index(offset);
    return offset_child && is_visible_node(*offset_child) && is_block_node(*offset_child);
}

// https://w3c.github.io/editing/docs/execCommand/#block-node
bool is_block_node(GC::Ref<DOM::Node> node)
{
    // A block node is either an Element whose "display" property does not have resolved value
    // "inline" or "inline-block" or "inline-table" or "none", or a document, or a DocumentFragment.
    if (is<DOM::Document>(*node) || is<DOM::DocumentFragment>(*node))
        return true;

    auto layout_node = node->layout_node();
    if (!layout_node)
        return false;

    auto display = layout_node->display();
    return is<DOM::Element>(*node)
        && !(display.is_inline_outside() && (display.is_flow_inside() || display.is_flow_root_inside() || display.is_table_inside()))
        && !display.is_none();
}

// https://w3c.github.io/editing/docs/execCommand/#block-start-point
bool is_block_start_point(GC::Ref<DOM::Node> node, u32 offset)
{
    // A boundary point (node, offset) is a block start point if either node's parent is null and
    // offset is zero;
    if (!node->parent() && offset == 0)
        return true;

    // or node has a child with index offset − 1, and that child is either a visible block node or a
    // visible br.
    auto offset_minus_one_child = node->child_at_index(offset - 1);
    if (!offset_minus_one_child)
        return false;
    return is_visible_node(*offset_minus_one_child)
        && (is_block_node(*offset_minus_one_child) || is<HTML::HTMLBRElement>(*offset_minus_one_child));
}

// https://w3c.github.io/editing/docs/execCommand/#collapsed-block-prop
bool is_collapsed_block_prop(GC::Ref<DOM::Node> node)
{
    // A collapsed block prop is either a collapsed line break that is not an extraneous line break,
    if (is_collapsed_line_break(node) && !is_extraneous_line_break(node))
        return true;

    // or an Element that is an inline node
    if (!is<DOM::Element>(*node) || !is_inline_node(node))
        return false;

    // and whose children are all either invisible or collapsed block props
    bool children_all_invisible_or_collapsed = true;
    bool has_collapsed_block_prop = false;
    node->for_each_child([&](GC::Ref<DOM::Node> child) {
        auto child_is_collapsed_block_prop = is_collapsed_block_prop(child);
        if (!is_invisible_node(child) && !child_is_collapsed_block_prop) {
            children_all_invisible_or_collapsed = false;
            return IterationDecision::Break;
        }
        if (child_is_collapsed_block_prop)
            has_collapsed_block_prop = true;
        return IterationDecision::Continue;
    });
    if (!children_all_invisible_or_collapsed)
        return false;

    // and that has at least one child that is a collapsed block prop.
    return has_collapsed_block_prop;
}

// https://w3c.github.io/editing/docs/execCommand/#collapsed-line-break
bool is_collapsed_line_break(GC::Ref<DOM::Node> node)
{
    // A collapsed line break is a br
    if (!is<HTML::HTMLBRElement>(*node))
        return false;

    // that begins a line box which has nothing else in it, and therefore has zero height.
    auto layout_node = node->layout_node();
    if (!layout_node)
        return false;
    VERIFY(is<Layout::BreakNode>(*layout_node));

    // NOTE: We do not generate a TextNode for empty text after the break, so if we do not have a sibling or if that
    //       sibling is not a TextNode, we consider it a collapsed line break.
    auto* next_layout_node = layout_node->next_sibling();
    return !is<Layout::TextNode>(next_layout_node);
}

// https://w3c.github.io/editing/docs/execCommand/#collapsed-whitespace-node
bool is_collapsed_whitespace_node(GC::Ref<DOM::Node> node)
{
    // 1. If node is not a whitespace node, return false.
    if (!is_whitespace_node(node))
        return false;

    // 2. If node's data is the empty string, return true.
    auto node_data = node->text_content();
    if (!node_data.has_value() || node_data->is_empty())
        return true;

    // 3. Let ancestor be node's parent.
    GC::Ptr<DOM::Node> ancestor = node->parent();

    // 4. If ancestor is null, return true.
    if (!ancestor)
        return true;

    // 5. If the "display" property of some ancestor of node has resolved value "none", return true.
    if (ancestor->layout_node() && ancestor->layout_node()->display().is_none())
        return true;

    // 6. While ancestor is not a block node and its parent is not null, set ancestor to its parent.
    while (!is_block_node(*ancestor) && ancestor->parent())
        ancestor = ancestor->parent();

    // 7. Let reference be node.
    GC::Ptr<DOM::Node> reference = node;

    // 8. While reference is a descendant of ancestor:
    while (reference->is_descendant_of(*ancestor)) {
        // 1. Let reference be the node before it in tree order.
        reference = reference->previous_in_pre_order();

        // 2. If reference is a block node or a br, return true.
        if (is_block_node(*reference) || is<HTML::HTMLBRElement>(*reference))
            return true;

        // 3. If reference is a Text node that is not a whitespace node, or is an img, break from
        //    this loop.
        if ((is<DOM::Text>(*reference) && !is_whitespace_node(*reference)) || is<HTML::HTMLImageElement>(*reference))
            break;
    }

    // 9. Let reference be node.
    reference = node;

    // 10. While reference is a descendant of ancestor:
    while (reference->is_descendant_of(*ancestor)) {
        // 1. Let reference be the node after it in tree order, or null if there is no such node.
        reference = reference->next_in_pre_order();

        // NOTE: Both steps below and the loop condition require a reference, so break if it's null.
        if (!reference)
            break;

        // 2. If reference is a block node or a br, return true.
        if (is_block_node(*reference) || is<HTML::HTMLBRElement>(*reference))
            return true;

        // 3. If reference is a Text node that is not a whitespace node, or is an img, break from
        //    this loop.
        if ((is<DOM::Text>(*reference) && !is_whitespace_node(*reference)) || is<HTML::HTMLImageElement>(*reference))
            break;
    }

    // 11. Return false.
    return false;
}

// https://w3c.github.io/editing/docs/execCommand/#element-with-inline-contents
bool is_element_with_inline_contents(GC::Ref<DOM::Node> node)
{
    // An element with inline contents is an HTML element whose local name is a name of an element with inline contents.
    return is<HTML::HTMLElement>(*node)
        && is_name_of_an_element_with_inline_contents(static_cast<DOM::Element&>(*node).local_name());
}

// https://w3c.github.io/editing/docs/execCommand/#extraneous-line-break
bool is_extraneous_line_break(GC::Ref<DOM::Node> node)
{
    // An extraneous line break is a br
    if (!is<HTML::HTMLBRElement>(*node))
        return false;

    // ...except that a br that is the sole child of an li is not extraneous.
    GC::Ptr<DOM::Node> parent = node->parent();
    if (is<HTML::HTMLLIElement>(parent.ptr()) && parent->child_count() == 1)
        return false;

    // FIXME: ...that has no visual effect, in that removing it from the DOM
    //        would not change layout,

    return false;
}

// https://w3c.github.io/editing/docs/execCommand/#in-the-same-editing-host
bool is_in_same_editing_host(GC::Ref<DOM::Node> node_a, GC::Ref<DOM::Node> node_b)
{
    // Two nodes are in the same editing host if the editing host of the first is non-null and the
    // same as the editing host of the second.
    auto editing_host_a = editing_host_of_node(node_a);
    auto editing_host_b = editing_host_of_node(node_b);
    return editing_host_a && editing_host_a == editing_host_b;
}

// https://w3c.github.io/editing/docs/execCommand/#inline-node
bool is_inline_node(GC::Ref<DOM::Node> node)
{
    // An inline node is a node that is not a block node.
    return !is_block_node(node);
}

// https://w3c.github.io/editing/docs/execCommand/#invisible
bool is_invisible_node(GC::Ref<DOM::Node> node)
{
    // Something is invisible if it is a node that is not visible.
    return !is_visible_node(node);
}

// https://w3c.github.io/editing/docs/execCommand/#name-of-an-element-with-inline-contents
bool is_name_of_an_element_with_inline_contents(FlyString const& local_name)
{
    // A name of an element with inline contents is "a", "abbr", "b", "bdi", "bdo", "cite", "code", "dfn", "em", "h1",
    // "h2", "h3", "h4", "h5", "h6", "i", "kbd", "mark", "p", "pre", "q", "rp", "rt", "ruby", "s", "samp", "small",
    // "span", "strong", "sub", "sup", "u", "var", "acronym", "listing", "strike", "xmp", "big", "blink", "font",
    // "marquee", "nobr", or "tt".
    return local_name.is_one_of(
        HTML::TagNames::a,
        HTML::TagNames::abbr,
        HTML::TagNames::b,
        HTML::TagNames::bdi,
        HTML::TagNames::bdo,
        HTML::TagNames::cite,
        HTML::TagNames::code,
        HTML::TagNames::dfn,
        HTML::TagNames::em,
        HTML::TagNames::h1,
        HTML::TagNames::h2,
        HTML::TagNames::h3,
        HTML::TagNames::h4,
        HTML::TagNames::h5,
        HTML::TagNames::h6,
        HTML::TagNames::i,
        HTML::TagNames::kbd,
        HTML::TagNames::mark,
        HTML::TagNames::p,
        HTML::TagNames::pre,
        HTML::TagNames::q,
        HTML::TagNames::rp,
        HTML::TagNames::rt,
        HTML::TagNames::ruby,
        HTML::TagNames::s,
        HTML::TagNames::samp,
        HTML::TagNames::small,
        HTML::TagNames::span,
        HTML::TagNames::strong,
        HTML::TagNames::sub,
        HTML::TagNames::sup,
        HTML::TagNames::u,
        HTML::TagNames::var,
        HTML::TagNames::acronym,
        HTML::TagNames::listing,
        HTML::TagNames::strike,
        HTML::TagNames::xmp,
        HTML::TagNames::big,
        HTML::TagNames::blink,
        HTML::TagNames::font,
        HTML::TagNames::marquee,
        HTML::TagNames::nobr,
        HTML::TagNames::tt);
}

// https://w3c.github.io/editing/docs/execCommand/#non-list-single-line-container
bool is_non_list_single_line_container(GC::Ref<DOM::Node> node)
{
    // A non-list single-line container is an HTML element with local name "address", "divis_", "h1", "h2", "h3", "h4",
    // "h5", "h6", "listing", "p", "pre", or "xmp".
    if (!is<HTML::HTMLElement>(*node))
        return false;
    auto& local_name = static_cast<HTML::HTMLElement&>(*node).local_name();
    return is_heading(local_name)
        || local_name.is_one_of(HTML::TagNames::address, HTML::TagNames::div, HTML::TagNames::listing,
            HTML::TagNames::p, HTML::TagNames::pre, HTML::TagNames::xmp);
}

// https://w3c.github.io/editing/docs/execCommand/#prohibited-paragraph-child
bool is_prohibited_paragraph_child(GC::Ref<DOM::Node> node)
{
    // A prohibited paragraph child is an HTML element whose local name is a prohibited paragraph child name.
    return is<HTML::HTMLElement>(*node) && is_prohibited_paragraph_child_name(static_cast<DOM::Element&>(*node).local_name());
}

// https://w3c.github.io/editing/docs/execCommand/#prohibited-paragraph-child-name
bool is_prohibited_paragraph_child_name(FlyString const& local_name)
{
    // A prohibited paragraph child name is "address", "article", "aside", "blockquote", "caption", "center", "col",
    // "colgroup", "dd", "details", "dir", "div", "dl", "dt", "fieldset", "figcaption", "figure", "footer", "form",
    // "h1", "h2", "h3", "h4", "h5", "h6", "header", "hgroup", "hr", "li", "listing", "menu", "nav", "ol", "p",
    // "plaintext", "pre", "section", "summary", "table", "tbody", "td", "tfoot", "th", "thead", "tr", "ul", or "xmp".
    return local_name.is_one_of(
        HTML::TagNames::address,
        HTML::TagNames::article,
        HTML::TagNames::aside,
        HTML::TagNames::blockquote,
        HTML::TagNames::caption,
        HTML::TagNames::center,
        HTML::TagNames::col,
        HTML::TagNames::colgroup,
        HTML::TagNames::dd,
        HTML::TagNames::details,
        HTML::TagNames::dir,
        HTML::TagNames::div,
        HTML::TagNames::dl,
        HTML::TagNames::dt,
        HTML::TagNames::fieldset,
        HTML::TagNames::figcaption,
        HTML::TagNames::figure,
        HTML::TagNames::footer,
        HTML::TagNames::form,
        HTML::TagNames::h1,
        HTML::TagNames::h2,
        HTML::TagNames::h3,
        HTML::TagNames::h4,
        HTML::TagNames::h5,
        HTML::TagNames::h6,
        HTML::TagNames::header,
        HTML::TagNames::hgroup,
        HTML::TagNames::hr,
        HTML::TagNames::li,
        HTML::TagNames::listing,
        HTML::TagNames::menu,
        HTML::TagNames::nav,
        HTML::TagNames::ol,
        HTML::TagNames::p,
        HTML::TagNames::plaintext,
        HTML::TagNames::pre,
        HTML::TagNames::section,
        HTML::TagNames::summary,
        HTML::TagNames::table,
        HTML::TagNames::tbody,
        HTML::TagNames::td,
        HTML::TagNames::tfoot,
        HTML::TagNames::th,
        HTML::TagNames::thead,
        HTML::TagNames::tr,
        HTML::TagNames::ul,
        HTML::TagNames::xmp);
}

// https://w3c.github.io/editing/docs/execCommand/#single-line-container
bool is_single_line_container(GC::Ref<DOM::Node> node)
{
    // A single-line container is either a non-list single-line container, or an HTML element with local name "li",
    // "dt", or "dd".
    if (is_non_list_single_line_container(node))
        return true;
    if (!is<HTML::HTMLElement>(*node))
        return false;
    auto& html_element = static_cast<HTML::HTMLElement&>(*node);
    return html_element.local_name().is_one_of(HTML::TagNames::li, HTML::TagNames::dt, HTML::TagNames::dd);
}

// https://w3c.github.io/editing/docs/execCommand/#visible
bool is_visible_node(GC::Ref<DOM::Node> node)
{
    // excluding any node with an inclusive ancestor Element whose "display" property has resolved
    // value "none".
    GC::Ptr<DOM::Node> inclusive_ancestor = node;
    while (inclusive_ancestor) {
        auto* layout_node = inclusive_ancestor->layout_node();
        if (layout_node && layout_node->display().is_none())
            return false;
        inclusive_ancestor = inclusive_ancestor->parent();
    }

    // Something is visible if it is a node that either is a block node,
    if (is_block_node(node))
        return true;

    // or a Text node that is not a collapsed whitespace node,
    if (is<DOM::Text>(*node) && !is_collapsed_whitespace_node(node))
        return true;

    // or an img,
    if (is<HTML::HTMLImageElement>(*node))
        return true;

    // or a br that is not an extraneous line break,
    if (is<HTML::HTMLBRElement>(*node) && !is_extraneous_line_break(node))
        return true;

    // or any node with a visible descendant;
    // NOTE: We call into is_visible_node() recursively, so check children instead of descendants.
    bool has_visible_child_node = false;
    node->for_each_child([&](DOM::Node& child_node) {
        if (is_visible_node(child_node)) {
            has_visible_child_node = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return has_visible_child_node;
}

// https://w3c.github.io/editing/docs/execCommand/#whitespace-node
bool is_whitespace_node(GC::Ref<DOM::Node> node)
{
    // NOTE: All constraints below check that node is a Text node
    if (!is<DOM::Text>(*node))
        return false;

    // A whitespace node is either a Text node whose data is the empty string;
    auto& character_data = static_cast<DOM::CharacterData&>(*node);
    if (character_data.data().is_empty())
        return true;

    // NOTE: All constraints below require a parent Element with a resolved value for "white-space"
    GC::Ptr<DOM::Node> parent = node->parent();
    if (!is<DOM::Element>(parent.ptr()))
        return false;
    auto* layout_node = parent->layout_node();
    if (!layout_node)
        return false;
    auto white_space = layout_node->computed_values().white_space();

    // or a Text node whose data consists only of one or more tabs (0x0009), line feeds (0x000A),
    // carriage returns (0x000D), and/or spaces (0x0020), and whose parent is an Element whose
    // resolved value for "white-space" is "normal" or "nowrap";
    auto is_tab_lf_cr_or_space = [](u32 codepoint) {
        return codepoint == '\t' || codepoint == '\n' || codepoint == '\r' || codepoint == ' ';
    };
    auto code_points = character_data.data().code_points();
    if (all_of(code_points, is_tab_lf_cr_or_space) && (white_space == CSS::WhiteSpace::Normal || white_space == CSS::WhiteSpace::Nowrap))
        return true;

    // or a Text node whose data consists only of one or more tabs (0x0009), carriage returns
    // (0x000D), and/or spaces (0x0020), and whose parent is an Element whose resolved value for
    // "white-space" is "pre-line".
    auto is_tab_cr_or_space = [](u32 codepoint) {
        return codepoint == '\t' || codepoint == '\r' || codepoint == ' ';
    };
    if (all_of(code_points, is_tab_cr_or_space) && white_space == CSS::WhiteSpace::PreLine)
        return true;

    return false;
}

// https://w3c.github.io/editing/docs/execCommand/#last-equivalent-point
BoundaryPoint last_equivalent_point(BoundaryPoint boundary_point)
{
    // 1. While (node, offset)'s next equivalent point is not null, set (node, offset) to its next equivalent point.
    while (true) {
        auto next_point = next_equivalent_point(boundary_point);
        if (!next_point.has_value())
            break;
        boundary_point = next_point.release_value();
    }

    // 2. Return (node, offset).
    return boundary_point;
}

// https://w3c.github.io/editing/docs/execCommand/#preserving-ranges
void move_node_preserving_ranges(GC::Ref<DOM::Node> node, GC::Ref<DOM::Node> new_parent, u32 new_index)
{
    // To move a node to a new location, preserving ranges, remove the node from its original parent
    // (if any), then insert it in the new location. In doing so, follow these rules instead of
    // those defined by the insert and remove algorithms:

    // FIXME: Currently this is a simple range-destroying move. Implement "follow these rules" as
    //        described above.

    // 1. Let node be the moved node, old parent and old index be the old parent (which may be null)
    //    and index, and new parent and new index be the new parent and index.
    auto* old_parent = node->parent();
    [[maybe_unused]] auto old_index = node->index();
    if (old_parent)
        node->remove();

    auto* new_next_sibling = new_parent->child_at_index(new_index);
    new_parent->insert_before(node, new_next_sibling);

    // FIXME: 2. If a boundary point's node is the same as or a descendant of node, leave it unchanged, so
    //    it moves to the new location.

    // FIXME: 3. If a boundary point's node is new parent and its offset is greater than new index, add one
    //    to its offset.

    // FIXME: 4. If a boundary point's node is old parent and its offset is old index or old index + 1, set
    //    its node to new parent and add new index − old index to its offset.

    // FIXME: 5. If a boundary point's node is old parent and its offset is greater than old index + 1,
    //    subtract one from its offset.
}

// https://w3c.github.io/editing/docs/execCommand/#next-equivalent-point
Optional<BoundaryPoint> next_equivalent_point(BoundaryPoint boundary_point)
{
    // 1. If node's length is zero, return null.
    auto node = boundary_point.node;
    auto node_length = node->length();
    if (node_length == 0)
        return {};

    // 3. If offset is node's length, and node's parent is not null, and node is an inline node, return (node's parent,
    //    1 + node's index).
    if (boundary_point.offset == node_length && node->parent() && is_inline_node(*node))
        return BoundaryPoint { *node->parent(), static_cast<WebIDL::UnsignedLong>(node->index() + 1) };

    // 5. If node has a child with index offset, and that child's length is not zero, and that child is an inline node,
    //    return (that child, 0).
    auto child_at_offset = node->child_at_index(boundary_point.offset);
    if (child_at_offset && child_at_offset->length() != 0 && is_inline_node(*child_at_offset))
        return BoundaryPoint { *child_at_offset, 0 };

    // 7. Return null.
    return {};
}

// https://w3c.github.io/editing/docs/execCommand/#normalize-sublists
void normalize_sublists_in_node(GC::Ref<DOM::Element> item)
{
    // 1. If item is not an li or it is not editable or its parent is not editable, abort these
    //    steps.
    if (item->local_name() != HTML::TagNames::li || !item->is_editable() || !item->parent()->is_editable())
        return;

    // 2. Let new item be null.
    GC::Ptr<DOM::Element> new_item;

    // 3. While item has an ol or ul child:
    while (item->has_child_of_type<HTML::HTMLOListElement>() || item->has_child_of_type<HTML::HTMLUListElement>()) {
        // 1. Let child be the last child of item.
        GC::Ref<DOM::Node> child = *item->last_child();

        // 2. If child is an ol or ul, or new item is null and child is a Text node whose data
        //    consists of zero of more space characters:
        auto child_text = child->text_content();
        auto text_is_all_whitespace = child_text.has_value() && all_of(child_text.value().bytes_as_string_view(), Infra::is_ascii_whitespace);
        if ((is<HTML::HTMLOListElement>(*child) || is<HTML::HTMLUListElement>(*child))
            || (!new_item && is<DOM::Text>(*child) && text_is_all_whitespace)) {
            // 1. Set new item to null.
            new_item = {};

            // 2. Insert child into the parent of item immediately following item, preserving
            //    ranges.
            move_node_preserving_ranges(child, *item->parent(), item->index());
        }

        // 3. Otherwise:
        else {
            // 1. If new item is null, let new item be the result of calling createElement("li") on
            //    the ownerDocument of item, then insert new item into the parent of item
            //    immediately after item.
            if (!new_item) {
                new_item = MUST(DOM::create_element(*item->owner_document(), HTML::TagNames::li, Namespace::HTML));
                item->parent()->insert_before(*new_item, item->next_sibling());
            }

            // 2. Insert child into new item as its first child, preserving ranges.
            move_node_preserving_ranges(child, *new_item, 0);
        }
    }
}

// https://w3c.github.io/editing/docs/execCommand/#precedes-a-line-break
bool precedes_a_line_break(GC::Ref<DOM::Node> node)
{
    // 1. Let offset be node's length.
    auto offset = node->length();

    // 2. While (node, offset) is not a block boundary point:
    while (!is_block_boundary_point(node, offset)) {
        // 1. If node has a visible child with index offset, return false.
        auto* offset_child = node->child_at_index(offset);
        if (offset_child && is_visible_node(*offset_child))
            return false;

        // 2. If offset is node's length or node has no children, set offset to one plus node's
        //    index, then set node to its parent.
        if (offset == node->length() || node->child_count() == 0) {
            offset = node->index() + 1;
            node = *node->parent();
        }

        // 3. Otherwise, set node to its child with index offset and set offset to zero.
        else {
            node = *node->child_at_index(offset);
            offset = 0;
        }
    }

    // 3. Return true;
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#previous-equivalent-point
Optional<BoundaryPoint> previous_equivalent_point(BoundaryPoint boundary_point)
{
    // 1. If node's length is zero, return null.
    auto node = boundary_point.node;
    auto node_length = node->length();
    if (node_length == 0)
        return {};

    // 2. If offset is 0, and node's parent is not null, and node is an inline node, return (node's parent, node's
    //    index).
    if (boundary_point.offset == 0 && node->parent() && is_inline_node(*node))
        return BoundaryPoint { *node->parent(), static_cast<WebIDL::UnsignedLong>(node->index()) };

    // 3. If node has a child with index offset − 1, and that child's length is not zero, and that child is an inline
    //    node, return (that child, that child's length).
    auto child_at_offset = node->child_at_index(boundary_point.offset - 1);
    if (child_at_offset && child_at_offset->length() != 0 && is_inline_node(*child_at_offset))
        return BoundaryPoint { *child_at_offset, static_cast<WebIDL::UnsignedLong>(child_at_offset->length()) };

    // 4. Return null.
    return {};
}

// https://w3c.github.io/editing/docs/execCommand/#record-current-states-and-values
Vector<RecordedOverride> record_current_states_and_values(GC::Ref<DOM::Range>)
{
    // 1. Let overrides be a list of (string, string or boolean) ordered pairs, initially empty.
    Vector<RecordedOverride> overrides;

    // FIXME: 2. Let node be the first formattable node effectively contained in the active range, or null if there is none.

    // FIXME: 3. If node is null, return overrides.

    // FIXME: 4. Add ("createLink", node's effective command value for "createLink") to overrides.

    // FIXME: 5. For each command in the list "bold", "italic", "strikethrough", "subscript", "superscript", "underline", in
    //    order: if node's effective command value for command is one of its inline command activated values, add
    //    (command, true) to overrides, and otherwise add (command, false) to overrides.

    // FIXME: 6. For each command in the list "fontName", "foreColor", "hiliteColor", in order: add (command, command's value)
    //    to overrides.

    // FIXME: 7. Add ("fontSize", node's effective command value for "fontSize") to overrides.

    // 8. Return overrides.
    return overrides;
}

// https://w3c.github.io/editing/docs/execCommand/#record-the-values
Vector<RecordedNodeValue> record_the_values_of_nodes(Vector<GC::Ref<DOM::Node>> const& node_list)
{
    // 1. Let values be a list of (node, command, specified command value) triples, initially empty.
    Vector<RecordedNodeValue> values;

    // 2. For each node in node list, for each command in the list "subscript", "bold", "fontName",
    //    "fontSize", "foreColor", "hiliteColor", "italic", "strikethrough", and "underline" in that
    //    order:
    Array const commands = { CommandNames::subscript, CommandNames::bold, CommandNames::fontName,
        CommandNames::fontSize, CommandNames::foreColor, CommandNames::hiliteColor, CommandNames::italic,
        CommandNames::strikethrough, CommandNames::underline };
    for (auto node : node_list) {
        for (auto command : commands) {
            // 1. Let ancestor equal node.
            auto ancestor = node;

            // 2. If ancestor is not an Element, set it to its parent.
            if (!is<DOM::Element>(*ancestor))
                ancestor = *ancestor->parent();

            // 3. While ancestor is an Element and its specified command value for command is null, set
            //    it to its parent.
            while (is<DOM::Element>(*ancestor) && !specified_command_value(static_cast<DOM::Element&>(*ancestor), command).has_value())
                ancestor = *ancestor->parent();

            // 4. If ancestor is an Element, add (node, command, ancestor's specified command value for
            //    command) to values. Otherwise add (node, command, null) to values.
            if (is<DOM::Element>(*ancestor))
                values.empend(*node, command, specified_command_value(static_cast<DOM::Element&>(*ancestor), command));
            else
                values.empend(*node, command, OptionalNone {});
        }
    }

    // 3. Return values.
    return values;
}

// https://w3c.github.io/editing/docs/execCommand/#remove-extraneous-line-breaks-at-the-end-of
void remove_extraneous_line_breaks_at_the_end_of_node(GC::Ref<DOM::Node> node)
{
    // 1. Let ref be node.
    GC::Ptr<DOM::Node> ref = node;

    // 2. While ref has children, set ref to its lastChild.
    while (ref->child_count() > 0)
        ref = ref->last_child();

    // 3. While ref is invisible but not an extraneous line break, and ref does not equal node, set
    //    ref to the node before it in tree order.
    while (is_invisible_node(*ref)
        && !is_extraneous_line_break(*ref)
        && ref != node) {
        ref = ref->previous_in_pre_order();
    }

    // 4. If ref is an editable extraneous line break:
    if (ref->is_editable() && is_extraneous_line_break(*ref)) {
        // 1. While ref's parent is editable and invisible, set ref to its parent.
        while (ref->parent()->is_editable() && is_invisible_node(*ref->parent()))
            ref = ref->parent();

        // 2. Remove ref from its parent.
        ref->remove();
    }
}

// https://w3c.github.io/editing/docs/execCommand/#remove-extraneous-line-breaks-before
void remove_extraneous_line_breaks_before_node(GC::Ref<DOM::Node> node)
{
    // 1. Let ref be the previousSibling of node.
    GC::Ptr<DOM::Node> ref = node->previous_sibling();

    // 2. If ref is null, abort these steps.
    if (!ref)
        return;

    // 3. While ref has children, set ref to its lastChild.
    while (ref->child_count() > 0)
        ref = ref->last_child();

    // 4. While ref is invisible but not an extraneous line break, and ref does not equal node's
    //    parent, set ref to the node before it in tree order.
    while (is_invisible_node(*ref)
        && !is_extraneous_line_break(*ref)
        && ref != node->parent()) {
        ref = ref->previous_in_pre_order();
    }

    // 5. If ref is an editable extraneous line break, remove it from its parent.
    if (ref->is_editable() && is_extraneous_line_break(*ref))
        ref->remove();
}

// https://w3c.github.io/editing/docs/execCommand/#remove-extraneous-line-breaks-from
void remove_extraneous_line_breaks_from_a_node(GC::Ref<DOM::Node> node)
{
    // To remove extraneous line breaks from a node, first remove extraneous line breaks before it,
    // then remove extraneous line breaks at the end of it.
    remove_extraneous_line_breaks_before_node(node);
    remove_extraneous_line_breaks_at_the_end_of_node(node);
}

// https://w3c.github.io/editing/docs/execCommand/#preserving-its-descendants
void remove_node_preserving_its_descendants(GC::Ref<DOM::Node> node)
{
    // To remove a node node while preserving its descendants, split the parent of node's children
    // if it has any.
    if (node->child_count() > 0) {
        Vector<GC::Ref<DOM::Node>> children;
        children.ensure_capacity(node->child_count());
        for (auto* child = node->first_child(); child; child = child->next_sibling())
            children.append(*child);
        split_the_parent_of_nodes(move(children));
        return;
    }

    // If it has no children, instead remove it from its parent.
    node->remove();
}

// https://w3c.github.io/editing/docs/execCommand/#restore-states-and-values
void restore_states_and_values(GC::Ref<DOM::Range>, Vector<RecordedOverride> const& overrides)
{
    // FIXME: 1. Let node be the first formattable node effectively contained in the active range, or null if there is none.

    // FIXME: 2. If node is not null, then for each (command, override) pair in overrides, in order:
    {
        // FIXME: 1. If override is a boolean, and queryCommandState(command) returns something different from override, take
        //    the action for command, with value equal to the empty string.

        // FIXME: 2. Otherwise, if override is a string, and command is neither "createLink" nor "fontSize", and
        //    queryCommandValue(command) returns something not equivalent to override, take the action for command, with
        //    value equal to override.

        // FIXME: 3. Otherwise, if override is a string; and command is "createLink"; and either there is a value override for
        //    "createLink" that is not equal to override, or there is no value override for "createLink" and node's
        //    effective command value for "createLink" is not equal to override: take the action for "createLink", with
        //    value equal to override.

        // FIXME: 4. Otherwise, if override is a string; and command is "fontSize"; and either there is a value override for
        //    "fontSize" that is not equal to override, or there is no value override for "fontSize" and node's
        //     effective command value for "fontSize" is not loosely equivalent to override:
        {
            // FIXME: 1. Convert override to an integer number of pixels, and set override to the legacy font size for the
            //    result.

            // FIXME: 2. Take the action for "fontSize", with value equal to override.
        }

        // FIXME: 5. Otherwise, continue this loop from the beginning.

        // FIXME: 6. Set node to the first formattable node effectively contained in the active range, if there is one.
    }

    // 3. Otherwise, for each (command, override) pair in overrides, in order:
    for ([[maybe_unused]] auto const& override : overrides) {
        // FIXME: 1. If override is a boolean, set the state override for command to override.

        // FIXME: 2. If override is a string, set the value override for command to override.
    }
}

// https://w3c.github.io/editing/docs/execCommand/#restore-the-values
void restore_the_values_of_nodes(Vector<RecordedNodeValue> const& values)
{
    // 1. For each (node, command, value) triple in values:
    for (auto& recorded_node_value : values) {
        // 1. Let ancestor equal node.
        GC::Ptr<DOM::Node> ancestor = recorded_node_value.node;

        // 2. If ancestor is not an Element, set it to its parent.
        if (!is<DOM::Element>(*ancestor))
            ancestor = *ancestor->parent();

        // 3. While ancestor is an Element and its specified command value for command is null, set it to its parent.
        auto const& command = recorded_node_value.command;
        while (is<DOM::Element>(*ancestor) && !specified_command_value(static_cast<DOM::Element&>(*ancestor), command).has_value())
            ancestor = *ancestor->parent();

        // FIXME: 4. If value is null and ancestor is an Element, push down values on node for command, with new value null.

        // FIXME: 5. Otherwise, if ancestor is an Element and its specified command value for command is not equivalent to
        //    value, or if ancestor is not an Element and value is not null, force the value of command to value on
        //    node.
    }
}

// https://w3c.github.io/editing/docs/execCommand/#set-the-tag-name
GC::Ref<DOM::Element> set_the_tag_name(GC::Ref<DOM::Element> element, FlyString const& new_name)
{
    // 1. If element is an HTML element with local name equal to new name, return element.
    if (is<HTML::HTMLElement>(*element) && static_cast<DOM::Element&>(element).local_name() == new_name)
        return element;

    // 2. If element's parent is null, return element.
    if (!element->parent())
        return element;

    // 3. Let replacement element be the result of calling createElement(new name) on the ownerDocument of element.
    auto replacement_element = MUST(element->owner_document()->create_element(new_name.to_string(), DOM::ElementCreationOptions {}));

    // 4. Insert replacement element into element's parent immediately before element.
    element->parent()->insert_before(replacement_element, element);

    // 5. Copy all attributes of element to replacement element, in order.
    element->for_each_attribute([&replacement_element](FlyString const& name, String const& value) {
        MUST(replacement_element->set_attribute(name, value));
    });

    // 6. While element has children, append the first child of element as the last child of replacement element, preserving ranges.
    while (element->has_children())
        move_node_preserving_ranges(*element->first_child(), *replacement_element, replacement_element->child_count());

    // 7. Remove element from its parent.
    element->remove();

    // 8. Return replacement element.
    return replacement_element;
}

// https://w3c.github.io/editing/docs/execCommand/#specified-command-value
Optional<String> specified_command_value(GC::Ref<DOM::Element> element, FlyString const& command)
{
    // 1. If command is "backColor" or "hiliteColor" and the Element's display property does not have resolved value "inline", return null.
    auto layout_node = element->layout_node();
    if ((command == CommandNames::backColor || command == CommandNames::hiliteColor) && layout_node) {
        if (layout_node->computed_values().display().is_inline_outside())
            return {};
    }

    // 2. If command is "createLink" or "unlink":
    if (command == CommandNames::createLink || command == CommandNames::unlink) {
        // 1. If element is an a element and has an href attribute, return the value of that attribute.
        auto href_attribute = element->get_attribute(HTML::AttributeNames::href);
        if (href_attribute.has_value())
            return href_attribute.release_value();

        // 2. Return null.
        return {};
    }

    // 3. If command is "subscript" or "superscript":
    if (command == CommandNames::subscript || command == CommandNames::superscript) {
        // 1. If element is a sup, return "superscript".
        if (element->local_name() == HTML::TagNames::sup)
            return "superscript"_string;

        // 2. If element is a sub, return "subscript".
        if (element->local_name() == HTML::TagNames::sub)
            return "subscript"_string;

        // 3. Return null.
        return {};
    }

    // FIXME: 4. If command is "strikethrough", and element has a style attribute set, and that attribute sets "text-decoration":
    if (false) {
        // FIXME: 1. If element's style attribute sets "text-decoration" to a value containing "line-through", return "line-through".

        // 2. Return null.
        return {};
    }

    // 5. If command is "strikethrough" and element is an s or strike element, return "line-through".
    if (command == CommandNames::strikethrough && (element->local_name() == HTML::TagNames::s || element->local_name() == HTML::TagNames::strike))
        return "line-through"_string;

    // FIXME: 6. If command is "underline", and element has a style attribute set, and that attribute sets "text-decoration":
    if (false) {
        // FIXME: 1. If element's style attribute sets "text-decoration" to a value containing "underline", return "underline".

        // 2. Return null.
        return {};
    }

    // 7. If command is "underline" and element is a u element, return "underline".
    if (command == CommandNames::underline && element->local_name() == HTML::TagNames::u)
        return "underline"_string;

    // FIXME: 8. Let property be the relevant CSS property for command.

    // FIXME: 9. If property is null, return null.

    // FIXME: 10. If element has a style attribute set, and that attribute has the effect of setting property, return the value
    //     that it sets property to.

    // FIXME: 11. If element is a font element that has an attribute whose effect is to create a presentational hint for
    //     property, return the value that the hint sets property to. (For a size of 7, this will be the non-CSS value
    //     "xxx-large".)

    // FIXME: 12. If element is in the following list, and property is equal to the CSS property name listed for it, return the
    //     string listed for it.
    //     * b, strong: font-weight: "bold"
    //     * i, em: font-style: "italic"

    // 13. Return null.
    return {};
}

// https://w3c.github.io/editing/docs/execCommand/#split-the-parent
void split_the_parent_of_nodes(Vector<GC::Ref<DOM::Node>> const& nodes)
{
    VERIFY(nodes.size() > 0);

    // 1. Let original parent be the parent of the first member of node list.
    GC::Ref<DOM::Node> first_node = *nodes.first();
    GC::Ref<DOM::Node> last_node = *nodes.last();
    GC::Ref<DOM::Node> original_parent = *first_node->parent();

    // 2. If original parent is not editable or its parent is null, do nothing and abort these
    //    steps.
    if (!original_parent->is_editable() || !original_parent->parent())
        return;

    // 3. If the first child of original parent is in node list, remove extraneous line breaks
    //    before original parent.
    GC::Ref<DOM::Node> first_child = *original_parent->first_child();
    auto first_child_in_nodes_list = nodes.contains_slow(first_child);
    if (first_child_in_nodes_list)
        remove_extraneous_line_breaks_before_node(original_parent);

    // 4. If the first child of original parent is in node list, and original parent follows a line
    //    break, set follows line break to true. Otherwise, set follows line break to false.
    auto follows_line_break = first_child_in_nodes_list && follows_a_line_break(original_parent);

    // 5. If the last child of original parent is in node list, and original parent precedes a line
    //    break, set precedes line break to true. Otherwise, set precedes line break to false.
    GC::Ref<DOM::Node> last_child = *original_parent->last_child();
    bool last_child_in_nodes_list = nodes.contains_slow(last_child);
    auto precedes_line_break = last_child_in_nodes_list && precedes_a_line_break(original_parent);

    // 6. If the first child of original parent is not in node list, but its last child is:
    GC::Ref<DOM::Node> parent_of_original_parent = *original_parent->parent();
    auto original_parent_index = original_parent->index();
    auto& document = original_parent->document();
    if (!first_child_in_nodes_list && last_child_in_nodes_list) {
        // 1. For each node in node list, in reverse order, insert node into the parent of original
        //    parent immediately after original parent, preserving ranges.
        for (auto node : nodes.in_reverse())
            move_node_preserving_ranges(node, parent_of_original_parent, original_parent_index + 1);

        // 2. If precedes line break is true, and the last member of node list does not precede a
        //    line break, call createElement("br") on the context object and insert the result
        //    immediately after the last member of node list.
        if (precedes_line_break && !precedes_a_line_break(last_node)) {
            auto br_element = MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML));
            MUST(last_node->parent()->append_child(br_element));
        }

        // 3. Remove extraneous line breaks at the end of original parent.
        remove_extraneous_line_breaks_at_the_end_of_node(original_parent);

        // 4. Abort these steps.
        return;
    }

    // 7. If the first child of original parent is not in node list:
    if (!first_child_in_nodes_list) {
        // 1. Let cloned parent be the result of calling cloneNode(false) on original parent.
        auto cloned_parent = MUST(original_parent->clone_node(nullptr, false));

        // 2. If original parent has an id attribute, unset it.
        auto& original_parent_element = static_cast<DOM::Element&>(*original_parent);
        if (original_parent_element.has_attribute(HTML::AttributeNames::id))
            original_parent_element.remove_attribute(HTML::AttributeNames::id);

        // 3. Insert cloned parent into the parent of original parent immediately before original
        //    parent.
        original_parent->parent()->insert_before(cloned_parent, original_parent);

        // 4. While the previousSibling of the first member of node list is not null, append the
        //    first child of original parent as the last child of cloned parent, preserving ranges.
        while (first_node->previous_sibling())
            move_node_preserving_ranges(*original_parent->first_child(), cloned_parent, cloned_parent->child_count());
    }

    // 8. For each node in node list, insert node into the parent of original parent immediately
    //    before original parent, preserving ranges.
    for (auto node : nodes)
        move_node_preserving_ranges(node, parent_of_original_parent, original_parent_index - 1);

    // 9. If follows line break is true, and the first member of node list does not follow a line
    //    break, call createElement("br") on the context object and insert the result immediately
    //    before the first member of node list.
    if (follows_line_break && !follows_a_line_break(first_node)) {
        auto br_element = MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML));
        first_node->parent()->insert_before(br_element, first_node);
    }

    // 10. If the last member of node list is an inline node other than a br, and the first child of
    //     original parent is a br, and original parent is not an inline node, remove the first
    //     child of original parent from original parent.
    if (is_inline_node(last_node) && !is<HTML::HTMLBRElement>(*last_node) && is<HTML::HTMLBRElement>(*first_child) && !is_inline_node(original_parent))
        first_child->remove();

    // 11. If original parent has no children:
    if (original_parent->child_count() == 0) {
        // 1. Remove original parent from its parent.
        original_parent->remove();

        // 2. If precedes line break is true, and the last member of node list does not precede a
        //    line break, call createElement("br") on the context object and insert the result
        //    immediately after the last member of node list.
        if (precedes_line_break && !precedes_a_line_break(last_node)) {
            auto br_element = MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML));
            last_node->parent()->insert_before(br_element, last_node->next_sibling());
        }
    }

    // 12. Otherwise, remove extraneous line breaks before original parent.
    else {
        remove_extraneous_line_breaks_before_node(original_parent);
    }

    // 13. If node list's last member's nextSibling is null, but its parent is not null, remove
    //     extraneous line breaks at the end of node list's last member's parent.
    if (!last_node->next_sibling() && last_node->parent())
        remove_extraneous_line_breaks_at_the_end_of_node(*last_node->parent());
}

// https://w3c.github.io/editing/docs/execCommand/#wrap
GC::Ptr<DOM::Node> wrap(
    Vector<GC::Ref<DOM::Node>> node_list,
    Function<bool(GC::Ref<DOM::Node>)> sibling_criteria,
    Function<GC::Ptr<DOM::Node>()> new_parent_instructions)
{
    VERIFY(!node_list.is_empty());

    // If not provided, sibling criteria returns false and new parent instructions returns null.
    if (!sibling_criteria)
        sibling_criteria = [](auto) { return false; };
    if (!new_parent_instructions)
        new_parent_instructions = [] { return nullptr; };

    // 1. If every member of node list is invisible, and none is a br, return null and abort these steps.
    auto any_node_visible_or_br = false;
    for (auto& node : node_list) {
        if (is_visible_node(node) || is<HTML::HTMLBRElement>(*node)) {
            any_node_visible_or_br = true;
            break;
        }
    }
    if (!any_node_visible_or_br)
        return {};

    // 2. If node list's first member's parent is null, return null and abort these steps.
    if (!node_list.first()->parent())
        return {};

    // 3. If node list's last member is an inline node that's not a br, and node list's last member's nextSibling is a
    //    br, append that br to node list.
    auto last_member = node_list.last();
    if (is_inline_node(last_member) && !is<HTML::HTMLBRElement>(*last_member) && is<HTML::HTMLBRElement>(last_member->next_sibling()))
        node_list.append(*last_member->next_sibling());

    // 4. While node list's first member's previousSibling is invisible, prepend it to node list.
    while (node_list.first()->previous_sibling() && is_invisible_node(*node_list.first()->previous_sibling()))
        node_list.prepend(*node_list.first()->previous_sibling());

    // 5. While node list's last member's nextSibling is invisible, append it to node list.
    while (node_list.last()->next_sibling() && is_invisible_node(*node_list.last()->next_sibling()))
        node_list.append(*node_list.last()->next_sibling());

    auto new_parent = [&]() -> GC::Ptr<DOM::Node> {
        // 6. If the previousSibling of the first member of node list is editable and running sibling criteria on it returns
        //    true, let new parent be the previousSibling of the first member of node list.
        GC::Ptr<DOM::Node> previous_sibling = node_list.first()->previous_sibling();
        if (previous_sibling && previous_sibling->is_editable() && sibling_criteria(*previous_sibling))
            return previous_sibling;

        // 7. Otherwise, if the nextSibling of the last member of node list is editable and running sibling criteria on it
        //    returns true, let new parent be the nextSibling of the last member of node list.
        GC::Ptr<DOM::Node> next_sibling = node_list.last()->next_sibling();
        if (next_sibling && next_sibling->is_editable() && sibling_criteria(*next_sibling))
            return next_sibling;

        // 8. Otherwise, run new parent instructions, and let new parent be the result.
        return new_parent_instructions();
    }();

    // 9. If new parent is null, abort these steps and return null.
    if (!new_parent)
        return {};

    // 10. If new parent's parent is null:
    if (!new_parent->parent()) {
        // 1. Insert new parent into the parent of the first member of node list immediately before the first member of
        //    node list.
        auto first_member = node_list.first();
        first_member->parent()->insert_before(*new_parent, first_member);

        // FIXME: 2. If any range has a boundary point with node equal to the parent of new parent and offset equal to the
        //    index of new parent, add one to that boundary point's offset.
    }

    // 11. Let original parent be the parent of the first member of node list.
    auto const original_parent = GC::Ptr { node_list.first()->parent() };

    // 12. If new parent is before the first member of node list in tree order:
    if (new_parent->is_before(node_list.first())) {
        // 1. If new parent is not an inline node, but the last visible child of new parent and the first visible member
        //    of node list are both inline nodes, and the last child of new parent is not a br, call createElement("br")
        //    on the ownerDocument of new parent and append the result as the last child of new parent.
        if (!is_inline_node(*new_parent)) {
            auto last_visible_child = [&] -> GC::Ref<DOM::Node> {
                GC::Ptr<DOM::Node> child = new_parent->last_child();
                while (child) {
                    if (is_visible_node(*child))
                        return *child;
                    child = child->previous_sibling();
                }
                VERIFY_NOT_REACHED();
            }();
            auto first_visible_member = [&] -> GC::Ref<DOM::Node> {
                for (auto& member : node_list) {
                    if (is_visible_node(member))
                        return member;
                }
                VERIFY_NOT_REACHED();
            }();
            if (is_inline_node(last_visible_child) && is_inline_node(first_visible_member)
                && !is<HTML::HTMLBRElement>(new_parent->last_child())) {
                auto br_element = MUST(DOM::create_element(*new_parent->owner_document(), HTML::TagNames::br, Namespace::HTML));
                MUST(new_parent->append_child(br_element));
            }
        }

        // 2. For each node in node list, append node as the last child of new parent, preserving ranges.
        auto new_position = new_parent->child_count();
        for (auto& node : node_list)
            move_node_preserving_ranges(node, *new_parent, new_position++);
    }

    // 13. Otherwise:
    else {
        // 1. If new parent is not an inline node, but the first visible child of new parent and the last visible member
        //    of node list are both inline nodes, and the last member of node list is not a br, call createElement("br")
        //    on the ownerDocument of new parent and insert the result as the first child of new parent.
        if (!is_inline_node(*new_parent)) {
            auto first_visible_child = [&] -> GC::Ref<DOM::Node> {
                GC::Ptr<DOM::Node> child = new_parent->first_child();
                while (child) {
                    if (is_visible_node(*child))
                        return *child;
                    child = child->next_sibling();
                }
                VERIFY_NOT_REACHED();
            }();
            auto last_visible_member = [&] -> GC::Ref<DOM::Node> {
                for (auto& member : node_list.in_reverse()) {
                    if (is_visible_node(member))
                        return member;
                }
                VERIFY_NOT_REACHED();
            }();
            if (is_inline_node(first_visible_child) && is_inline_node(last_visible_member)
                && !is<HTML::HTMLBRElement>(*node_list.last())) {
                auto br_element = MUST(DOM::create_element(*new_parent->owner_document(), HTML::TagNames::br, Namespace::HTML));
                new_parent->insert_before(br_element, new_parent->first_child());
            }
        }

        // 2. For each node in node list, in reverse order, insert node as the first child of new parent, preserving
        //    ranges.
        for (auto& node : node_list.in_reverse())
            move_node_preserving_ranges(node, *new_parent, 0);
    }

    // 14. If original parent is editable and has no children, remove it from its parent.
    if (original_parent->is_editable() && !original_parent->has_children())
        original_parent->remove();

    // 15. If new parent's nextSibling is editable and running sibling criteria on it returns true:
    GC::Ptr<DOM::Node> next_sibling = new_parent->next_sibling();
    if (next_sibling && next_sibling->is_editable() && sibling_criteria(*next_sibling)) {
        // 1. If new parent is not an inline node, but new parent's last child and new parent's nextSibling's first
        //    child are both inline nodes, and new parent's last child is not a br, call createElement("br") on the
        //    ownerDocument of new parent and append the result as the last child of new parent.
        if (!is_inline_node(*new_parent) && is_inline_node(*new_parent->last_child())
            && is_inline_node(*next_sibling->first_child()) && !is<HTML::HTMLBRElement>(new_parent->last_child())) {
            auto br_element = MUST(DOM::create_element(*new_parent->owner_document(), HTML::TagNames::br, Namespace::HTML));
            MUST(new_parent->append_child(br_element));
        }

        // 2. While new parent's nextSibling has children, append its first child as the last child of new parent,
        //    preserving ranges.
        auto new_position = new_parent->child_count();
        while (next_sibling->has_children())
            move_node_preserving_ranges(*next_sibling->first_child(), *new_parent, new_position++);

        // 3. Remove new parent's nextSibling from its parent.
        next_sibling->remove();
    }

    // 16. Remove extraneous line breaks from new parent.
    remove_extraneous_line_breaks_from_a_node(*new_parent);

    // 17. Return new parent.
    return new_parent;
}

bool has_visible_children(GC::Ref<DOM::Node> node)
{
    bool has_visible_child = false;
    node->for_each_child([&has_visible_child](GC::Ref<DOM::Node> child) {
        if (is_visible_node(child)) {
            has_visible_child = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return has_visible_child;
}

bool is_heading(FlyString const& local_name)
{
    return local_name.is_one_of(
        HTML::TagNames::h1,
        HTML::TagNames::h2,
        HTML::TagNames::h3,
        HTML::TagNames::h4,
        HTML::TagNames::h5,
        HTML::TagNames::h6);
}

}
