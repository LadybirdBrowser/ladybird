/*
 * Copyright (c) 2024-2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Color.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/DocumentType.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/CommandNames.h>
#include <LibWeb/Editing/Commands.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLDivElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLFontElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLLIElement.h>
#include <LibWeb/HTML/HTMLOListElement.h>
#include <LibWeb/HTML/HTMLParagraphElement.h>
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

// https://w3c.github.io/editing/docs/execCommand/#active-range
GC::Ptr<DOM::Range> active_range(DOM::Document const& document)
{
    // The active range is the range of the selection given by calling getSelection() on the context object. (Thus the
    // active range may be null.)
    auto selection = document.get_selection();
    if (!selection)
        return {};
    return selection->range();
}

// https://w3c.github.io/editing/docs/execCommand/#alignment-value
JustifyAlignment alignment_value_of_node(GC::Ptr<DOM::Node> node)
{
    // 1. While node is neither null nor an Element, or it is an Element but its "display" property has resolved value
    //    "inline" or "none", set node to its parent.
    auto is_display_inline_or_none = [](GC::Ref<DOM::Node> node) {
        auto display = resolved_display(node);
        if (!display.has_value())
            return false;
        return (display.value().is_inline_outside() && display.value().is_flow_inside()) || display.value().is_none();
    };
    while ((node && !is<DOM::Element>(node.ptr())) || (is<DOM::Element>(node.ptr()) && is_display_inline_or_none(*node)))
        node = node->parent();

    // 2. If node is not an Element, return "left".
    if (!is<DOM::Element>(node.ptr()))
        return JustifyAlignment::Left;
    GC::Ref<DOM::Element> element = static_cast<DOM::Element&>(*node);

    // 3. If node's "text-align" property has resolved value "start", return "left" if the directionality of node is
    //    "ltr", "right" if it is "rtl".
    auto text_align_value = resolved_keyword(*node, CSS::PropertyID::TextAlign);
    if (!text_align_value.has_value())
        return JustifyAlignment::Left;
    if (text_align_value.value() == CSS::Keyword::Start)
        return element->directionality() == DOM::Element::Directionality::Ltr ? JustifyAlignment::Left : JustifyAlignment::Right;

    // 4. If node's "text-align" property has resolved value "end", return "right" if the directionality of node is
    //    "ltr", "left" if it is "rtl".
    if (text_align_value.value() == CSS::Keyword::End)
        return element->directionality() == DOM::Element::Directionality::Ltr ? JustifyAlignment::Right : JustifyAlignment::Left;

    // 5. If node's "text-align" property has resolved value "center", "justify", "left", or "right", return that value.
    switch (text_align_value.value()) {
    case CSS::Keyword::Center:
        return JustifyAlignment::Center;
    case CSS::Keyword::Justify:
        return JustifyAlignment::Justify;
    case CSS::Keyword::Left:
        return JustifyAlignment::Left;
    case CSS::Keyword::Right:
        return JustifyAlignment::Right;
    default:
        // 6. Return "left".
        return JustifyAlignment::Left;
    }
}

// https://w3c.github.io/editing/docs/execCommand/#autolink
void autolink(DOM::BoundaryPoint point)
{
    // 1. While (node, end offset)'s previous equivalent point is not null, set it to its previous equivalent point.
    while (true) {
        auto previous_point = previous_equivalent_point(point);
        if (!previous_point.has_value())
            break;
        point = previous_point.release_value();
    }

    // 2. If node is not a Text node, or has an a ancestor, do nothing and abort these steps.
    if (!is<DOM::Text>(*point.node) || point.node->first_ancestor_of_type<HTML::HTMLAnchorElement>())
        return;

    // FIXME: 3. Let search be the largest substring of node's data whose end is end offset and that contains no space
    //    characters.

    // FIXME: 4. If some substring of search is an autolinkable URL:
    String href;
    if (false) {
        // FIXME: 1. While there is no substring of node's data ending at end offset that is an autolinkable URL, decrement end
        //    offset.

        // FIXME: 2. Let start offset be the start index of the longest substring of node's data that is an autolinkable URL
        //    ending at end offset.

        // FIXME: 3. Let href be the substring of node's data starting at start offset and ending at end offset.
    }

    // FIXME: 5. Otherwise, if some substring of search is a valid e-mail address:
    else if (false) {
        // FIXME: 1. While there is no substring of node's data ending at end offset that is a valid e-mail address, decrement
        //    end offset.

        // FIXME: 2. Let start offset be the start index of the longest substring of node's data that is a valid e-mail address
        //    ending at end offset.

        // FIXME: 3. Let href be "mailto:" concatenated with the substring of node's data starting at start offset and ending
        //    at end offset.
    }

    // 6. Otherwise, do nothing and abort these steps.
    else {
        return;
    }

    // 7. Let original range be the active range.
    auto& document = point.node->document();
    auto original_range = active_range(document);

    // FIXME: 8. Create a new range with start (node, start offset) and end (node, end offset), and set the context object's
    //    selection's range to it.

    // 9. Take the action for "createLink", with value equal to href.
    take_the_action_for_command(document, CommandNames::createLink, href);

    // 10. Set the context object's selection's range to original range.
    if (original_range)
        document.get_selection()->add_range(*original_range);
}

// https://w3c.github.io/editing/docs/execCommand/#block-extend
GC::Ref<DOM::Range> block_extend_a_range(GC::Ref<DOM::Range> range)
{
    // 1. Let start node, start offset, end node, and end offset be the start and end nodes and offsets of range.
    GC::Ptr<DOM::Node> start_node = range->start_container();
    auto start_offset = range->start_offset();
    GC::Ptr<DOM::Node> end_node = range->end_container();
    auto end_offset = range->end_offset();

    // 2. If some inclusive ancestor of start node is an li, set start offset to the index of the last such li in tree
    //    order, and set start node to that li's parent.
    start_node->for_each_inclusive_ancestor([&](GC::Ref<DOM::Node> ancestor) {
        if (is<HTML::HTMLLIElement>(*ancestor)) {
            start_offset = ancestor->index();
            start_node = ancestor->parent();
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });

    // 3. If (start node, start offset) is not a block start point, repeat the following steps:
    if (!is_block_start_point({ *start_node, start_offset })) {
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
        } while (!is_block_boundary_point({ *start_node, start_offset }));
    }

    // 4. While start offset is zero and start node's parent is not null, set start offset to start node's index, then
    //    set start node to its parent.
    while (start_offset == 0 && start_node->parent()) {
        start_offset = start_node->index();
        start_node = start_node->parent();
    }

    // 5. If some inclusive ancestor of end node is an li, set end offset to one plus the index of the last such li in
    //    tree order, and set end node to that li's parent.
    end_node->for_each_inclusive_ancestor([&](GC::Ref<DOM::Node> ancestor) {
        if (is<HTML::HTMLLIElement>(*ancestor)) {
            end_offset = ancestor->index() + 1;
            end_node = ancestor->parent();
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });

    // 6. If (end node, end offset) is not a block end point, repeat the following steps:
    if (!is_block_end_point({ *end_node, end_offset })) {
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
        } while (!is_block_boundary_point({ *end_node, end_offset }));
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
void canonicalize_whitespace(DOM::BoundaryPoint boundary, bool fix_collapsed_space)
{
    auto node = boundary.node;
    auto offset = boundary.offset;

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
        if (is<DOM::Text>(*start_node) && start_offset != 0) {
            auto parent_white_space = resolved_keyword(*start_node->parent(), CSS::PropertyID::WhiteSpace);

            // FIXME: Find a way to get code points directly from the UTF-8 string
            auto start_node_data = *start_node->text_content();
            auto utf16_code_units = MUST(AK::utf8_to_utf16(start_node_data));
            auto offset_minus_one_code_point = Utf16View { utf16_code_units }.code_point_at(start_offset - 1);
            if (parent_white_space != CSS::Keyword::Pre && parent_white_space != CSS::Keyword::PreWrap
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
        if (is<DOM::Text>(*end_node) && end_offset != end_node->length()) {
            auto parent_white_space = resolved_keyword(*end_node->parent(), CSS::PropertyID::WhiteSpace);

            // FIXME: Find a way to get code points directly from the UTF-8 string
            auto end_node_data = *end_node->text_content();
            auto utf16_code_units = MUST(AK::utf8_to_utf16(end_node_data));
            auto offset_code_point = Utf16View { utf16_code_units }.code_point_at(end_offset);
            if (parent_white_space != CSS::Keyword::Pre && parent_white_space != CSS::Keyword::PreWrap
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
            auto relative_position = DOM::position_of_boundary_point_relative_to_other_boundary_point({ *start_node, start_offset }, { *end_node, end_offset });
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
            if (is<DOM::Text>(*end_node) && end_offset == end_node->length() && precedes_a_line_break(end_node)) {
                auto parent_white_space = resolved_keyword(*end_node->parent(), CSS::PropertyID::WhiteSpace);
                if (parent_white_space != CSS::Keyword::Pre && parent_white_space != CSS::Keyword::PreWrap
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
        auto relative_position = DOM::position_of_boundary_point_relative_to_other_boundary_point({ start_node, start_offset }, { end_node, end_offset });
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

// https://w3c.github.io/editing/docs/execCommand/#clear-the-value
Vector<GC::Ref<DOM::Node>> clear_the_value(FlyString const& command, GC::Ref<DOM::Element> element)
{
    // 1. Let command be the current command.

    // 2. If element is not editable, return the empty list.
    if (!element->is_editable())
        return {};

    // 3. If element's specified command value for command is null, return the empty list.
    if (!specified_command_value(element, command).has_value())
        return {};

    // 4. If element is a simple modifiable element:
    if (is_simple_modifiable_element(element)) {
        // 1. Let children be the children of element.
        Vector<GC::Ref<DOM::Node>> children;
        element->for_each_child([&children](DOM::Node& child) {
            children.append(child);
            return IterationDecision::Continue;
        });

        // 2. For each child in children, insert child into element's parent immediately before element, preserving
        //    ranges.
        auto element_index = element->index();
        for (auto child : children)
            move_node_preserving_ranges(child, *element->parent(), element_index++);

        // 3. Remove element from its parent.
        element->remove();

        // 4. Return children.
        return children;
    }

    // 5. If command is "strikethrough", and element has a style attribute that sets "text-decoration" to some value
    //    containing "line-through", delete "line-through" from the value.
    auto remove_text_decoration_value = [&element](CSS::Keyword keyword_to_delete) {
        auto inline_style = element->inline_style();
        if (!inline_style)
            return;

        auto style_property = inline_style->property(CSS::PropertyID::TextDecoration);
        if (!style_property.has_value())
            return;

        auto style_value = style_property.value().value;
        VERIFY(style_value->is_value_list());
        auto const& value_list = style_value->as_value_list();
        auto& old_values = value_list.values();

        auto new_values = old_values;
        auto was_removed = new_values.remove_all_matching([&](CSS::ValueComparingNonnullRefPtr<CSS::CSSStyleValue const> const& value) {
            return value->is_keyword() && value->as_keyword().keyword() == keyword_to_delete;
        });
        if (!was_removed)
            return;
        if (new_values.is_empty()) {
            MUST(inline_style->remove_property(string_from_property_id(CSS::PropertyID::TextDecoration)));
            return;
        }

        auto new_style_value = CSS::StyleValueList::create(move(new_values), value_list.separator());
        MUST(inline_style->set_property(
            string_from_property_id(CSS::PropertyID::TextDecoration),
            new_style_value->to_string(CSS::CSSStyleValue::SerializationMode::Normal),
            {}));
    };
    if (command == CommandNames::strikethrough)
        remove_text_decoration_value(CSS::Keyword::LineThrough);

    // 6. If command is "underline", and element has a style attribute that sets "text-decoration" to some value
    //    containing "underline", delete "underline" from the value.
    if (command == CommandNames::underline)
        remove_text_decoration_value(CSS::Keyword::Underline);

    // 7. If the relevant CSS property for command is not null, unset that property of element.
    auto command_definition = find_command_definition(command);
    // FIXME: remove command_definition.has_value() as soon as all commands are implemented.
    if (command_definition.has_value() && command_definition.value().relevant_css_property.has_value()) {
        auto property_to_remove = command_definition.value().relevant_css_property.value();
        if (auto inline_style = element->inline_style())
            MUST(inline_style->remove_property(string_from_property_id(property_to_remove)));
    }

    // 8. If element is a font element:
    if (is<HTML::HTMLFontElement>(*element)) {
        // 1. If command is "foreColor", unset element's color attribute, if set.
        if (command == CommandNames::foreColor)
            element->remove_attribute(HTML::AttributeNames::color);

        // 2. If command is "fontName", unset element's face attribute, if set.
        if (command == CommandNames::fontName)
            element->remove_attribute(HTML::AttributeNames::face);

        // 3. If command is "fontSize", unset element's size attribute, if set.
        if (command == CommandNames::fontSize)
            element->remove_attribute(HTML::AttributeNames::size);
    }

    // 9. If element is an a element and command is "createLink" or "unlink", unset the href property of element.
    if (is<HTML::HTMLAnchorElement>(*element) && command.is_one_of(CommandNames::createLink, CommandNames::unlink))
        element->remove_attribute(HTML::AttributeNames::href);

    // 10. If element's specified command value for command is null, return the empty list.
    if (!specified_command_value(element, command).has_value())
        return {};

    // 11. Set the tag name of element to "span", and return the one-node list consisting of the result.
    return { set_the_tag_name(element, HTML::TagNames::span) };
}

// https://w3c.github.io/editing/docs/execCommand/#delete-the-selection
void delete_the_selection(Selection& selection, bool block_merging, bool strip_wrappers, Selection::Direction direction)
{
    auto& document = *selection.document();

    // 1. If the active range is null, abort these steps and do nothing.
    // NOTE: The selection is collapsed often in this algorithm, so we shouldn't store the active range in a variable.
    if (!active_range(document))
        return;

    // 2. Canonicalize whitespace at the active range's start.
    canonicalize_whitespace(active_range(document)->start());

    // 3. Canonicalize whitespace at the active range's end.
    canonicalize_whitespace(active_range(document)->end());

    // 4. Let (start node, start offset) be the last equivalent point for the active range's start.
    auto start = last_equivalent_point(active_range(document)->start());

    // 5. Let (end node, end offset) be the first equivalent point for the active range's end.
    auto end = first_equivalent_point(active_range(document)->end());

    // 6. If (end node, end offset) is not after (start node, start offset):
    auto relative_position = DOM::position_of_boundary_point_relative_to_other_boundary_point({ end.node, end.offset }, { start.node, start.offset });
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
    GC::Ptr<DOM::Node> start_block = active_range(document)->start_container();

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
    GC::Ptr<DOM::Node> end_block = active_range(document)->end_container();

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
    auto overrides = record_current_states_and_values(document);

    // 21. If start node and end node are the same, and start node is an editable Text node:
    if (start.node == end.node && is<DOM::Text>(*start.node) && start.node->is_editable()) {
        // 1. Call deleteData(start offset, end offset − start offset) on start node.
        MUST(static_cast<DOM::Text&>(*start.node).delete_data(start.offset, end.offset - start.offset));

        // 2. Canonicalize whitespace at (start node, start offset), with fix collapsed space false.
        canonicalize_whitespace(start, false);

        // 3. If direction is "forward", call collapseToStart() on the context object's selection.
        if (direction == Selection::Direction::Forwards) {
            MUST(selection.collapse_to_start());
        }

        // 4. Otherwise, call collapseToEnd() on the context object's selection.
        else {
            MUST(selection.collapse_to_end());
        }

        // 5. Restore states and values from overrides.
        restore_states_and_values(document, overrides);

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
    active_range(document)->for_each_contained([&node_list](GC::Ref<DOM::Node> node) {
        if (!node_list.is_empty() && node_list.last()->is_ancestor_of(node))
            return IterationDecision::Continue;

        if (!node->is_editable())
            return IterationDecision::Continue;

        if (!is<HTML::HTMLTableSectionElement>(*node) && !is<HTML::HTMLTableRowElement>(*node) && !is<HTML::HTMLTableCellElement>(*node))
            node_list.append(node);

        return IterationDecision::Continue;
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
    canonicalize_whitespace(active_range(document)->start(), false);

    // 28. Canonicalize whitespace at the active range's end, with fix collapsed space false.
    canonicalize_whitespace(active_range(document)->end(), false);

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
        restore_states_and_values(document, overrides);

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
            restore_states_and_values(document, overrides);

            // 5. Abort these steps.
            return;
        }

        // 5. If end block's firstChild is not an inline node, restore states and values from record, then abort these
        //    steps.
        if (!is_inline_node(*end_block->first_child())) {
            restore_states_and_values(document, overrides);
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
        bool has_valid_ol_or_ul_ancestor = false;
        ancestor->for_each_inclusive_ancestor([&](GC::Ref<DOM::Node> inclusive_ancestor) {
            if (inclusive_ancestor->next_sibling() && is_in_same_editing_host(*ancestor, *inclusive_ancestor)
                && is_in_same_editing_host(*inclusive_ancestor, *inclusive_ancestor->next_sibling())
                && ((is<HTML::HTMLOListElement>(*inclusive_ancestor) && is<HTML::HTMLOListElement>(*inclusive_ancestor->next_sibling()))
                    || (is<HTML::HTMLUListElement>(*inclusive_ancestor) && is<HTML::HTMLUListElement>(*inclusive_ancestor->next_sibling())))) {
                has_valid_ol_or_ul_ancestor = true;
                return IterationDecision::Break;
            }
            return IterationDecision::Continue;
        });
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
    restore_states_and_values(document, overrides);
}

// https://w3c.github.io/editing/docs/execCommand/#editing-host-of
GC::Ptr<DOM::Node> editing_host_of_node(GC::Ref<DOM::Node> node)
{
    // node itself, if node is an editing host;
    if (node->is_editing_host())
        return node;

    // or the nearest ancestor of node that is an editing host, if node is editable.
    if (node->is_editable()) {
        GC::Ptr<DOM::Node> result;
        node->for_each_ancestor([&result](GC::Ref<DOM::Node> ancestor) {
            if (ancestor->is_editing_host()) {
                result = ancestor;
                return IterationDecision::Break;
            }
            return IterationDecision::Continue;
        });
        VERIFY(result);
        return result;
    }

    // The editing host of node is null if node is neither editable nor an editing host;
    return {};
}

// https://w3c.github.io/editing/docs/execCommand/#effective-command-value
Optional<String> effective_command_value(GC::Ptr<DOM::Node> node, FlyString const& command)
{
    VERIFY(node);

    // 1. If neither node nor its parent is an Element, return null.
    // 2. If node is not an Element, return the effective command value of its parent for command.
    if (!is<DOM::Element>(*node)) {
        if (!node->parent() || !is<DOM::Element>(*node->parent()))
            return {};
        return effective_command_value(node->parent(), command);
    }

    // 3. If command is "createLink" or "unlink":
    auto node_as_element = [&] -> GC::Ref<DOM::Element> { return static_cast<DOM::Element&>(*node); };
    if (command.is_one_of(CommandNames::createLink, CommandNames::unlink)) {
        // 1. While node is not null, and is not an a element that has an href attribute, set node to its parent.
        while (node && !(is<HTML::HTMLAnchorElement>(*node) && node_as_element()->has_attribute(HTML::AttributeNames::href)))
            node = node->parent();

        // 2. If node is null, return null.
        if (!node)
            return {};

        // 3. Return the value of node's href attribute.
        return node_as_element()->get_attribute_value(HTML::AttributeNames::href);
    }

    // 4. If command is "backColor" or "hiliteColor":
    if (command.is_one_of(CommandNames::backColor, CommandNames::hiliteColor)) {
        // 1. While the resolved value of "background-color" on node is any fully transparent value, and node's parent
        //    is an Element, set node to its parent.
        auto resolved_background_color = [&] { return resolved_value(*node, CSS::PropertyID::BackgroundColor); };
        auto resolved_background_alpha = [&] {
            auto background_color = resolved_background_color();
            if (!background_color.has_value())
                return NumericLimits<u8>::max();
            VERIFY(is<Layout::NodeWithStyle>(node->layout_node()));
            return background_color.value()->to_color(*static_cast<Layout::NodeWithStyle*>(node->layout_node())).alpha();
        };
        while (resolved_background_alpha() == 0 && node->parent() && is<DOM::Element>(*node->parent()))
            node = node->parent();

        // 2. Return the resolved value of "background-color" for node.
        auto resolved_value = resolved_background_color();
        if (!resolved_value.has_value())
            return {};
        return resolved_value.value()->to_string(CSS::CSSStyleValue::SerializationMode::ResolvedValue);
    }

    // 5. If command is "subscript" or "superscript":
    if (command.is_one_of(CommandNames::subscript, CommandNames::superscript)) {
        // 1. Let affected by subscript and affected by superscript be two boolean variables, both initially false.
        bool affected_by_subscript = false;
        bool affected_by_superscript = false;

        // 2. While node is an inline node:
        while (node && is_inline_node(*node)) {
            // 1. If node is a sub, set affected by subscript to true.
            if (is<DOM::Element>(*node) && node_as_element()->local_name() == HTML::TagNames::sub) {
                affected_by_subscript = true;
            }

            // 2. Otherwise, if node is a sup, set affected by superscript to true.
            else if (is<DOM::Element>(*node) && node_as_element()->local_name() == HTML::TagNames::sup) {
                affected_by_superscript = true;
            }

            // 3. Set node to its parent.
            node = node->parent();
        }

        // 3. If affected by subscript and affected by superscript are both true, return the string "mixed".
        if (affected_by_subscript && affected_by_superscript)
            return "mixed"_string;

        // 4. If affected by subscript is true, return "subscript".
        if (affected_by_subscript)
            return "subscript"_string;

        // 5. If affected by superscript is true, return "superscript".
        if (affected_by_superscript)
            return "superscript"_string;

        // 6. Return null.
        return {};
    }

    // 6. If command is "strikethrough", and the "text-decoration" property of node or any of its ancestors has resolved
    //    value containing "line-through", return "line-through". Otherwise, return null.
    if (command == CommandNames::strikethrough) {
        auto inclusive_ancestor = node;
        do {
            auto text_decoration_line = resolved_value(*node, CSS::PropertyID::TextDecorationLine);
            if (text_decoration_line.has_value() && value_contains_keyword(text_decoration_line.value(), CSS::Keyword::LineThrough))
                return "line-through"_string;
            inclusive_ancestor = inclusive_ancestor->parent();
        } while (inclusive_ancestor);

        return {};
    }

    // 7. If command is "underline", and the "text-decoration" property of node or any of its ancestors has resolved
    //    value containing "underline", return "underline". Otherwise, return null.
    if (command == CommandNames::underline) {
        auto inclusive_ancestor = node;
        do {
            auto text_decoration_line = resolved_value(*node, CSS::PropertyID::TextDecorationLine);
            if (text_decoration_line.has_value() && value_contains_keyword(text_decoration_line.value(), CSS::Keyword::Underline))
                return "underline"_string;
            inclusive_ancestor = inclusive_ancestor->parent();
        } while (inclusive_ancestor);

        return {};
    }

    // 8. Return the resolved value for node of the relevant CSS property for command.
    auto optional_command_definition = find_command_definition(command);
    // FIXME: change this to VERIFY(command_definition.has_value()) once all command definitions are in place.
    if (!optional_command_definition.has_value())
        return {};
    auto const& command_definition = optional_command_definition.release_value();
    VERIFY(command_definition.relevant_css_property.has_value());

    auto optional_value = resolved_value(*node, command_definition.relevant_css_property.value());
    if (!optional_value.has_value())
        return {};
    return optional_value.value()->to_string(CSS::CSSStyleValue::SerializationMode::ResolvedValue);
}

// https://w3c.github.io/editing/docs/execCommand/#first-equivalent-point
DOM::BoundaryPoint first_equivalent_point(DOM::BoundaryPoint boundary_point)
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
    node->for_each_ancestor([&](GC::Ref<DOM::Node> ancestor) {
        if (is_in_same_editing_host(ancestor, node) && is_allowed_child_of_node(node, ancestor)) {
            allowed_child_of_any_ancestor = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
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
        if (!is_allowed_child_of_node(HTML::TagNames::p, GC::Ref { *editing_host_of_node(node) }))
            return;

        // 3. If node is not a prohibited paragraph child, abort these steps.
        if (!is_prohibited_paragraph_child(node))
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
    auto values = record_the_values_of_nodes({ node });

    // 4. While node is not an allowed child of its parent, split the parent of the one-node list consisting of node.
    while (!is_allowed_child_of_node(node, GC::Ref { *node->parent() }))
        split_the_parent_of_nodes({ node });

    // 5. Restore the values from values.
    restore_the_values_of_nodes(values);
}

// https://w3c.github.io/editing/docs/execCommand/#follows-a-line-break
bool follows_a_line_break(GC::Ref<DOM::Node> node)
{
    // 1. Let offset be zero.
    auto offset = 0u;

    // 2. While (node, offset) is not a block boundary point:
    while (!is_block_boundary_point({ node, offset })) {
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

// https://w3c.github.io/editing/docs/execCommand/#force-the-value
void force_the_value(GC::Ref<DOM::Node> node, FlyString const& command, Optional<String> new_value)
{
    // 1. Let command be the current command.

    // 2. If node's parent is null, abort this algorithm.
    if (!node->parent())
        return;

    // 3. If new value is null, abort this algorithm.
    if (!new_value.has_value())
        return;

    // 4. If node is an allowed child of "span":
    if (is_allowed_child_of_node(node, HTML::TagNames::span)) {
        // 1. Reorder modifiable descendants of node's previousSibling.
        if (node->previous_sibling())
            reorder_modifiable_descendants(*node->previous_sibling(), command, new_value);

        // 2. Reorder modifiable descendants of node's nextSibling.
        if (node->next_sibling())
            reorder_modifiable_descendants(*node->next_sibling(), command, new_value);

        // 3. Wrap the one-node list consisting of node, with sibling criteria returning true for a simple modifiable
        //    element whose specified command value is equivalent to new value and whose effective command value is
        //    loosely equivalent to new value and false otherwise, and with new parent instructions returning null.
        wrap(
            { node },
            [&](GC::Ref<DOM::Node> sibling) {
                return is_simple_modifiable_element(sibling)
                    && specified_command_value(static_cast<DOM::Element&>(*sibling), command) == new_value
                    && values_are_loosely_equivalent(command, effective_command_value(sibling, command), new_value);
            },
            [] -> GC::Ptr<DOM::Node> { return {}; });
    }

    // 5. If node is invisible, abort this algorithm.
    if (is_invisible_node(node))
        return;

    // 6. If the effective command value of command is loosely equivalent to new value on node, abort this algorithm.
    if (values_are_loosely_equivalent(command, effective_command_value(node, command), new_value))
        return;

    // 7. If node is not an allowed child of "span":
    if (!is_allowed_child_of_node(node, HTML::TagNames::span)) {
        // 1. Let children be all children of node, omitting any that are Elements whose specified command value for
        //    command is neither null nor equivalent to new value.
        Vector<GC::Ref<DOM::Node>> children;
        node->for_each_child([&](GC::Ref<DOM::Node> child) {
            if (is<DOM::Element>(*child)) {
                auto const& child_specified_value = specified_command_value(static_cast<DOM::Element&>(*child), command);
                if (child_specified_value.has_value() && !values_are_equivalent(command, child_specified_value.value(), new_value))
                    return IterationDecision::Continue;
            }

            children.append(child);
            return IterationDecision::Continue;
        });

        // 2. Force the value of each node in children, with command and new value as in this invocation of the
        //    algorithm.
        for (auto child : children)
            force_the_value(child, command, new_value);

        // 3. Abort this algorithm.
        return;
    }

    // 8. If the effective command value of command is loosely equivalent to new value on node, abort this algorithm.
    if (values_are_loosely_equivalent(command, effective_command_value(node, command), new_value))
        return;

    // 9. Let new parent be null.
    GC::Ptr<DOM::Element> new_parent;

    // 10. If the CSS styling flag is false:
    auto& document = node->document();
    if (!document.css_styling_flag()) {
        // 1. If command is "bold" and new value is "bold", let new parent be the result of calling createElement("b")
        //    on the ownerDocument of node.
        if (command == CommandNames::bold && new_value == "bold"sv)
            new_parent = MUST(DOM::create_element(document, HTML::TagNames::b, Namespace::HTML));

        // 2. If command is "italic" and new value is "italic", let new parent be the result of calling
        //    createElement("i") on the ownerDocument of node.
        if (command == CommandNames::italic && new_value == "italic"sv)
            new_parent = MUST(DOM::create_element(document, HTML::TagNames::i, Namespace::HTML));

        // 3. If command is "strikethrough" and new value is "line-through", let new parent be the result of calling
        //    createElement("s") on the ownerDocument of node.
        if (command == CommandNames::strikethrough && new_value == "line-through"sv)
            new_parent = MUST(DOM::create_element(document, HTML::TagNames::s, Namespace::HTML));

        // 4. If command is "underline" and new value is "underline", let new parent be the result of calling
        //    createElement("u") on the ownerDocument of node.
        if (command == CommandNames::underline && new_value == "underline"sv)
            new_parent = MUST(DOM::create_element(document, HTML::TagNames::u, Namespace::HTML));

        // 5.  If command is "foreColor", and new value is fully opaque with red, green, and blue components in the
        //     range 0 to 255:
        if (command == CommandNames::foreColor) {
            auto new_value_color = Color::from_string(new_value.value());
            if (new_value_color->alpha() == NumericLimits<u8>::max()) {
                // 1. Let new parent be the result of calling createElement("font") on the ownerDocument of node.
                new_parent = MUST(DOM::create_element(document, HTML::TagNames::font, Namespace::HTML));

                // 2. Set the color attribute of new parent to the result of applying the rules for serializing simple color
                //    values to new value (interpreted as a simple color).
                MUST(new_parent->set_attribute(HTML::AttributeNames::color, new_value_color->to_string_without_alpha()));
            }
        }

        // 6. If command is "fontName", let new parent be the result of calling createElement("font") on the
        //    ownerDocument of node, then set the face attribute of new parent to new value.
        if (command == CommandNames::fontName) {
            new_parent = MUST(DOM::create_element(document, HTML::TagNames::font, Namespace::HTML));
            MUST(new_parent->set_attribute(HTML::AttributeNames::face, new_value.value()));
        }
    }

    // 11. If command is "createLink" or "unlink":
    if (command.is_one_of(CommandNames::createLink, CommandNames::unlink)) {
        // 1. Let new parent be the result of calling createElement("a") on the ownerDocument of node.
        new_parent = MUST(DOM::create_element(document, HTML::TagNames::a, Namespace::HTML));

        // 2. Set the href attribute of new parent to new value.
        MUST(new_parent->set_attribute(HTML::AttributeNames::href, new_value.value()));

        // 3. Let ancestor be node's parent.
        GC::Ptr<DOM::Node> ancestor = node->parent();

        // 4. While ancestor is not null:
        while (ancestor) {
            // 1. If ancestor is an a, set the tag name of ancestor to "span", and let ancestor be the result.
            if (is<HTML::HTMLAnchorElement>(*ancestor))
                ancestor = set_the_tag_name(static_cast<DOM::Element&>(*ancestor), HTML::TagNames::span);

            // 2. Set ancestor to its parent.
            ancestor = ancestor->parent();
        }
    }

    // 12. If command is "fontSize"; and new value is one of "x-small", "small", "medium", "large", "x-large",
    //     "xx-large", or "xxx-large"; and either the CSS styling flag is false, or new value is "xxx-large":
    auto const& font_sizes = named_font_sizes();
    if (command == CommandNames::fontSize && font_sizes.contains_slow(new_value.value())
        && (!document.css_styling_flag() || new_value == "xxx-large"sv)) {
        // let new parent be the result of calling createElement("font") on the ownerDocument of node,
        new_parent = MUST(DOM::create_element(document, HTML::TagNames::font, Namespace::HTML));

        // then set the size attribute of new parent to the number from the following table based on new value:
        // * x-small: 1
        // * small: 2
        // * normal: 3
        // * large: 4
        // * x-large: 5
        // * xx-large: 6
        // * xxx-large: 7
        auto size = font_sizes.first_index_of(new_value.value()).value() + 1;
        MUST(new_parent->set_attribute(HTML::AttributeNames::size, String::number(size)));
    }

    // 13. If command is "subscript" or "superscript" and new value is "subscript", let new parent be the result of
    //     calling createElement("sub") on the ownerDocument of node.
    if (command.is_one_of(CommandNames::subscript, CommandNames::superscript) && new_value == "subscript"sv)
        new_parent = MUST(DOM::create_element(document, HTML::TagNames::sub, Namespace::HTML));

    // 14. If command is "subscript" or "superscript" and new value is "superscript", let new parent be the result of
    //     calling createElement("sup") on the ownerDocument of node.
    if (command.is_one_of(CommandNames::subscript, CommandNames::superscript) && new_value == "superscript"sv)
        new_parent = MUST(DOM::create_element(document, HTML::TagNames::sup, Namespace::HTML));

    // 15. If new parent is null, let new parent be the result of calling createElement("span") on the ownerDocument of
    //     node.
    if (!new_parent)
        new_parent = MUST(DOM::create_element(document, HTML::TagNames::span, Namespace::HTML));

    // 16. Insert new parent in node's parent before node.
    node->parent()->insert_before(*new_parent, node);

    // 17. If the effective command value of command for new parent is not loosely equivalent to new value, and the
    //     relevant CSS property for command is not null, set that CSS property of new parent to new value (if the new
    //     value would be valid).
    if (!values_are_loosely_equivalent(command, effective_command_value(new_parent, command), new_value)) {
        auto const& command_definition = find_command_definition(command);
        if (command_definition.has_value() && command_definition.value().relevant_css_property.has_value()) {
            auto inline_style = new_parent->style_for_bindings();
            MUST(inline_style->set_property(command_definition.value().relevant_css_property.value(), new_value.value()));
        }
    }

    // 18. If command is "strikethrough", and new value is "line-through", and the effective command value of
    //     "strikethrough" for new parent is not "line-through", set the "text-decoration" property of new parent to
    //     "line-through".
    if (command == CommandNames::strikethrough && new_value == "line-through"sv
        && effective_command_value(new_parent, command) != "line-through"sv) {
        auto inline_style = new_parent->style_for_bindings();
        MUST(inline_style->set_property(CSS::PropertyID::TextDecoration, "line-through"sv));
    }

    // 19. If command is "underline", and new value is "underline", and the effective command value of "underline" for
    //     new parent is not "underline", set the "text-decoration" property of new parent to "underline".
    if (command == CommandNames::underline && new_value == "underline"sv
        && effective_command_value(new_parent, command) != "underline"sv) {
        auto inline_style = new_parent->style_for_bindings();
        MUST(inline_style->set_property(CSS::PropertyID::TextDecoration, "underline"sv));
    }

    // 20. Append node to new parent as its last child, preserving ranges.
    move_node_preserving_ranges(node, *new_parent, new_parent->child_count());

    // 21. If node is an Element and the effective command value of command for node is not loosely equivalent to new
    //     value:
    if (is<DOM::Element>(*node) && !values_are_loosely_equivalent(command, effective_command_value(node, command), new_value)) {
        // 1. Insert node into the parent of new parent before new parent, preserving ranges.
        move_node_preserving_ranges(node, *new_parent->parent(), new_parent->index());

        // 2. Remove new parent from its parent.
        new_parent->remove();

        // 3. Let children be all children of node, omitting any that are Elements whose specified command value for
        //    command is neither null nor equivalent to new value.
        Vector<GC::Ref<DOM::Node>> children;
        node->for_each_child([&](GC::Ref<DOM::Node> child) {
            if (is<DOM::Element>(*child)) {
                auto child_value = specified_command_value(static_cast<DOM::Element&>(*child), command);
                if (child_value.has_value() && !values_are_equivalent(command, child_value.value(), new_value))
                    return IterationDecision::Continue;
            }

            children.append(child);
            return IterationDecision::Continue;
        });

        // 4. Force the value of each node in children, with command and new value as in this invocation of the
        //    algorithm.
        for (auto child : children)
            force_the_value(child, command, new_value);
    }
}

// https://w3c.github.io/editing/docs/execCommand/#indent
void indent(Vector<GC::Ref<DOM::Node>> node_list)
{
    // 1. If node list is empty, do nothing and abort these steps.
    if (node_list.is_empty())
        return;

    // 2. Let first node be the first member of node list.
    auto first_node = node_list.first();

    // 3. If first node's parent is an ol or ul:
    if (is<HTML::HTMLOListElement>(first_node->parent()) || is<HTML::HTMLUListElement>(first_node->parent())) {
        // 1. Let tag be the local name of the parent of first node.
        auto tag = static_cast<DOM::Element*>(first_node->parent())->local_name();

        // 2. Wrap node list, with sibling criteria returning true for an HTML element with local name tag and false
        //    otherwise, and new parent instructions returning the result of calling createElement(tag) on the
        //    ownerDocument of first node.
        wrap(
            node_list,
            [&](GC::Ref<DOM::Node> sibling) {
                return is<DOM::Element>(*sibling) && static_cast<DOM::Element&>(*sibling).local_name() == tag;
            },
            [&] { return MUST(DOM::create_element(*first_node->owner_document(), tag, Namespace::HTML)); });

        // 3. Abort these steps.
        return;
    }

    // 4. Wrap node list, with sibling criteria returning true for a simple indentation element and false otherwise, and
    //    new parent instructions returning the result of calling createElement("blockquote") on the ownerDocument of
    //    first node. Let new parent be the result.
    auto new_parent = wrap(
        node_list,
        [&](GC::Ref<DOM::Node> sibling) { return is_simple_indentation_element(sibling); },
        [&] { return MUST(DOM::create_element(*first_node->owner_document(), HTML::TagNames::blockquote, Namespace::HTML)); });

    // 5. Fix disallowed ancestors of new parent.
    if (new_parent)
        fix_disallowed_ancestors_of_node(*new_parent);
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
bool is_block_boundary_point(DOM::BoundaryPoint boundary_point)
{
    // A boundary point is a block boundary point if it is either a block start point or a block end point.
    return is_block_start_point(boundary_point) || is_block_end_point(boundary_point);
}

// https://w3c.github.io/editing/docs/execCommand/#block-end-point
bool is_block_end_point(DOM::BoundaryPoint boundary_point)
{
    // A boundary point (node, offset) is a block end point if either node's parent is null and
    // offset is node's length;
    if (!boundary_point.node->parent() && boundary_point.offset == boundary_point.node->length())
        return true;

    // or node has a child with index offset, and that child is a visible block node.
    auto offset_child = boundary_point.node->child_at_index(boundary_point.offset);
    return offset_child && is_visible_node(*offset_child) && is_block_node(*offset_child);
}

// https://w3c.github.io/editing/docs/execCommand/#block-node
bool is_block_node(GC::Ref<DOM::Node> node)
{
    // A block node is either an Element whose "display" property does not have resolved value
    // "inline" or "inline-block" or "inline-table" or "none", or a document, or a DocumentFragment.
    if (is<DOM::Document>(*node) || is<DOM::DocumentFragment>(*node))
        return true;

    if (!is<DOM::Element>(*node))
        return false;

    auto display = resolved_display(node);
    if (!display.has_value())
        return true;
    return !(display->is_inline_outside() && (display->is_flow_inside() || display->is_flow_root_inside() || display->is_table_inside()))
        && !display->is_none();
}

// https://w3c.github.io/editing/docs/execCommand/#block-start-point
bool is_block_start_point(DOM::BoundaryPoint boundary_point)
{
    // A boundary point (node, offset) is a block start point if either node's parent is null and
    // offset is zero;
    if (!boundary_point.node->parent() && boundary_point.offset == 0)
        return true;

    // or node has a child with index offset − 1, and that child is either a visible block node or a
    // visible br.
    auto offset_minus_one_child = boundary_point.node->child_at_index(boundary_point.offset - 1);
    return offset_minus_one_child && is_visible_node(*offset_minus_one_child)
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
    GC::Ptr<DOM::Node> some_ancestor = node->parent();
    while (some_ancestor) {
        auto display = resolved_display(*some_ancestor);
        if (display.has_value() && display->is_none())
            return true;
        some_ancestor = some_ancestor->parent();
    }

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

// https://w3c.github.io/editing/docs/execCommand/#effectively-contained
bool is_effectively_contained_in_range(GC::Ref<DOM::Node> node, GC::Ref<DOM::Range> range)
{
    // A node node is effectively contained in a range range if range is not collapsed, and at least one of the
    // following holds:
    if (range->collapsed())
        return false;

    // * node is contained in range.
    if (range->contains_node(node))
        return true;

    // * node is range's start node, it is a Text node, and its length is different from range's start offset.
    if (node == range->start_container() && is<DOM::Text>(*node) && node->length() != range->start_offset())
        return true;

    // * node is range's end node, it is a Text node, and range's end offset is not 0.
    if (node == range->end_container() && is<DOM::Text>(*node) && range->end_offset() != 0)
        return true;

    // * node has at least one child; and all its children are effectively contained in range;
    if (!node->has_children())
        return false;
    for (auto* child = node->first_child(); child; child = child->next_sibling()) {
        if (!is_effectively_contained_in_range(*child, range))
            return false;
    }

    // and either range's start node is not a descendant of node or is not a Text node or range's start offset is zero;
    auto start_node = range->start_container();
    if (start_node->is_descendant_of(node) && is<DOM::Text>(*start_node) && range->start_offset() != 0)
        return false;

    // and either range's end node is not a descendant of node or is not a Text node or range's end offset is its end
    // node's length.
    auto end_node = range->end_container();
    if (end_node->is_descendant_of(node) && is<DOM::Text>(*end_node) && range->end_offset() != end_node->length())
        return false;

    return true;
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

// https://w3c.github.io/editing/docs/execCommand/#formattable-block-name
bool is_formattable_block_name(FlyString const& local_name)
{
    // A formattable block name is "address", "dd", "div", "dt", "h1", "h2", "h3", "h4", "h5", "h6", "p", or "pre".
    return local_name.is_one_of(HTML::TagNames::address, HTML::TagNames::dd, HTML::TagNames::div, HTML::TagNames::dt,
        HTML::TagNames::h1, HTML::TagNames::h2, HTML::TagNames::h3, HTML::TagNames::h4, HTML::TagNames::h5,
        HTML::TagNames::h6, HTML::TagNames::p, HTML::TagNames::pre);
}

// https://w3c.github.io/editing/docs/execCommand/#formattable-node
bool is_formattable_node(GC::Ref<DOM::Node> node)
{
    // A formattable node is an editable visible node that is either a Text node, an img, or a br.
    return node->is_editable() && is_visible_node(node)
        && (is<DOM::Text>(*node) || is<HTML::HTMLImageElement>(*node) || is<HTML::HTMLBRElement>(*node));
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

// https://w3c.github.io/editing/docs/execCommand/#indentation-element
bool is_indentation_element(GC::Ref<DOM::Node> node)
{
    // An indentation element is either a blockquote,
    if (!is<DOM::Element>(*node))
        return false;
    auto& element = static_cast<DOM::Element&>(*node);
    if (element.local_name() == HTML::TagNames::blockquote)
        return true;

    // or a div that has a style attribute that sets "margin" or some subproperty of it.
    auto inline_style = element.inline_style();
    return is<HTML::HTMLDivElement>(element)
        && element.has_attribute(HTML::AttributeNames::style)
        && inline_style
        && (!inline_style->margin().is_empty() || !inline_style->margin_top().is_empty()
            || !inline_style->margin_right().is_empty() || !inline_style->margin_bottom().is_empty()
            || !inline_style->margin_left().is_empty());
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

// https://w3c.github.io/editing/docs/execCommand/#modifiable-element
bool is_modifiable_element(GC::Ref<DOM::Node> node)
{
    // NOTE: All conditions below expect an HTML element.
    if (!is<HTML::HTMLElement>(*node))
        return false;
    auto const& html_element = static_cast<HTML::HTMLElement const&>(*node);

    // A modifiable element is a b, em, i, s, span, strike, strong, sub, sup, or u element with no attributes except
    // possibly style;
    auto has_no_attributes_except = [&](auto exclusions) {
        auto attribute_count = 0;
        html_element.for_each_attribute([&](DOM::Attr const& attribute) {
            if (!exclusions.contains_slow(attribute.local_name()))
                ++attribute_count;
        });
        return attribute_count == 0;
    };
    if (html_element.local_name().is_one_of(HTML::TagNames::b, HTML::TagNames::em, HTML::TagNames::i,
            HTML::TagNames::s, HTML::TagNames::span, HTML::TagNames::strike, HTML::TagNames::strong,
            HTML::TagNames::sub, HTML::TagNames::sup, HTML::TagNames::u))
        return has_no_attributes_except(Array { HTML::AttributeNames::style });

    // or a font element with no attributes except possibly style, color, face, and/or size;
    if (is<HTML::HTMLFontElement>(html_element)) {
        return has_no_attributes_except(Array { HTML::AttributeNames::style, HTML::AttributeNames::color,
            HTML::AttributeNames::face, HTML::AttributeNames::size });
    }

    // or an a element with no attributes except possibly style and/or href.
    return is<HTML::HTMLAnchorElement>(html_element)
        && has_no_attributes_except(Array { HTML::AttributeNames::style, HTML::AttributeNames::href });
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
    // A non-list single-line container is an HTML element with local name "address", "div", "h1", "h2", "h3", "h4",
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

// https://w3c.github.io/editing/docs/execCommand/#removeformat-candidate
bool is_remove_format_candidate(GC::Ref<DOM::Node> node)
{
    // A removeFormat candidate is an editable HTML element with local name "abbr", "acronym", "b", "bdi", "bdo", "big",
    // "blink", "cite", "code", "dfn", "em", "font", "i", "ins", "kbd", "mark", "nobr", "q", "s", "samp", "small",
    // "span", "strike", "strong", "sub", "sup", "tt", "u", or "var".
    if (!node->is_editable())
        return false;
    if (!is<HTML::HTMLElement>(*node))
        return false;
    return static_cast<HTML::HTMLElement&>(*node).local_name().is_one_of(
        HTML::TagNames::abbr,
        HTML::TagNames::acronym,
        HTML::TagNames::b,
        HTML::TagNames::bdi,
        HTML::TagNames::bdo,
        HTML::TagNames::big,
        HTML::TagNames::blink,
        HTML::TagNames::cite,
        HTML::TagNames::code,
        HTML::TagNames::dfn,
        HTML::TagNames::em,
        HTML::TagNames::font,
        HTML::TagNames::i,
        HTML::TagNames::ins,
        HTML::TagNames::kbd,
        HTML::TagNames::mark,
        HTML::TagNames::nobr,
        HTML::TagNames::q,
        HTML::TagNames::s,
        HTML::TagNames::samp,
        HTML::TagNames::small,
        HTML::TagNames::span,
        HTML::TagNames::strike,
        HTML::TagNames::strong,
        HTML::TagNames::sub,
        HTML::TagNames::sup,
        HTML::TagNames::tt,
        HTML::TagNames::u,
        HTML::TagNames::var);
}

// https://w3c.github.io/editing/docs/execCommand/#simple-indentation-element
bool is_simple_indentation_element(GC::Ref<DOM::Node> node)
{
    // A simple indentation element is an indentation element
    if (!is_indentation_element(node))
        return false;
    auto const& element = static_cast<DOM::Element&>(*node);
    auto inline_style = element.inline_style();

    // that has no attributes except possibly
    bool has_only_valid_attributes = true;
    element.for_each_attribute([&](DOM::Attr const& attribute) {
        // * a style attribute that sets no properties other than "margin", "border", "padding", or subproperties of
        //   those;
        if (attribute.local_name() == HTML::AttributeNames::style) {
            if (!inline_style)
                return;
            for (auto& style_property : inline_style->properties()) {
                switch (style_property.property_id) {
                case CSS::PropertyID::Border:
                case CSS::PropertyID::BorderBottom:
                case CSS::PropertyID::BorderLeft:
                case CSS::PropertyID::BorderRight:
                case CSS::PropertyID::BorderTop:
                case CSS::PropertyID::Margin:
                case CSS::PropertyID::MarginBottom:
                case CSS::PropertyID::MarginLeft:
                case CSS::PropertyID::MarginRight:
                case CSS::PropertyID::MarginTop:
                case CSS::PropertyID::Padding:
                case CSS::PropertyID::PaddingBottom:
                case CSS::PropertyID::PaddingLeft:
                case CSS::PropertyID::PaddingRight:
                case CSS::PropertyID::PaddingTop:
                    // Allowed
                    break;
                default:
                    has_only_valid_attributes = false;
                    return;
                }
            }
        }

        // * and/or a dir attribute.
        else if (attribute.local_name() != HTML::AttributeNames::dir) {
            has_only_valid_attributes = false;
        }
    });
    return has_only_valid_attributes;
}

// https://w3c.github.io/editing/docs/execCommand/#simple-modifiable-element
bool is_simple_modifiable_element(GC::Ref<DOM::Node> node)
{
    // A simple modifiable element is an HTML element for which at least one of the following holds:
    if (!is<HTML::HTMLElement>(*node))
        return false;
    auto const& html_element = static_cast<HTML::HTMLElement&>(*node);
    auto const inline_style = html_element.inline_style();

    // * It is an a, b, em, font, i, s, span, strike, strong, sub, sup, or u element with no attributes.
    // * It is an a, b, em, font, i, s, span, strike, strong, sub, sup, or u element with exactly one attribute, which
    //   is style, which sets no CSS properties (including invalid or unrecognized properties).
    auto attribute_count = html_element.attribute_list_size();
    if (html_element.local_name().is_one_of(HTML::TagNames::a, HTML::TagNames::b, HTML::TagNames::em,
            HTML::TagNames::font, HTML::TagNames::i, HTML::TagNames::s, HTML::TagNames::span, HTML::TagNames::strike,
            HTML::TagNames::strong, HTML::TagNames::sub, HTML::TagNames::sup, HTML::TagNames::u)) {
        if (attribute_count == 0)
            return true;

        if (attribute_count == 1 && html_element.has_attribute(HTML::AttributeNames::style)
            && (!inline_style || inline_style->length() == 0))
            return true;
    }

    // NOTE: All conditions below require exactly one attribute on the element
    if (attribute_count != 1)
        return false;

    // * It is an a element with exactly one attribute, which is href.
    if (is<HTML::HTMLAnchorElement>(html_element)
        && html_element.get_attribute(HTML::AttributeNames::href).has_value())
        return true;

    // * It is a font element with exactly one attribute, which is either color, face, or size.
    if (is<HTML::HTMLFontElement>(html_element)) {
        if (html_element.has_attribute(HTML::AttributeNames::color)
            || html_element.has_attribute(HTML::AttributeNames::face)
            || html_element.has_attribute(HTML::AttributeNames::size))
            return true;
    }

    // NOTE: All conditions below require exactly one attribute which is style, that sets one CSS property.
    if (!html_element.has_attribute(HTML::AttributeNames::style) || !inline_style || (inline_style->length() != 1))
        return false;

    // * It is a b or strong element with exactly one attribute, which is style, and the style attribute sets exactly
    //   one CSS property (including invalid or unrecognized properties), which is "font-weight".
    if (html_element.local_name().is_one_of(HTML::TagNames::b, HTML::TagNames::strong)
        && inline_style->property(CSS::PropertyID::FontWeight).has_value())
        return true;

    // * It is an i or em element with exactly one attribute, which is style, and the style attribute sets exactly one
    //   CSS property (including invalid or unrecognized properties), which is "font-style".
    if (html_element.local_name().is_one_of(HTML::TagNames::i, HTML::TagNames::em)
        && inline_style->property(CSS::PropertyID::FontStyle).has_value())
        return true;

    // * It is an a, font, or span element with exactly one attribute, which is style, and the style attribute sets
    //   exactly one CSS property (including invalid or unrecognized properties), and that property is not
    //   "text-decoration".
    if (html_element.local_name().is_one_of(HTML::TagNames::a, HTML::TagNames::font, HTML::TagNames::span)
        && !inline_style->property(CSS::PropertyID::TextDecoration).has_value())
        return true;

    // * It is an a, font, s, span, strike, or u element with exactly one attribute, which is style, and the style
    //   attribute sets exactly one CSS property (including invalid or unrecognized properties), which is
    //   "text-decoration", which is set to "line-through" or "underline" or "overline" or "none".
    if (html_element.local_name().is_one_of(HTML::TagNames::a, HTML::TagNames::font, HTML::TagNames::s,
            HTML::TagNames::span, HTML::TagNames::strike, HTML::TagNames::u)
        && inline_style->property(CSS::PropertyID::TextDecoration).has_value()) {
        auto text_decoration = inline_style->text_decoration();
        if (first_is_one_of(text_decoration,
                string_from_keyword(CSS::Keyword::LineThrough),
                string_from_keyword(CSS::Keyword::Underline),
                string_from_keyword(CSS::Keyword::Overline),
                string_from_keyword(CSS::Keyword::None)))
            return true;
    }

    return false;
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
    bool has_display_none = false;
    node->for_each_inclusive_ancestor([&has_display_none](GC::Ref<DOM::Node> ancestor) {
        auto display = resolved_display(ancestor);
        if (display.has_value() && display->is_none()) {
            has_display_none = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (has_display_none)
        return false;

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
    auto resolved_white_space = resolved_keyword(*parent, CSS::PropertyID::WhiteSpace);
    if (!resolved_white_space.has_value())
        return false;
    auto white_space = resolved_white_space.value();

    // or a Text node whose data consists only of one or more tabs (0x0009), line feeds (0x000A),
    // carriage returns (0x000D), and/or spaces (0x0020), and whose parent is an Element whose
    // resolved value for "white-space" is "normal" or "nowrap";
    auto is_tab_lf_cr_or_space = [](u32 codepoint) {
        return codepoint == '\t' || codepoint == '\n' || codepoint == '\r' || codepoint == ' ';
    };
    auto code_points = character_data.data().code_points();
    if (all_of(code_points, is_tab_lf_cr_or_space) && (white_space == CSS::Keyword::Normal || white_space == CSS::Keyword::Nowrap))
        return true;

    // or a Text node whose data consists only of one or more tabs (0x0009), carriage returns
    // (0x000D), and/or spaces (0x0020), and whose parent is an Element whose resolved value for
    // "white-space" is "pre-line".
    auto is_tab_cr_or_space = [](u32 codepoint) {
        return codepoint == '\t' || codepoint == '\r' || codepoint == ' ';
    };
    if (all_of(code_points, is_tab_cr_or_space) && white_space == CSS::Keyword::PreLine)
        return true;

    return false;
}

// https://w3c.github.io/editing/docs/execCommand/#justify-the-selection
void justify_the_selection(DOM::Document& document, JustifyAlignment alignment)
{
    // 1. Block-extend the active range, and let new range be the result.
    auto new_range = block_extend_a_range(*active_range(document));

    // 2. Let element list be a list of all editable Elements contained in new range that either has an attribute in the
    //    HTML namespace whose local name is "align", or has a style attribute that sets "text-align", or is a center.
    Vector<GC::Ref<DOM::Element>> element_list;
    new_range->for_each_contained([&element_list](GC::Ref<DOM::Node> node) {
        if (!node->is_editable() || !is<DOM::Element>(*node))
            return IterationDecision::Continue;

        auto& element = static_cast<DOM::Element&>(*node);
        if (element.has_attribute_ns(Namespace::HTML, HTML::AttributeNames::align)
            || property_in_style_attribute(element, CSS::PropertyID::TextAlign).has_value()
            || element.local_name() == HTML::TagNames::center)
            element_list.append(element);

        return IterationDecision::Continue;
    });

    // 3. For each element in element list:
    for (auto element : element_list) {
        // 1. If element has an attribute in the HTML namespace whose local name is "align", remove that attribute.
        if (element->has_attribute_ns(Namespace::HTML, HTML::AttributeNames::align))
            element->remove_attribute_ns(Namespace::HTML, HTML::AttributeNames::align);

        // 2. Unset the CSS property "text-align" on element, if it's set by a style attribute.
        auto inline_style = element->style_for_bindings();
        MUST(inline_style->remove_property(CSS::PropertyID::TextAlign));

        // 3. If element is a div or span or center with no attributes, remove it, preserving its descendants.
        if (element->local_name().is_one_of(HTML::TagNames::div, HTML::TagNames::span, HTML::TagNames::center)
            && !element->has_attributes())
            remove_node_preserving_its_descendants(element);

        // 4. If element is a center with one or more attributes, set the tag name of element to "div".
        if (element->local_name() == HTML::TagNames::center && element->has_attributes())
            set_the_tag_name(element, HTML::TagNames::div);
    }

    // 4. Block-extend the active range, and let new range be the result.
    new_range = block_extend_a_range(*active_range(document));

    // 5. Let node list be a list of nodes, initially empty.
    Vector<GC::Ref<DOM::Node>> node_list;

    // 6. For each node node contained in new range, append node to node list if the last member of node list (if any)
    //    is not an ancestor of node; node is editable; node is an allowed child of "div"; and node's alignment value is
    //    not alignment.
    new_range->for_each_contained([&](GC::Ref<DOM::Node> node) {
        if ((node_list.is_empty() || !node_list.last()->is_ancestor_of(node))
            && node->is_editable()
            && is_allowed_child_of_node(node, HTML::TagNames::div)
            && alignment_value_of_node(node) != alignment)
            node_list.append(node);
        return IterationDecision::Continue;
    });

    // 7. While node list is not empty:
    while (!node_list.is_empty()) {
        // 1. Let sublist be a list of nodes, initially empty.
        Vector<GC::Ref<DOM::Node>> sublist;

        // 2. Remove the first member of node list and append it to sublist.
        sublist.append(node_list.take_first());

        // 3. While node list is not empty, and the first member of node list is the nextSibling of the last member of
        //    sublist, remove the first member of node list and append it to sublist.
        while (!node_list.is_empty() && node_list.first().ptr() == sublist.last()->next_sibling())
            sublist.append(node_list.take_first());

        // 4. Wrap sublist. Sibling criteria returns true for any div that has one or both of the following two
        //    attributes and no other attributes, and false otherwise:
        //    * An align attribute whose value is an ASCII case-insensitive match for alignment.
        //    * A style attribute which sets exactly one CSS property (including unrecognized or invalid attributes),
        //      which is "text-align", which is set to alignment.
        //
        //    New parent instructions are to call createElement("div") on the context object, then set its CSS property
        //    "text-align" to alignment and return the result.
        auto alignment_keyword = string_from_keyword([&alignment] {
            switch (alignment) {
            case JustifyAlignment::Center:
                return CSS::Keyword::Center;
            case JustifyAlignment::Justify:
                return CSS::Keyword::Justify;
            case JustifyAlignment::Left:
                return CSS::Keyword::Left;
            case JustifyAlignment::Right:
                return CSS::Keyword::Right;
            }
            VERIFY_NOT_REACHED();
        }());

        wrap(
            sublist,
            [&](GC::Ref<DOM::Node> sibling) {
                if (!is<HTML::HTMLDivElement>(*sibling))
                    return false;
                GC::Ref<DOM::Element> element = static_cast<DOM::Element&>(*sibling);
                u8 number_of_matching_attributes = 0;
                if (element->get_attribute_value(HTML::AttributeNames::align).equals_ignoring_ascii_case(alignment_keyword))
                    ++number_of_matching_attributes;
                if (element->has_attribute(HTML::AttributeNames::style) && element->inline_style()
                    && element->inline_style()->length() == 1) {
                    auto text_align = element->inline_style()->property(CSS::PropertyID::TextAlign);
                    if (text_align.has_value()) {
                        auto align_value = text_align.value().value->to_string(CSS::CSSStyleValue::SerializationMode::Normal);
                        if (align_value.equals_ignoring_ascii_case(alignment_keyword))
                            ++number_of_matching_attributes;
                    }
                }
                return element->attribute_list_size() == number_of_matching_attributes;
            },
            [&] {
                auto div = MUST(DOM::create_element(document, HTML::TagNames::div, Namespace::HTML));
                auto inline_style = div->style_for_bindings();
                MUST(inline_style->set_property(CSS::PropertyID::TextAlign, alignment_keyword));
                return div;
            });
    }
}

// https://w3c.github.io/editing/docs/execCommand/#last-equivalent-point
DOM::BoundaryPoint last_equivalent_point(DOM::BoundaryPoint boundary_point)
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

// https://w3c.github.io/editing/docs/execCommand/#legacy-font-size-for
String legacy_font_size(int pixel_size)
{
    // 1. Let returned size be 1.
    auto returned_size = 1;

    // 2. While returned size is less than 7:
    while (returned_size < 7) {
        // 1. Let lower bound be the resolved value of "font-size" in pixels of a font element whose size attribute is
        //    set to returned size.
        auto lower_bound = font_size_to_pixel_size(MUST(String::formatted("{}", returned_size))).to_float();

        // 2. Let upper bound be the resolved value of "font-size" in pixels of a font element whose size attribute is
        //    set to one plus returned size.
        auto upper_bound = font_size_to_pixel_size(MUST(String::formatted("{}", returned_size + 1))).to_float();

        // 3. Let average be the average of upper bound and lower bound.
        auto average = (lower_bound + upper_bound) / 2;

        // 4. If pixel size is less than average, return the one-code unit string consisting of the digit returned size.
        if (pixel_size < average)
            return MUST(String::formatted("{}", returned_size));

        // 5. Add one to returned size.
        ++returned_size;
    }

    // 3. Return "7".
    return "7"_string;
}

// https://w3c.github.io/editing/docs/execCommand/#preserving-ranges
void move_node_preserving_ranges(GC::Ref<DOM::Node> node, GC::Ref<DOM::Node> new_parent, u32 new_index)
{
    // To move a node to a new location, preserving ranges, remove the node from its original parent
    // (if any), then insert it in the new location. In doing so, follow these rules instead of
    // those defined by the insert and remove algorithms:

    // AD-HOC: We implement this spec by taking note of the current active range (if any), performing the remove and
    //         insertion of node, and then restoring the range after performing any necessary adjustments.
    Optional<DOM::BoundaryPoint> start;
    Optional<DOM::BoundaryPoint> end;

    auto range = active_range(node->document());
    if (range) {
        start = range->start();
        end = range->end();
    }

    // 1. Let node be the moved node, old parent and old index be the old parent (which may be null)
    //    and index, and new parent and new index be the new parent and index.
    auto* old_parent = node->parent();
    auto old_index = node->index();
    if (old_parent)
        node->remove();

    auto* new_next_sibling = new_parent->child_at_index(new_index);
    new_parent->insert_before(node, new_next_sibling);

    // AD-HOC: Return early if there was no active range
    if (!range)
        return;

    // 2. If a boundary point's node is the same as or a descendant of node, leave it unchanged, so
    //    it moves to the new location.
    // NOTE: This step exists for completeness.

    // 3. If a boundary point's node is new parent and its offset is greater than new index, add one
    //    to its offset.
    if (start->node == new_parent && start->offset > new_index)
        start->offset++;
    if (end->node == new_parent && end->offset > new_index)
        end->offset++;

    // 4. If a boundary point's node is old parent and its offset is old index or old index + 1, set
    //    its node to new parent and add new index − old index to its offset.
    if (start->node == old_parent && (start->offset == old_index || start->offset == old_index + 1)) {
        start->node = new_parent;
        start->offset += new_index - old_index;
    }
    if (end->node == old_parent && (end->offset == old_index || end->offset == old_index + 1)) {
        end->node = new_parent;
        end->offset += new_index - old_index;
    }

    // 5. If a boundary point's node is old parent and its offset is greater than old index + 1,
    //    subtract one from its offset.
    if (start->node == old_parent && start->offset > old_index + 1)
        start->offset--;
    if (end->node == old_parent && end->offset > old_index + 1)
        end->offset--;

    // AD-HOC: Set the new active range
    MUST(range->set_start(start->node, start->offset));
    MUST(range->set_end(end->node, end->offset));
}

// https://w3c.github.io/editing/docs/execCommand/#next-equivalent-point
Optional<DOM::BoundaryPoint> next_equivalent_point(DOM::BoundaryPoint boundary_point)
{
    // 1. If node's length is zero, return null.
    auto node = boundary_point.node;
    auto node_length = node->length();
    if (node_length == 0)
        return {};

    // 3. If offset is node's length, and node's parent is not null, and node is an inline node, return (node's parent,
    //    1 + node's index).
    if (boundary_point.offset == node_length && node->parent() && is_inline_node(*node))
        return DOM::BoundaryPoint { *node->parent(), static_cast<WebIDL::UnsignedLong>(node->index() + 1) };

    // 5. If node has a child with index offset, and that child's length is not zero, and that child is an inline node,
    //    return (that child, 0).
    auto child_at_offset = node->child_at_index(boundary_point.offset);
    if (child_at_offset && child_at_offset->length() != 0 && is_inline_node(*child_at_offset))
        return DOM::BoundaryPoint { *child_at_offset, 0 };

    // 7. Return null.
    return {};
}

// https://w3c.github.io/editing/docs/execCommand/#normalize-sublists
void normalize_sublists_in_node(GC::Ref<DOM::Node> item)
{
    // 1. If item is not an li or it is not editable or its parent is not editable, abort these
    //    steps.
    if (!is<HTML::HTMLLIElement>(*item) || !item->is_editable() || !item->parent()->is_editable())
        return;

    // 2. Let new item be null.
    GC::Ptr<DOM::Node> new_item;

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

// https://w3c.github.io/editing/docs/execCommand/#outdent
void outdent(GC::Ref<DOM::Node> node)
{
    // 1. If node is not editable, abort these steps.
    if (!node->is_editable())
        return;

    // 2. If node is a simple indentation element, remove node, preserving its descendants. Then abort these steps.
    if (is_simple_indentation_element(node)) {
        remove_node_preserving_its_descendants(node);
        return;
    }

    // 3. If node is an indentation element:
    if (is_indentation_element(node)) {
        // 1. Unset the dir attribute of node, if any.
        auto& element = static_cast<DOM::Element&>(*node);
        element.remove_attribute(HTML::AttributeNames::dir);

        // 2. Unset the margin, padding, and border CSS properties of node.
        if (auto inline_style = element.inline_style()) {
            MUST(inline_style->remove_property(CSS::string_from_property_id(CSS::PropertyID::Border)));
            MUST(inline_style->remove_property(CSS::string_from_property_id(CSS::PropertyID::Margin)));
            MUST(inline_style->remove_property(CSS::string_from_property_id(CSS::PropertyID::Padding)));
        }

        // 3. Set the tag name of node to "div".
        set_the_tag_name(element, HTML::TagNames::div);

        // 4. Abort these steps.
        return;
    }

    // 4. Let current ancestor be node's parent.
    GC::Ptr<DOM::Node> current_ancestor = node->parent();

    // 5. Let ancestor list be a list of nodes, initially empty.
    Vector<GC::Ref<DOM::Node>> ancestor_list;

    // 6. While current ancestor is an editable Element that is neither a simple indentation element nor an ol nor a ul,
    //    append current ancestor to ancestor list and then set current ancestor to its parent.
    while (is<DOM::Element>(current_ancestor.ptr())
        && current_ancestor->is_editable()
        && !is_simple_indentation_element(*current_ancestor)
        && !is<HTML::HTMLOListElement>(*current_ancestor)
        && !is<HTML::HTMLUListElement>(*current_ancestor)) {
        ancestor_list.append(*current_ancestor);
        current_ancestor = current_ancestor->parent();
    }

    // 7. If current ancestor is not an editable simple indentation element:
    if (!current_ancestor || !current_ancestor->is_editable() || !is_simple_indentation_element(*current_ancestor)) {
        // 1. Let current ancestor be node's parent.
        current_ancestor = node->parent();

        // 2. Let ancestor list be the empty list.
        ancestor_list.clear_with_capacity();

        // 3. While current ancestor is an editable Element that is neither an indentation element nor an ol nor a ul,
        //    append current ancestor to ancestor list and then set current ancestor to its parent.
        while (is<DOM::Element>(current_ancestor.ptr())
            && current_ancestor->is_editable()
            && !is_indentation_element(*current_ancestor)
            && !is<HTML::HTMLOListElement>(*current_ancestor)
            && !is<HTML::HTMLUListElement>(*current_ancestor)) {
            ancestor_list.append(*current_ancestor);
            current_ancestor = current_ancestor->parent();
        }
    }

    // 8. If node is an ol or ul and current ancestor is not an editable indentation element:
    if ((is<HTML::HTMLOListElement>(*node) || is<HTML::HTMLUListElement>(*node))
        && !(current_ancestor->is_editable() && is_indentation_element(*current_ancestor))) {
        // 1. Unset the reversed, start, and type attributes of node, if any are set.
        auto& node_element = static_cast<DOM::Element&>(*node);
        node_element.remove_attribute(HTML::AttributeNames::reversed);
        node_element.remove_attribute(HTML::AttributeNames::start);
        node_element.remove_attribute(HTML::AttributeNames::type);

        // 2. Let children be the children of node.
        Vector<GC::Ref<DOM::Node>> children;
        for (auto* child = node->first_child(); child; child = child->next_sibling())
            children.append(*child);

        // 3. If node has attributes, and its parent is not an ol or ul, set the tag name of node to "div".
        if (node_element.has_attributes() && !is<HTML::HTMLOListElement>(node->parent())
            && !is<HTML::HTMLUListElement>(node->parent())) {
            set_the_tag_name(node_element, HTML::TagNames::div);
        }

        // 4. Otherwise:
        else {
            // 1. Record the values of node's children, and let values be the result.
            auto values = record_the_values_of_nodes(children);

            // 2. Remove node, preserving its descendants.
            remove_node_preserving_its_descendants(node);

            // 3. Restore the values from values.
            restore_the_values_of_nodes(values);
        }

        // 5. Fix disallowed ancestors of each member of children.
        for (auto child : children)
            fix_disallowed_ancestors_of_node(*child);

        // 6. Abort these steps.
        return;
    }

    // 9. If current ancestor is not an editable indentation element, abort these steps.
    if (!current_ancestor || !current_ancestor->is_editable() || !is_indentation_element(*current_ancestor))
        return;

    // 10. Append current ancestor to ancestor list.
    ancestor_list.append(*current_ancestor);

    // 11. Let original ancestor be current ancestor.
    auto original_ancestor = current_ancestor;

    // 12. While ancestor list is not empty:
    while (!ancestor_list.is_empty()) {
        // 1. Let current ancestor be the last member of ancestor list.
        // 2. Remove the last member from ancestor list.
        current_ancestor = ancestor_list.take_last();

        // 3. Let target be the child of current ancestor that is equal to either node or the last member of ancestor
        //    list.
        GC::Ptr<DOM::Node> target;
        for (auto* child = current_ancestor->first_child(); child; child = child->next_sibling()) {
            if (child == node.ptr() || (!ancestor_list.is_empty() && child == ancestor_list.last().ptr())) {
                target = child;
                break;
            }
        }
        VERIFY(target);

        // 4. If target is an inline node that is not a br, and its nextSibling is a br, remove target's nextSibling
        //    from its parent.
        if (is_inline_node(*target) && !is<HTML::HTMLBRElement>(*target) && is<HTML::HTMLBRElement>(target->next_sibling()))
            target->next_sibling()->remove();

        // 5. Let preceding siblings be the precedings siblings of target, and let following siblings be the followings
        //    siblings of target.
        Vector<GC::Ref<DOM::Node>> preceding_siblings;
        for (auto* sibling = target->previous_sibling(); sibling; sibling = sibling->previous_sibling())
            preceding_siblings.append(*sibling);
        Vector<GC::Ref<DOM::Node>> following_siblings;
        for (auto* sibling = target->next_sibling(); sibling; sibling = sibling->next_sibling())
            following_siblings.append(*sibling);

        // 6. Indent preceding siblings.
        indent(preceding_siblings);

        // 7. Indent following siblings.
        indent(following_siblings);
    }

    // 13. Outdent original ancestor.
    outdent(*original_ancestor);
}

// https://w3c.github.io/editing/docs/execCommand/#precedes-a-line-break
bool precedes_a_line_break(GC::Ref<DOM::Node> node)
{
    // 1. Let offset be node's length.
    WebIDL::UnsignedLong offset = node->length();

    // 2. While (node, offset) is not a block boundary point:
    while (!is_block_boundary_point({ node, offset })) {
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
Optional<DOM::BoundaryPoint> previous_equivalent_point(DOM::BoundaryPoint boundary_point)
{
    // 1. If node's length is zero, return null.
    auto node = boundary_point.node;
    auto node_length = node->length();
    if (node_length == 0)
        return {};

    // 2. If offset is 0, and node's parent is not null, and node is an inline node, return (node's parent, node's
    //    index).
    if (boundary_point.offset == 0 && node->parent() && is_inline_node(*node))
        return DOM::BoundaryPoint { *node->parent(), static_cast<WebIDL::UnsignedLong>(node->index()) };

    // 3. If node has a child with index offset − 1, and that child's length is not zero, and that child is an inline
    //    node, return (that child, that child's length).
    auto child_at_offset = node->child_at_index(boundary_point.offset - 1);
    if (child_at_offset && child_at_offset->length() != 0 && is_inline_node(*child_at_offset))
        return DOM::BoundaryPoint { *child_at_offset, static_cast<WebIDL::UnsignedLong>(child_at_offset->length()) };

    // 4. Return null.
    return {};
}

// https://w3c.github.io/editing/docs/execCommand/#push-down-values
void push_down_values(FlyString const& command, GC::Ref<DOM::Node> node, Optional<String> new_value)
{
    // 1. Let command be the current command.

    // 2. If node's parent is not an Element, abort this algorithm.
    if (!is<DOM::Element>(node->parent()))
        return;

    // 3. If the effective command value of command is loosely equivalent to new value on node, abort this algorithm.
    if (values_are_loosely_equivalent(command, effective_command_value(node, command), new_value))
        return;

    // 4. Let current ancestor be node's parent.
    auto current_ancestor = GC::Ptr { node->parent() };

    // 5. Let ancestor list be a list of nodes, initially empty.
    Vector<GC::Ref<DOM::Node>> ancestor_list;

    // 6. While current ancestor is an editable Element and the effective command value of command is not loosely
    //    equivalent to new value on it, append current ancestor to ancestor list, then set current ancestor to its
    //    parent.
    while (is<DOM::Element>(current_ancestor.ptr()) && current_ancestor->is_editable()
        && !values_are_loosely_equivalent(command, effective_command_value(current_ancestor, command), new_value)) {
        ancestor_list.append(*current_ancestor);
        current_ancestor = current_ancestor->parent();
    }

    // 7. If ancestor list is empty, abort this algorithm.
    if (ancestor_list.is_empty())
        return;

    // 8. Let propagated value be the specified command value of command on the last member of ancestor list.
    auto propagated_value = specified_command_value(static_cast<DOM::Element&>(*ancestor_list.last()), command);

    // 9. If propagated value is null and is not equal to new value, abort this algorithm.
    if (!propagated_value.has_value() && new_value.has_value())
        return;

    // 10. If the effective command value of command is not loosely equivalent to new value on the parent of the last
    //     member of ancestor list, and new value is not null, abort this algorithm.
    if (new_value.has_value() && ancestor_list.last()->parent()
        && !values_are_loosely_equivalent(command, effective_command_value(ancestor_list.last()->parent(), command), new_value))
        return;

    // 11. While ancestor list is not empty:
    while (!ancestor_list.is_empty()) {
        // 1. Let current ancestor be the last member of ancestor list.
        // 2. Remove the last member from ancestor list.
        current_ancestor = ancestor_list.take_last();

        // 3. If the specified command value of current ancestor for command is not null, set propagated value to that
        //    value.
        // NOTE: Step 6 above guarantees that current_ancestor is an Element.
        auto command_value = specified_command_value(static_cast<DOM::Element&>(*current_ancestor), command);
        if (command_value.has_value())
            propagated_value = command_value.value();

        // 4. Let children be the children of current ancestor.
        auto children = current_ancestor->children_as_vector();

        // 5. If the specified command value of current ancestor for command is not null, clear the value of current
        //    ancestor.
        if (command_value.has_value())
            clear_the_value(command, static_cast<DOM::Element&>(*current_ancestor));

        // 6. For every child in children:
        for (auto const& child : children) {
            // 1. If child is node, continue with the next child.
            if (child.ptr() == node.ptr())
                continue;

            // 2. If child is an Element whose specified command value for command is neither null nor equivalent to
            //    propagated value, continue with the next child.
            if (is<DOM::Element>(*child)) {
                auto child_command_value = specified_command_value(static_cast<DOM::Element&>(*child), command);
                if (child_command_value.has_value() && child_command_value != propagated_value)
                    continue;
            }

            // 3. If child is the last member of ancestor list, continue with the next child.
            if (!ancestor_list.is_empty() && child.ptr() == ancestor_list.last().ptr())
                continue;

            // 4. Force the value of child, with command as in this algorithm and new value equal to propagated value.
            force_the_value(*child, command, propagated_value);
        }
    }
}

// https://w3c.github.io/editing/docs/execCommand/#record-current-overrides
Vector<RecordedOverride> record_current_overrides(DOM::Document const& document)
{
    // 1. Let overrides be a list of (string, string or boolean) ordered pairs, initially empty.
    Vector<RecordedOverride> overrides;

    // 2. If there is a value override for "createLink", add ("createLink", value override for "createLink") to
    //    overrides.
    if (auto override = document.command_value_override(CommandNames::createLink); override.has_value())
        overrides.empend(CommandNames::createLink, override.release_value());

    // 3. For each command in the list "bold", "italic", "strikethrough", "subscript", "superscript", "underline", in
    //    order: if there is a state override for command, add (command, command's state override) to overrides.
    for (auto const& command : { CommandNames::bold, CommandNames::italic, CommandNames::strikethrough,
             CommandNames::subscript, CommandNames::superscript, CommandNames::underline }) {
        if (auto override = document.command_state_override(command); override.has_value())
            overrides.empend(command, override.release_value());
    }

    // 4. For each command in the list "fontName", "fontSize", "foreColor", "hiliteColor", in order: if there is a value
    //    override for command, add (command, command's value override) to overrides.
    for (auto const& command : { CommandNames::fontName, CommandNames::fontSize, CommandNames::foreColor,
             CommandNames::hiliteColor }) {
        if (auto override = document.command_value_override(command); override.has_value())
            overrides.empend(command, override.release_value());
    }

    // 5. Return overrides.
    return overrides;
}

// https://w3c.github.io/editing/docs/execCommand/#record-current-states-and-values
Vector<RecordedOverride> record_current_states_and_values(DOM::Document const& document)
{
    // 1. Let overrides be a list of (string, string or boolean) ordered pairs, initially empty.
    Vector<RecordedOverride> overrides;

    // 2. Let node be the first formattable node effectively contained in the active range, or null if there is none.
    auto node = first_formattable_node_effectively_contained(active_range(document));

    // 3. If node is null, return overrides.
    if (!node)
        return overrides;

    // 4. Add ("createLink", node's effective command value for "createLink") to overrides.
    auto effective_value = effective_command_value(node, CommandNames::createLink);
    if (effective_value.has_value())
        overrides.empend(CommandNames::createLink, effective_value.release_value());

    // 5. For each command in the list "bold", "italic", "strikethrough", "subscript", "superscript", "underline", in
    //    order: if node's effective command value for command is one of its inline command activated values, add
    //    (command, true) to overrides, and otherwise add (command, false) to overrides.
    for (auto const& command : { CommandNames::bold, CommandNames::italic, CommandNames::strikethrough,
             CommandNames::subscript, CommandNames::superscript, CommandNames::underline }) {
        auto command_definition = find_command_definition(command);
        // FIXME: change this to VERIFY(command_definition.has_value()) once all command definitions are in place.
        if (!command_definition.has_value())
            continue;

        effective_value = effective_command_value(node, command);
        auto& inline_activated_values = command_definition.value().inline_activated_values;
        overrides.empend(command, effective_value.has_value() && inline_activated_values.contains_slow(*effective_value));
    }

    // 6. For each command in the list "fontName", "foreColor", "hiliteColor", in order: add (command, command's value)
    //    to overrides.
    for (auto const& command : { CommandNames::fontName, CommandNames::foreColor, CommandNames::hiliteColor })
        overrides.empend(command, MUST(node->document().query_command_value(command)));

    // 7. Add ("fontSize", node's effective command value for "fontSize") to overrides.
    effective_value = effective_command_value(node, CommandNames::fontSize);
    if (effective_value.has_value())
        overrides.empend(CommandNames::fontSize, effective_value.release_value());

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
    // To remove a node node while preserving its descendants, split the parent of node's children if it has any.
    if (node->has_children()) {
        Vector<GC::Ref<DOM::Node>> children;
        children.ensure_capacity(node->child_count());
        for (auto* child = node->first_child(); child; child = child->next_sibling())
            children.append(*child);
        split_the_parent_of_nodes(children);
        return;
    }

    // If it has no children, instead remove it from its parent.
    node->remove();
}

// https://w3c.github.io/editing/docs/execCommand/#reorder-modifiable-descendants
void reorder_modifiable_descendants(GC::Ref<DOM::Node> node, FlyString const& command, Optional<String> new_value)
{
    // 1. Let candidate equal node.
    GC::Ptr<DOM::Node> candidate = node;

    // 2. While candidate is a modifiable element, and candidate has exactly one child, and that child is also a
    //    modifiable element, and candidate is not a simple modifiable element or candidate's specified command value
    //    for command is not equivalent to new value, set candidate to its child.
    while (is_modifiable_element(*candidate) && candidate->child_count() == 1
        && is_modifiable_element(*candidate->first_child())
        && (!is_simple_modifiable_element(*candidate)
            || specified_command_value(static_cast<DOM::Element&>(*candidate), command) != new_value)) {
        candidate = candidate->first_child();
    }

    // 3. If candidate is node, or is not a simple modifiable element, or its specified command value is not equivalent
    //    to new value, or its effective command value is not loosely equivalent to new value, abort these steps.
    if (candidate == node
        || !is_simple_modifiable_element(*candidate)
        || specified_command_value(static_cast<DOM::Element&>(*candidate), command) != new_value
        || !values_are_loosely_equivalent(CommandNames::createLink, effective_command_value(candidate, command), new_value))
        return;

    // 4. While candidate has children, insert the first child of candidate into candidate's parent immediately before
    //    candidate, preserving ranges.
    while (candidate->has_children())
        move_node_preserving_ranges(*candidate->first_child(), *candidate->parent(), candidate->index());

    // 5. Insert candidate into node's parent immediately after node.
    if (node->next_sibling())
        node->parent()->insert_before(*candidate, node->next_sibling());
    else
        MUST(node->parent()->append_child(*candidate));

    // 6. Append the node as the last child of candidate, preserving ranges.
    move_node_preserving_ranges(node, *candidate, candidate->child_count());
}

// https://w3c.github.io/editing/docs/execCommand/#restore-states-and-values
void restore_states_and_values(DOM::Document& document, Vector<RecordedOverride> const& overrides)
{
    // 1. Let node be the first formattable node effectively contained in the active range, or null if there is none.
    auto node = first_formattable_node_effectively_contained(active_range(document));

    // 2. If node is not null,
    if (node) {
        // then for each (command, override) pair in overrides, in order:
        for (auto override : overrides) {
            // 1. If override is a boolean, and queryCommandState(command) returns something different from override,
            //    take the action for command, with value equal to the empty string.
            if (override.value.has<bool>() && MUST(document.query_command_state(override.command)) != override.value.get<bool>()) {
                take_the_action_for_command(document, override.command, {});
            }

            // 2. Otherwise, if override is a string, and command is neither "createLink" nor "fontSize", and
            //    queryCommandValue(command) returns something not equivalent to override, take the action for command,
            //    with value equal to override.
            else if (override.value.has<String>() && !override.command.is_one_of(CommandNames::createLink, CommandNames::fontSize)
                && MUST(document.query_command_value(override.command)) != override.value.get<String>()) {
                take_the_action_for_command(document, override.command, override.value.get<String>());
            }

            // 3. Otherwise, if override is a string; and command is "createLink"; and either there is a value override
            //    for "createLink" that is not equal to override, or there is no value override for "createLink" and
            //    node's effective command value for "createLink" is not equal to override: take the action for
            //    "createLink", with value equal to override.
            else if (auto value_override = document.command_value_override(CommandNames::createLink);
                override.value.has<String>() && override.command == CommandNames::createLink
                && ((value_override.has_value() && value_override.value() != override.value.get<String>())
                    || (!value_override.has_value()
                        && effective_command_value(node, CommandNames::createLink) != override.value.get<String>()))) {
                take_the_action_for_command(document, CommandNames::createLink, override.value.get<String>());
            }

            // 4. Otherwise, if override is a string; and command is "fontSize"; and either there is a value override
            //    for "fontSize" that is not equal to override, or there is no value override for "fontSize" and node's
            //    effective command value for "fontSize" is not loosely equivalent to override:
            else if (auto value_override = document.command_value_override(CommandNames::fontSize);
                override.value.has<String>() && override.command == CommandNames::fontSize
                && ((value_override.has_value() && value_override.value() != override.value.get<String>())
                    || (!value_override.has_value()
                        && !values_are_loosely_equivalent(
                            CommandNames::fontSize,
                            effective_command_value(node, CommandNames::fontSize),
                            override.value.get<String>())))) {
                // 1. Convert override to an integer number of pixels, and set override to the legacy font size for the
                //    result.
                auto override_pixel_size = font_size_to_pixel_size(override.value.get<String>());
                override.value = legacy_font_size(override_pixel_size.to_int());

                // 2. Take the action for "fontSize", with value equal to override.
                take_the_action_for_command(document, CommandNames::fontSize, override.value.get<String>());
            }

            // 5. Otherwise, continue this loop from the beginning.
            else {
                continue;
            }

            // 6. Set node to the first formattable node effectively contained in the active range, if there is one.
            if (auto new_formattable_node = first_formattable_node_effectively_contained(active_range(document)))
                node = new_formattable_node;
        }
    }

    // 3. Otherwise, for each (command, override) pair in overrides, in order:
    else {
        for (auto const& override : overrides) {
            // 1. If override is a boolean, set the state override for command to override.
            // 2. If override is a string, set the value override for command to override.
            override.value.visit(
                [&](bool value) { document.set_command_state_override(override.command, value); },
                [&](String const& value) { document.set_command_value_override(override.command, value); });
        }
    }
}

// https://w3c.github.io/editing/docs/execCommand/#restore-the-values
void restore_the_values_of_nodes(Vector<RecordedNodeValue> const& values)
{
    // 1. For each (node, command, value) triple in values:
    for (auto& recorded_node_value : values) {
        auto node = recorded_node_value.node;
        auto const& command = recorded_node_value.command;
        auto value = recorded_node_value.specified_command_value;

        // 1. Let ancestor equal node.
        GC::Ptr<DOM::Node> ancestor = node;

        // 2. If ancestor is not an Element, set it to its parent.
        if (!is<DOM::Element>(*ancestor))
            ancestor = ancestor->parent();

        // 3. While ancestor is an Element and its specified command value for command is null, set it to its parent.
        while (is<DOM::Element>(ancestor.ptr()) && !specified_command_value(static_cast<DOM::Element&>(*ancestor), command).has_value())
            ancestor = ancestor->parent();

        // 4. If value is null and ancestor is an Element, push down values on node for command, with new value null.
        if (!value.has_value() && is<DOM::Element>(ancestor.ptr())) {
            push_down_values(command, node, {});
        }

        // 5. Otherwise, if ancestor is an Element and its specified command value for command is not equivalent to
        //    value, or if ancestor is not an Element and value is not null, force the value of command to value on
        //    node.
        else if ((is<DOM::Element>(ancestor.ptr()) && specified_command_value(static_cast<DOM::Element&>(*ancestor), command) != value)
            || (!is<DOM::Element>(ancestor.ptr()) && value.has_value())) {
            force_the_value(node, command, value);
        }
    }
}

// https://w3c.github.io/editing/docs/execCommand/#selection's-list-state
SelectionsListState selections_list_state(DOM::Document const& document)
{
    // 1. If the active range is null, return "none".
    auto range = active_range(document);
    if (!range)
        return SelectionsListState::None;

    // 2. Block-extend the active range, and let new range be the result.
    auto new_range = block_extend_a_range(*range);

    // 3. Let node list be a list of nodes, initially empty.
    Vector<GC::Ref<DOM::Node>> node_list;

    // 4. For each node contained in new range, append node to node list if the last member of node list (if any) is not
    //    an ancestor of node; node is editable; node is not an indentation element; and node is either an ol or ul, or
    //    the child of an ol or ul, or an allowed child of "li".
    new_range->for_each_contained([&node_list](GC::Ref<DOM::Node> node) {
        if ((node_list.is_empty() || !node_list.last()->is_ancestor_of(node))
            && node->is_editable()
            && !is_indentation_element(node)
            && ((is<HTML::HTMLOListElement>(*node) || is<HTML::HTMLUListElement>(*node))
                || (is<HTML::HTMLOListElement>(node->parent()) || is<HTML::HTMLUListElement>(node->parent()))
                || is_allowed_child_of_node(node, HTML::TagNames::li)))
            node_list.append(node);
        return IterationDecision::Continue;
    });

    // 5. If node list is empty, return "none".
    if (node_list.is_empty())
        return SelectionsListState::None;

    // 6. If every member of node list is either an ol or the child of an ol or the child of an li child of an ol, and
    //    none is a ul or an ancestor of a ul, return "ol".
    auto is_ancestor_of_type = []<typename T>(GC::Ref<DOM::Node> node) {
        bool has_type = false;
        node->for_each_in_subtree([&has_type](GC::Ref<DOM::Node> descendant) {
            if (is<T>(*descendant)) {
                has_type = true;
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });
        return has_type;
    };
    auto is_type_or_child_of_list_type = []<typename T>(GC::Ref<DOM::Node> node) {
        return is<T>(*node) || is<T>(node->parent())
            || (is<HTML::HTMLLIElement>(node->parent()) && is<T>(node->parent()->parent()));
    };
    auto is_type_or_child_or_ancestor_of_list_type = [&]<typename T>(GC::Ref<DOM::Node> node) {
        return is_type_or_child_of_list_type.operator()<T>(node) || is_ancestor_of_type.operator()<T>(node);
    };

    bool all_is_an_ol = true;
    bool none_is_a_ul = true;
    for (auto node : node_list) {
        if (!is_type_or_child_of_list_type.operator()<HTML::HTMLOListElement>(*node)) {
            all_is_an_ol = false;
            break;
        }
        if (is<HTML::HTMLUListElement>(*node) || is_ancestor_of_type.operator()<HTML::HTMLUListElement>(node)) {
            none_is_a_ul = false;
            break;
        }
    }
    if (all_is_an_ol && none_is_a_ul)
        return SelectionsListState::Ol;

    // 7. If every member of node list is either a ul or the child of a ul or the child of an li child of a ul, and none
    //    is an ol or an ancestor of an ol, return "ul".
    bool all_is_a_ul = true;
    bool none_is_an_ol = true;
    for (auto node : node_list) {
        if (!is_type_or_child_of_list_type.operator()<HTML::HTMLUListElement>(*node)) {
            all_is_a_ul = false;
            break;
        }
        if (is<HTML::HTMLOListElement>(*node) || is_ancestor_of_type.operator()<HTML::HTMLOListElement>(node)) {
            none_is_an_ol = false;
            break;
        }
    }
    if (all_is_a_ul && none_is_an_ol)
        return SelectionsListState::Ul;

    // 8. If some member of node list is either an ol or the child or ancestor of an ol or the child of an li child of
    //    an ol, and some member of node list is either a ul or the child or ancestor of a ul or the child of an li
    //    child of a ul, return "mixed".
    bool any_is_ol = false;
    bool any_is_ul = false;
    for (auto node : node_list) {
        if (is_type_or_child_or_ancestor_of_list_type.operator()<HTML::HTMLOListElement>(*node))
            any_is_ol = true;
        if (is_type_or_child_or_ancestor_of_list_type.operator()<HTML::HTMLUListElement>(*node))
            any_is_ul = true;
        if (any_is_ol && any_is_ul)
            break;
    }
    if (any_is_ol && any_is_ul)
        return SelectionsListState::Mixed;

    // 9. If some member of node list is either an ol or the child or ancestor of an ol or the child of an li child of
    //    an ol, return "mixed ol".
    if (any_is_ol)
        return SelectionsListState::MixedOl;

    // 10. If some member of node list is either a ul or the child or ancestor of a ul or the child of an li child of a
    //     ul, return "mixed ul".
    if (any_is_ul)
        return SelectionsListState::MixedUl;

    // 11. Return "none".
    return SelectionsListState::None;
}

// https://w3c.github.io/editing/docs/execCommand/#set-the-selection's-value
void set_the_selections_value(DOM::Document& document, FlyString const& command, Optional<String> new_value)
{
    // 1. Let command be the current command.

    // 2. If there is no formattable node effectively contained in the active range:
    auto has_matching_node = false;
    for_each_node_effectively_contained_in_range(active_range(document), [&](GC::Ref<DOM::Node> descendant) {
        if (is_formattable_node(descendant)) {
            has_matching_node = true;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    if (!has_matching_node) {
        // 1. If command has inline command activated values, set the state override to true if new value is among them
        //    and false if it's not.
        auto command_definition = find_command_definition(command);
        // FIXME: remove .has_value() once all commands are implemented.
        if (command_definition.has_value() && !command_definition.value().inline_activated_values.is_empty()) {
            auto new_override = new_value.has_value() && command_definition.value().inline_activated_values.contains_slow(*new_value);
            document.set_command_state_override(command, new_override);
        }

        // 2. If command is "subscript", unset the state override for "superscript".
        if (command == CommandNames::subscript)
            document.clear_command_state_override(CommandNames::superscript);

        // 3. If command is "superscript", unset the state override for "subscript".
        if (command == CommandNames::superscript)
            document.clear_command_state_override(CommandNames::subscript);

        // 4. If new value is null, unset the value override (if any).
        if (!new_value.has_value()) {
            document.clear_command_value_override(command);
        }

        // 5. Otherwise, if command is "createLink" or it has a value specified, set the value override to new value.
        else if (command == CommandNames::createLink || !MUST(document.query_command_value(CommandNames::createLink)).is_empty()) {
            document.set_command_value_override(command, new_value.value());
        }

        // 6. Abort these steps.
        return;
    }

    // 3. If the active range's start node is an editable Text node, and its start offset is neither zero nor its start
    //    node's length, call splitText() on the active range's start node, with argument equal to the active range's
    //    start offset. Then set the active range's start node to the result, and its start offset to zero.
    auto range = active_range(document);
    auto start = range->start();
    if (start.node->is_editable() && is<DOM::Text>(*start.node) && start.offset != 0 && start.offset != start.node->length()) {
        auto new_node = MUST(static_cast<DOM::Text&>(*start.node).split_text(start.offset));
        MUST(range->set_start(new_node, 0));
    }

    // 4. If the active range's end node is an editable Text node, and its end offset is neither zero nor its end node's
    //    length, call splitText() on the active range's end node, with argument equal to the active range's end offset.
    auto end = range->end();
    if (end.node->is_editable() && is<DOM::Text>(*end.node) && end.offset != 0 && end.offset != end.node->length())
        MUST(static_cast<DOM::Text&>(*end.node).split_text(end.offset));

    // 5. Let element list be all editable Elements effectively contained in the active range.
    Vector<GC::Ref<DOM::Element>> element_list;
    for_each_node_effectively_contained_in_range(active_range(document), [&](GC::Ref<DOM::Node> descendant) {
        if (descendant->is_editable() && is<DOM::Element>(*descendant))
            element_list.append(static_cast<DOM::Element&>(*descendant));
        return TraversalDecision::Continue;
    });

    // 6. For each element in element list, clear the value of element.
    for (auto element : element_list)
        clear_the_value(command, element);

    // 7. Let node list be all editable nodes effectively contained in the active range.
    Vector<GC::Ref<DOM::Node>> node_list;
    for_each_node_effectively_contained_in_range(active_range(document), [&](GC::Ref<DOM::Node> descendant) {
        if (descendant->is_editable())
            node_list.append(descendant);
        return TraversalDecision::Continue;
    });

    // 8. For each node in node list:
    for (auto node : node_list) {
        // 1. Push down values on node.
        push_down_values(command, node, new_value);

        // 2. If node is an allowed child of "span", force the value of node.
        if (is_allowed_child_of_node(node, HTML::TagNames::span))
            force_the_value(node, command, new_value);
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
    // 1. If command is "backColor" or "hiliteColor" and the Element's display property does not have resolved value
    //    "inline", return null.
    if (command.is_one_of(CommandNames::backColor, CommandNames::hiliteColor)) {
        auto display = resolved_display(element);
        if (!display.has_value() || !display->is_inline_outside() || !display->is_flow_inside())
            return {};
    }

    // 2. If command is "createLink" or "unlink":
    if (command.is_one_of(CommandNames::createLink, CommandNames::unlink)) {
        // 1. If element is an a element and has an href attribute, return the value of that attribute.
        auto href_attribute = element->get_attribute(HTML::AttributeNames::href);
        if (href_attribute.has_value())
            return href_attribute.release_value();

        // 2. Return null.
        return {};
    }

    // 3. If command is "subscript" or "superscript":
    if (command.is_one_of(CommandNames::subscript, CommandNames::superscript)) {
        // 1. If element is a sup, return "superscript".
        if (element->local_name() == HTML::TagNames::sup)
            return "superscript"_string;

        // 2. If element is a sub, return "subscript".
        if (element->local_name() == HTML::TagNames::sub)
            return "subscript"_string;

        // 3. Return null.
        return {};
    }

    // 4. If command is "strikethrough", and element has a style attribute set, and that attribute sets
    //    "text-decoration":
    if (command == CommandNames::strikethrough) {
        auto text_decoration_style = property_in_style_attribute(element, CSS::PropertyID::TextDecoration);
        if (text_decoration_style.has_value()) {
            // 1. If element's style attribute sets "text-decoration" to a value containing "line-through", return
            //    "line-through".
            if (value_contains_keyword(text_decoration_style.value(), CSS::Keyword::LineThrough))
                return "line-through"_string;

            // 2. Return null.
            return {};
        }
    }

    // 5. If command is "strikethrough" and element is an s or strike element, return "line-through".
    if (command == CommandNames::strikethrough && element->local_name().is_one_of(HTML::TagNames::s, HTML::TagNames::strike))
        return "line-through"_string;

    // 6. If command is "underline", and element has a style attribute set, and that attribute sets "text-decoration":
    if (command == CommandNames::underline) {
        auto text_decoration_style = property_in_style_attribute(element, CSS::PropertyID::TextDecoration);
        if (text_decoration_style.has_value()) {
            // 1. If element's style attribute sets "text-decoration" to a value containing "underline", return "underline".
            if (value_contains_keyword(text_decoration_style.value(), CSS::Keyword::Underline))
                return "underline"_string;

            // 2. Return null.
            return {};
        }
    }

    // 7. If command is "underline" and element is a u element, return "underline".
    if (command == CommandNames::underline && element->local_name() == HTML::TagNames::u)
        return "underline"_string;

    // 8. Let property be the relevant CSS property for command.
    auto command_definition = find_command_definition(command);
    // FIXME: change this to VERIFY(command_definition.has_value()) once all command definitions are in place.
    if (!command_definition.has_value())
        return {};
    auto property = command_definition.value().relevant_css_property;

    // 9. If property is null, return null.
    if (!property.has_value())
        return {};

    // 10. If element has a style attribute set, and that attribute has the effect of setting property, return the value
    //     that it sets property to.
    auto style_value = property_in_style_attribute(element, property.value());
    if (style_value.has_value())
        return style_value.value()->to_string(CSS::CSSStyleValue::SerializationMode::Normal);

    // 11. If element is a font element that has an attribute whose effect is to create a presentational hint for
    //     property, return the value that the hint sets property to. (For a size of 7, this will be the non-CSS value
    //     "xxx-large".)
    if (is<HTML::HTMLFontElement>(*element)) {
        auto const& font_element = static_cast<HTML::HTMLFontElement&>(*element);
        auto cascaded_properties = font_element.document().heap().allocate<CSS::CascadedProperties>();
        font_element.apply_presentational_hints(cascaded_properties);
        auto property_value = cascaded_properties->property(property.value());
        if (property_value)
            return property_value->to_string(CSS::CSSStyleValue::SerializationMode::Normal);
    }

    // 12. If element is in the following list, and property is equal to the CSS property name listed for it, return the
    //     string listed for it.
    //     * b, strong: font-weight: "bold"
    //     * i, em: font-style: "italic"
    if (element->local_name().is_one_of(HTML::TagNames::b, HTML::TagNames::strong) && *property == CSS::PropertyID::FontWeight)
        return "bold"_string;
    if (element->local_name().is_one_of(HTML::TagNames::i, HTML::TagNames::em) && *property == CSS::PropertyID::FontStyle)
        return "italic"_string;

    // 13. Return null.
    return {};
}

// https://w3c.github.io/editing/docs/execCommand/#split-the-parent
void split_the_parent_of_nodes(Vector<GC::Ref<DOM::Node>> const& node_list)
{
    VERIFY(!node_list.is_empty());

    // 1. Let original parent be the parent of the first member of node list.
    GC::Ref<DOM::Node> first_node = *node_list.first();
    GC::Ref<DOM::Node> last_node = *node_list.last();
    GC::Ref<DOM::Node> original_parent = *first_node->parent();

    // 2. If original parent is not editable or its parent is null, do nothing and abort these
    //    steps.
    if (!original_parent->is_editable() || !original_parent->parent())
        return;

    // 3. If the first child of original parent is in node list, remove extraneous line breaks
    //    before original parent.
    GC::Ref<DOM::Node> first_child = *original_parent->first_child();
    auto first_child_in_nodes_list = node_list.contains_slow(first_child);
    if (first_child_in_nodes_list)
        remove_extraneous_line_breaks_before_node(original_parent);

    // 4. If the first child of original parent is in node list, and original parent follows a line
    //    break, set follows line break to true. Otherwise, set follows line break to false.
    auto follows_line_break = first_child_in_nodes_list && follows_a_line_break(original_parent);

    // 5. If the last child of original parent is in node list, and original parent precedes a line
    //    break, set precedes line break to true. Otherwise, set precedes line break to false.
    GC::Ref<DOM::Node> last_child = *original_parent->last_child();
    bool last_child_in_nodes_list = node_list.contains_slow(last_child);
    auto precedes_line_break = last_child_in_nodes_list && precedes_a_line_break(original_parent);

    // 6. If the first child of original parent is not in node list, but its last child is:
    GC::Ref<DOM::Node> parent_of_original_parent = *original_parent->parent();
    auto original_parent_index = original_parent->index();
    auto& document = original_parent->document();
    if (!first_child_in_nodes_list && last_child_in_nodes_list) {
        // 1. For each node in node list, in reverse order, insert node into the parent of original
        //    parent immediately after original parent, preserving ranges.
        for (auto node : node_list.in_reverse())
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
    for (auto node : node_list)
        move_node_preserving_ranges(node, parent_of_original_parent, original_parent_index++);

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

enum class ToggleListMode : u8 {
    Enable,
    Disable,
};

// https://w3c.github.io/editing/docs/execCommand/#toggle-lists
void toggle_lists(DOM::Document& document, FlyString const& tag_name)
{
    VERIFY(first_is_one_of(tag_name, HTML::TagNames::ol, HTML::TagNames::ul));

    // 1. Let mode be "disable" if the selection's list state is tag name, and "enable" otherwise.
    auto mode = ToggleListMode::Enable;
    auto list_state = selections_list_state(document);
    if ((list_state == SelectionsListState::Ol && tag_name == HTML::TagNames::ol)
        || (list_state == SelectionsListState::Ul && tag_name == HTML::TagNames::ul))
        mode = ToggleListMode::Disable;

    // 2. Let other tag name be "ol" if tag name is "ul", and "ul" if tag name is "ol".
    auto other_tag_name = tag_name == HTML::TagNames::ul ? HTML::TagNames::ol : HTML::TagNames::ul;

    // 3. Let items be a list of all lis that are inclusive ancestors of the active range's start and/or end node.
    Vector<GC::Ref<DOM::Node>> items;
    auto add_li_ancestors = [&items](GC::Ref<DOM::Node> node) {
        node->for_each_inclusive_ancestor([&items](GC::Ref<DOM::Node> ancestor) {
            if (is<HTML::HTMLLIElement>(*ancestor) && !items.contains_slow(ancestor))
                items.append(ancestor);
            return IterationDecision::Continue;
        });
    };
    auto range = active_range(document);
    add_li_ancestors(range->start_container());
    add_li_ancestors(range->end_container());

    // 4. For each item in items, normalize sublists of item.
    for (auto item : items)
        normalize_sublists_in_node(item);

    // 5. Block-extend the active range, and let new range be the result.
    auto new_range = block_extend_a_range(*active_range(document));

    // 6. If mode is "enable", then let lists to convert consist of every editable HTML element with local name other
    //    tag name that is contained in new range, and for every list in lists to convert:
    if (mode == ToggleListMode::Enable) {
        Vector<GC::Ref<DOM::Node>> lists_to_convert;
        new_range->for_each_contained([&](GC::Ref<DOM::Node> node) {
            if (node->is_editable() && is<HTML::HTMLElement>(*node)
                && static_cast<DOM::Element&>(*node).local_name() == other_tag_name)
                lists_to_convert.append(node);
            return IterationDecision::Continue;
        });
        for (auto list : lists_to_convert) {
            // 1. If list's previousSibling or nextSibling is an editable HTML element with local name tag name:
            if ((is<HTML::HTMLElement>(list->previous_sibling()) && list->previous_sibling()->is_editable()
                    && static_cast<DOM::Element&>(*list->previous_sibling()).local_name() == tag_name)
                || (is<HTML::HTMLElement>(list->next_sibling()) && list->next_sibling()->is_editable()
                    && static_cast<DOM::Element&>(*list->next_sibling()).local_name() == tag_name)) {
                // 1. Let children be list's children.
                Vector<GC::Ref<DOM::Node>> children;
                list->for_each_child([&children](GC::Ref<DOM::Node> child) {
                    children.append(child);
                    return IterationDecision::Continue;
                });

                // 2. Record the values of children, and let values be the result.
                auto values = record_the_values_of_nodes(children);

                // 3. Split the parent of children.
                split_the_parent_of_nodes(children);

                // 4. Wrap children, with sibling criteria returning true for an HTML element with local name tag name and
                //    false otherwise.
                wrap(
                    children,
                    [&tag_name](GC::Ref<DOM::Node> sibling) {
                        return is<HTML::HTMLElement>(*sibling)
                            && static_cast<DOM::Element&>(*sibling).local_name() == tag_name;
                    },
                    {});

                // 5. Restore the values from values.
                restore_the_values_of_nodes(values);
            }

            // 2. Otherwise, set the tag name of list to tag name.
            else {
                set_the_tag_name(static_cast<DOM::Element&>(*list), tag_name);
            }
        }
    }

    // 7. Let node list be a list of nodes, initially empty.
    Vector<GC::Ref<DOM::Node>> node_list;

    // 8. For each node node contained in new range, if node is editable; the last member of node list (if any) is not
    //    an ancestor of node; node is not an indentation element; and either node is an ol or ul, or its parent is an
    //    ol or ul, or it is an allowed child of "li"; then append node to node list.
    new_range->for_each_contained([&node_list](GC::Ref<DOM::Node> node) {
        if (node->is_editable()
            && (node_list.is_empty() || !node_list.last()->is_ancestor_of(node))
            && !is_indentation_element(node)
            && ((is<HTML::HTMLOListElement>(*node) || is<HTML::HTMLUListElement>(*node))
                || (is<HTML::HTMLOListElement>(node->parent()) || is<HTML::HTMLUListElement>(node->parent()))
                || is_allowed_child_of_node(node, HTML::TagNames::li)))
            node_list.append(node);
        return IterationDecision::Continue;
    });

    // 9. If mode is "enable", remove from node list any ol or ul whose parent is not also an ol or ul.
    if (mode == ToggleListMode::Enable) {
        node_list.remove_all_matching([](GC::Ref<DOM::Node> node) {
            return (is<HTML::HTMLOListElement>(*node) && !is<HTML::HTMLOListElement>(node->parent()))
                || (is<HTML::HTMLUListElement>(*node) && !is<HTML::HTMLUListElement>(node->parent()));
        });
    }

    // 10. If mode is "disable", then while node list is not empty:
    if (mode == ToggleListMode::Disable) {
        while (!node_list.is_empty()) {
            // 1. Let sublist be an empty list of nodes.
            Vector<GC::Ref<DOM::Node>> sublist;

            // 2. Remove the first member from node list and append it to sublist.
            sublist.append(node_list.take_first());

            // 3. If the first member of sublist is an HTML element with local name tag name, outdent it and continue this
            //    loop from the beginning.
            if (is<HTML::HTMLElement>(*sublist.first()) && static_cast<DOM::Element&>(*sublist.first()).local_name() == tag_name) {
                outdent(sublist.first());
                continue;
            }

            // 4. While node list is not empty, and the first member of node list is the nextSibling of the last member of
            //    sublist and is not an HTML element with local name tag name, remove the first member from node list and
            //    append it to sublist.
            while (!node_list.is_empty() && node_list.first().ptr() == sublist.last()->next_sibling()
                && !(is<HTML::HTMLElement>(*node_list.first()) && static_cast<DOM::Element&>(*node_list.first()).local_name() == tag_name))
                sublist.append(node_list.take_first());

            // 5. Record the values of sublist, and let values be the result.
            auto values = record_the_values_of_nodes(sublist);

            // 6. Split the parent of sublist.
            split_the_parent_of_nodes(sublist);

            // 7. Fix disallowed ancestors of each member of sublist.
            for (auto member : sublist)
                fix_disallowed_ancestors_of_node(member);

            // 8. Restore the values from values.
            restore_the_values_of_nodes(values);
        }
    }

    // 11. Otherwise, while node list is not empty:
    else {
        while (!node_list.is_empty()) {
            // 1. Let sublist be an empty list of nodes.
            Vector<GC::Ref<DOM::Node>> sublist;

            // 2. While either sublist is empty, or node list is not empty and its first member is the nextSibling of
            //    sublist's last member:
            while (sublist.is_empty() || (!node_list.is_empty() && node_list.first().ptr() == sublist.last()->next_sibling())) {
                // 1. If node list's first member is a p or div, set the tag name of node list's first member to "li",
                //    and append the result to sublist. Remove the first member from node list.
                if (is<HTML::HTMLParagraphElement>(*node_list.first()) || is<HTML::HTMLDivElement>(*node_list.first())) {
                    sublist.append(set_the_tag_name(static_cast<DOM::Element&>(*node_list.first()), HTML::TagNames::li));
                    node_list.take_first();
                }

                // 2. Otherwise, if the first member of node list is an li or ol or ul, remove it from node list and
                //    append it to sublist.
                else if (is<DOM::Element>(*node_list.first())
                    && (is<HTML::HTMLLIElement>(*node_list.first())
                        || is<HTML::HTMLOListElement>(*node_list.first())
                        || is<HTML::HTMLUListElement>(*node_list.first()))) {
                    sublist.append(node_list.take_first());
                }

                // 3. Otherwise:
                else {
                    // 1. Let nodes to wrap be a list of nodes, initially empty.
                    Vector<GC::Ref<DOM::Node>> nodes_to_wrap;

                    // 2. While nodes to wrap is empty, or node list is not empty and its first member is the
                    //    nextSibling of nodes to wrap's last member and the first member of node list is an inline node
                    //    and the last member of nodes to wrap is an inline node other than a br, remove the first
                    //    member from node list and append it to nodes to wrap.
                    while (nodes_to_wrap.is_empty() || (!node_list.is_empty() && node_list.first().ptr() == nodes_to_wrap.last()->next_sibling() && is_inline_node(node_list.first()) && is_inline_node(nodes_to_wrap.last()) && !is<HTML::HTMLBRElement>(*nodes_to_wrap.last())))
                        nodes_to_wrap.append(node_list.take_first());

                    // 3. Wrap nodes to wrap, with new parent instructions returning the result of calling
                    //    createElement("li") on the context object. Append the result to sublist.
                    auto result = wrap(
                        nodes_to_wrap,
                        {},
                        [&] { return MUST(DOM::create_element(document, HTML::TagNames::li, Namespace::HTML)); });
                    if (result)
                        sublist.append(*result);
                }
            }

            // 3. If sublist's first member's parent is an HTML element with local name tag name, or if every member of
            //    sublist is an ol or ul, continue this loop from the beginning.
            if (!sublist.is_empty() && is<HTML::HTMLElement>(sublist.first()->parent())
                && static_cast<DOM::Element&>(*sublist.first()->parent()).local_name() == tag_name)
                continue;
            bool all_are_ol_or_ul = true;
            for (auto member : sublist) {
                if (!is<HTML::HTMLOListElement>(*member) && !is<HTML::HTMLUListElement>(*member)) {
                    all_are_ol_or_ul = false;
                    break;
                }
            }
            if (all_are_ol_or_ul)
                continue;

            // 4. If sublist's first member's parent is an HTML element with local name other tag name:
            if (!sublist.is_empty() && is<HTML::HTMLElement>(sublist.first()->parent())
                && static_cast<DOM::Element&>(*sublist.first()->parent()).local_name() == other_tag_name) {
                // 1. Record the values of sublist, and let values be the result.
                auto values = record_the_values_of_nodes(sublist);

                // 2. Split the parent of sublist.
                split_the_parent_of_nodes(sublist);

                // 3. Wrap sublist, with sibling criteria returning true for an HTML element with local name tag name
                //    and false otherwise, and new parent instructions returning the result of calling
                //    createElement(tag name) on the context object.
                wrap(
                    sublist,
                    [&](GC::Ref<DOM::Node> sibling) {
                        return is<HTML::HTMLElement>(*sibling)
                            && static_cast<DOM::Element&>(*sibling).local_name() == tag_name;
                    },
                    [&] { return MUST(DOM::create_element(document, tag_name, Namespace::HTML)); });

                // 4. Restore the values from values.
                restore_the_values_of_nodes(values);

                // 5. Continue this loop from the beginning.
                continue;
            }

            // 5. Wrap sublist, with sibling criteria returning true for an HTML element with local name tag name and
            //    false otherwise, and new parent instructions being the following:
            auto result = wrap(
                sublist,
                [&](GC::Ref<DOM::Node> sibling) {
                    return is<HTML::HTMLElement>(*sibling)
                        && static_cast<DOM::Element&>(*sibling).local_name() == tag_name;
                },
                [&] -> GC::Ptr<DOM::Node> {
                    // 1. If sublist's first member's parent is not an editable simple indentation element, or sublist's
                    //    first member's parent's previousSibling is not an editable HTML element with local name tag name,
                    //    call createElement(tag name) on the context object and return the result.
                    auto* first_parent = sublist.first()->parent();
                    if (!first_parent->is_editable() || !is_simple_indentation_element(*first_parent)
                        || !(is<HTML::HTMLElement>(first_parent->previous_sibling())
                            && static_cast<DOM::Element&>(*first_parent->previous_sibling()).local_name() == tag_name))
                        return MUST(DOM::create_element(document, tag_name, Namespace::HTML));

                    // 2. Let list be sublist's first member's parent's previousSibling.
                    GC::Ref<DOM::Node> list = *sublist.first()->parent()->previous_sibling();

                    // 3. Normalize sublists of list's lastChild.
                    normalize_sublists_in_node(*list->last_child());

                    // 4. If list's lastChild is not an editable HTML element with local name tag name, call
                    //    createElement(tag name) on the context object, and append the result as the last child of list.
                    if (!list->last_child()->is_editable() || !is<HTML::HTMLElement>(list->last_child())
                        || static_cast<DOM::Element&>(*list->last_child()).local_name() != tag_name)
                        MUST(list->append_child(MUST(DOM::create_element(document, tag_name, Namespace::HTML))));

                    // 5. Return the last child of list.
                    return list->last_child();
                });

            // 6. Fix disallowed ancestors of the previous step's result.
            if (result)
                fix_disallowed_ancestors_of_node(*result);
        }
    }
}

// https://w3c.github.io/editing/docs/execCommand/#equivalent-values
bool values_are_equivalent(FlyString const& command, Optional<String> a, Optional<String> b)
{
    // Two quantities are equivalent values for a command if either both are null,
    if (!a.has_value() && !b.has_value())
        return true;

    // NOTE: Both need to be strings for all remaining conditions.
    if (!a.has_value() || !b.has_value())
        return false;

    // or both are strings and the command defines equivalent values and they match the definition.
    if (command.is_one_of(CommandNames::backColor, CommandNames::foreColor, CommandNames::hiliteColor)) {
        // Either both strings are valid CSS colors and have the same red, green, blue, and alpha components, or neither
        // string is a valid CSS color.
        auto a_color = Color::from_string(a.value());
        auto b_color = Color::from_string(b.value());
        if (a_color.has_value())
            return a_color == b_color;
        return !a_color.has_value() && !b_color.has_value();
    }
    if (command == CommandNames::bold) {
        // Either the two strings are equal, or one is "bold" and the other is "700", or one is "normal" and the other
        // is "400".
        if (a.value() == b.value())
            return true;

        auto either_is_bold = first_is_one_of("bold"sv, a.value(), b.value());
        auto either_is_700 = first_is_one_of("700"sv, a.value(), b.value());
        auto either_is_normal = first_is_one_of("normal"sv, a.value(), b.value());
        auto either_is_400 = first_is_one_of("400"sv, a.value(), b.value());

        return (either_is_bold && either_is_700) || (either_is_normal && either_is_400);
    }

    // or both are strings and they're equal and the command does not define any equivalent values,
    return a.value() == b.value();
}

// https://w3c.github.io/editing/docs/execCommand/#loosely-equivalent-values
bool values_are_loosely_equivalent(FlyString const& command, Optional<String> a, Optional<String> b)
{
    // Two quantities are loosely equivalent values for a command if either they are equivalent values for the command,
    if (values_are_equivalent(command, a, b))
        return true;

    // or if the command is the fontSize command; one of the quantities is one of "x-small", "small", "medium", "large",
    // "x-large", "xx-large", or "xxx-large"; and the other quantity is the resolved value of "font-size" on a font
    // element whose size attribute has the corresponding value set ("1" through "7" respectively).
    if (command == CommandNames::fontSize && a.has_value() && b.has_value()) {
        static constexpr Array named_quantities { "x-small"sv, "small"sv, "medium"sv, "large"sv, "x-large"sv,
            "xx-large"sv, "xxx-large"sv };
        static constexpr Array size_quantities { "1"sv, "2"sv, "3"sv, "4"sv, "5"sv, "6"sv, "7"sv };
        static_assert(named_quantities.size() == size_quantities.size());

        auto a_index = named_quantities.first_index_of(a.value())
                           .value_or_lazy_evaluated_optional([&] { return size_quantities.first_index_of(a.value()); });
        auto b_index = named_quantities.first_index_of(b.value())
                           .value_or_lazy_evaluated_optional([&] { return size_quantities.first_index_of(b.value()); });

        return a_index.has_value() && a_index == b_index;
    }

    return false;
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

        // 2. If any range has a boundary point with node equal to the parent of new parent and offset equal to the
        //    index of new parent, add one to that boundary point's offset.
        auto new_parent_index = new_parent->index();
        auto active_range = new_parent->document().get_selection()->range();
        if (active_range && active_range->start_container() == new_parent && active_range->start_offset() == new_parent_index)
            MUST(active_range->set_start(active_range->start_container(), new_parent_index + 1));
        if (active_range && active_range->end_container() == new_parent && active_range->end_offset() == new_parent_index)
            MUST(active_range->set_end(active_range->end_container(), new_parent_index + 1));
    }

    // 11. Let original parent be the parent of the first member of node list.
    auto const original_parent = GC::Ptr { node_list.first()->parent() };

    // 12. If new parent is before the first member of node list in tree order:
    if (new_parent->is_before(node_list.first())) {
        // 1. If new parent is not an inline node, but the last visible child of new parent and the first visible member
        //    of node list are both inline nodes, and the last child of new parent is not a br, call createElement("br")
        //    on the ownerDocument of new parent and append the result as the last child of new parent.
        if (!is_inline_node(*new_parent)) {
            auto last_visible_child = [&] -> GC::Ptr<DOM::Node> {
                GC::Ptr<DOM::Node> child = new_parent->last_child();
                while (child) {
                    if (is_visible_node(*child))
                        return *child;
                    child = child->previous_sibling();
                }
                return {};
            }();
            auto first_visible_member = [&] -> GC::Ptr<DOM::Node> {
                for (auto& member : node_list) {
                    if (is_visible_node(member))
                        return member;
                }
                return {};
            }();
            if (last_visible_child && is_inline_node(*last_visible_child) && first_visible_member && is_inline_node(*first_visible_member)
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

GC::Ptr<DOM::Node> first_formattable_node_effectively_contained(GC::Ptr<DOM::Range> range)
{
    GC::Ptr<DOM::Node> node;
    for_each_node_effectively_contained_in_range(range, [&](GC::Ref<DOM::Node> descendant) {
        if (is_formattable_node(descendant)) {
            node = descendant;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    return node;
}

CSSPixels font_size_to_pixel_size(StringView font_size)
{
    // If the font size ends in 'px', interpret the preceding as a number and return it.
    if (font_size.length() >= 2 && font_size.substring_view(font_size.length() - 2).equals_ignoring_ascii_case("px"sv)) {
        auto optional_number = font_size.substring_view(0, font_size.length() - 2).to_number<float>();
        if (optional_number.has_value())
            return CSSPixels::nearest_value_for(optional_number.value());
    }

    // Try to map the font size directly to a keyword (e.g. medium or x-large)
    auto keyword = CSS::keyword_from_string(font_size);

    // If that failed, try to interpret it as a legacy font size (e.g. 1 through 7)
    if (!keyword.has_value())
        keyword = HTML::HTMLFontElement::parse_legacy_font_size(font_size);

    // If that also failed, give up
    auto pixel_size = CSS::StyleComputer::default_user_font_size();
    if (!keyword.has_value())
        return pixel_size;

    // Return scaled pixel size
    return pixel_size * CSS::StyleComputer::absolute_size_mapping(keyword.release_value());
}

void for_each_node_effectively_contained_in_range(GC::Ptr<DOM::Range> range, Function<TraversalDecision(GC::Ref<DOM::Node>)> callback)
{
    if (!range)
        return;

    // A node can still be "effectively contained" in range even if it's not actually contained within the range; so we
    // need to do an inclusive subtree traversal since the common ancestor could be matched as well.
    range->common_ancestor_container()->for_each_in_inclusive_subtree([&](GC::Ref<DOM::Node> descendant) {
        if (!is_effectively_contained_in_range(descendant, *range)) {
            // NOTE: We cannot skip children here since if a descendant is not effectively contained within a range, its
            //       children might still be.
            return TraversalDecision::Continue;
        }
        return callback(descendant);
    });
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

String justify_alignment_to_string(JustifyAlignment alignment)
{
    switch (alignment) {
    case JustifyAlignment::Center:
        return "center"_string;
    case JustifyAlignment::Justify:
        return "justify"_string;
    case JustifyAlignment::Left:
        return "left"_string;
    case JustifyAlignment::Right:
        return "right"_string;
    }
    VERIFY_NOT_REACHED();
}

Array<StringView, 7> named_font_sizes()
{
    return { "x-small"sv, "small"sv, "medium"sv, "large"sv, "x-large"sv, "xx-large"sv, "xxx-large"sv };
}

Optional<NonnullRefPtr<CSS::CSSStyleValue const>> property_in_style_attribute(GC::Ref<DOM::Element> element, CSS::PropertyID property_id)
{
    auto inline_style = element->inline_style();
    if (!inline_style)
        return {};

    auto style_property = inline_style->property(property_id);
    if (!style_property.has_value())
        return {};

    return style_property.value().value;
}

Optional<CSS::Display> resolved_display(GC::Ref<DOM::Node> node)
{
    auto resolved_property = resolved_value(node, CSS::PropertyID::Display);
    if (!resolved_property.has_value() || !resolved_property.value()->is_display())
        return {};
    return resolved_property.value()->as_display().display();
}

Optional<CSS::Keyword> resolved_keyword(GC::Ref<DOM::Node> node, CSS::PropertyID property_id)
{
    auto resolved_property = resolved_value(node, property_id);
    if (!resolved_property.has_value() || !resolved_property.value()->is_keyword())
        return {};
    return resolved_property.value()->as_keyword().keyword();
}

Optional<NonnullRefPtr<CSS::CSSStyleValue const>> resolved_value(GC::Ref<DOM::Node> node, CSS::PropertyID property_id)
{
    // Find the nearest inclusive ancestor of node that is an Element. This allows for passing in a DOM::Text node.
    GC::Ptr<DOM::Node> element = node;
    while (element && !is<DOM::Element>(*element))
        element = element->parent();
    if (!element)
        return {};

    // Retrieve resolved style value
    auto resolved_css_style_declaration = CSS::CSSStyleProperties::create_resolved_style({ static_cast<DOM::Element&>(*element) });
    auto optional_style_property = resolved_css_style_declaration->property(property_id);
    if (!optional_style_property.has_value())
        return {};
    return optional_style_property.value().value;
}

void take_the_action_for_command(DOM::Document& document, FlyString const& command, String const& value)
{
    auto const& command_definition = find_command_definition(command);
    // FIXME: replace with VERIFY(command_definition.has_value()) as soon as all command definitions are in place.
    if (command_definition.has_value())
        command_definition->action(document, value);
}

bool value_contains_keyword(CSS::CSSStyleValue const& value, CSS::Keyword keyword)
{
    if (value.is_value_list()) {
        for (auto& css_style_value : value.as_value_list().values()) {
            if (css_style_value->is_keyword() && css_style_value->as_keyword().keyword() == keyword)
                return true;
        }
    }
    return value.to_keyword() == keyword;
}

}
