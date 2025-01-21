/*
 * Copyright (c) 2024-2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/DOM/Comment.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/CommandNames.h>
#include <LibWeb/Editing/Commands.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLHRElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLLIElement.h>
#include <LibWeb/HTML/HTMLTableElement.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Namespace.h>

namespace Web::Editing {

// https://w3c.github.io/editing/docs/execCommand/#the-backcolor-command
bool command_back_color_action(DOM::Document& document, String const& value)
{
    // 1. If value is not a valid CSS color, prepend "#" to it.
    auto resulting_value = value;
    if (!Color::from_string(resulting_value).has_value()) {
        resulting_value = MUST(String::formatted("#{}", resulting_value));

        // 2. If value is still not a valid CSS color, or if it is currentColor, return false.
        if (!Color::from_string(resulting_value).has_value()) {
            // FIXME: Also return false in case of currentColor.
            return false;
        }
    }

    // 3. Set the selection's value to value.
    set_the_selections_value(document, CommandNames::backColor, resulting_value);

    // 4. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-bold-command
bool command_bold_action(DOM::Document& document, String const&)
{
    // If queryCommandState("bold") returns true, set the selection's value to "normal".
    if (MUST(document.query_command_state(CommandNames::bold))) {
        set_the_selections_value(document, CommandNames::bold, "normal"_string);
    }

    // Otherwise set the selection's value to "bold".
    else {
        set_the_selections_value(document, CommandNames::bold, "bold"_string);
    }

    // Either way, return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-createlink-command
bool command_create_link_action(DOM::Document& document, String const& value)
{
    // 1. If value is the empty string, return false.
    if (value.is_empty())
        return false;

    // 2. For each editable a element that has an href attribute and is an ancestor of some node effectively contained
    //    in the active range, set that a element's href attribute to value.
    HashTable<DOM::Node*> visited_ancestors;
    auto set_value_for_ancestor_anchors = [&](GC::Ref<DOM::Node> node) {
        node->for_each_ancestor([&](GC::Ref<DOM::Node> ancestor) {
            if (visited_ancestors.contains(ancestor.ptr()))
                return IterationDecision::Break;
            if (auto* anchor = as_if<HTML::HTMLAnchorElement>(*ancestor); anchor && anchor->is_editable()
                && anchor->has_attribute(HTML::AttributeNames::href))
                MUST(anchor->set_href(value));
            visited_ancestors.set(ancestor.ptr());
            return IterationDecision::Continue;
        });
    };
    for_each_node_effectively_contained_in_range(active_range(document), [&](GC::Ref<DOM::Node> descendant) {
        set_value_for_ancestor_anchors(descendant);
        return TraversalDecision::Continue;
    });

    // 3. Set the selection's value to value.
    set_the_selections_value(document, CommandNames::createLink, value);

    // 4. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-defaultparagraphseparator-command
bool command_default_paragraph_separator_action(DOM::Document& document, String const& input_value)
{
    // Let value be converted to ASCII lowercase.
    auto value = input_value.to_ascii_lowercase();

    // If value is then equal to "p" or "div", set the context object's default single-line
    // container name to value, then return true.
    if (value == HTML::TagNames::p) {
        document.set_default_single_line_container_name(HTML::TagNames::p);
        return true;
    }
    if (value == HTML::TagNames::div) {
        document.set_default_single_line_container_name(HTML::TagNames::div);
        return true;
    }

    // Otherwise, return false.
    return false;
}

// https://w3c.github.io/editing/docs/execCommand/#the-defaultparagraphseparator-command
String command_default_paragraph_separator_value(DOM::Document const& document)
{
    // Return the context object's default single-line container name.
    return document.default_single_line_container_name().to_string();
}

// https://w3c.github.io/editing/docs/execCommand/#the-delete-command
bool command_delete_action(DOM::Document& document, String const&)
{
    // 1. If the active range is not collapsed, delete the selection and return true.
    auto& selection = *document.get_selection();
    auto& active_range = *selection.range();
    if (!active_range.collapsed()) {
        delete_the_selection(selection);
        return true;
    }

    // 2. Canonicalize whitespace at the active range's start.
    canonicalize_whitespace(active_range.start());

    // 3. Let node and offset be the active range's start node and offset.
    GC::Ptr<DOM::Node> node = active_range.start_container();
    int offset = active_range.start_offset();

    // 4. Repeat the following steps:
    GC::Ptr<DOM::Node> offset_minus_one_child;
    while (true) {
        offset_minus_one_child = node->child_at_index(offset - 1);

        // 1. If offset is zero and node's previousSibling is an editable invisible node, remove
        //    node's previousSibling from its parent.
        if (auto* previous_sibling = node->previous_sibling()) {
            if (offset == 0 && previous_sibling->is_editable() && is_invisible_node(*previous_sibling)) {
                previous_sibling->remove();
                continue;
            }
        }

        // 2. Otherwise, if node has a child with index offset − 1 and that child is an editable
        //    invisible node, remove that child from node, then subtract one from offset.
        if (offset_minus_one_child && offset_minus_one_child->is_editable() && is_invisible_node(*offset_minus_one_child)) {
            offset_minus_one_child->remove();
            --offset;
            continue;
        }

        // 3. Otherwise, if offset is zero and node is an inline node, or if node is an invisible
        //    node, set offset to the index of node, then set node to its parent.
        if ((offset == 0 && is_inline_node(*node)) || is_invisible_node(*node)) {
            offset = node->index();
            node = *node->parent();
            continue;
        }

        // 4. Otherwise, if node has a child with index offset − 1 and that child is an editable a,
        //    remove that child from node, preserving its descendants. Then return true.
        if (is<HTML::HTMLAnchorElement>(offset_minus_one_child.ptr()) && offset_minus_one_child->is_editable()) {
            remove_node_preserving_its_descendants(*offset_minus_one_child);
            return true;
        }

        // 5. Otherwise, if node has a child with index offset − 1 and that child is not a block
        //    node or a br or an img, set node to that child, then set offset to the length of node.
        if (offset_minus_one_child && !is_block_node(*offset_minus_one_child)
            && !is<HTML::HTMLBRElement>(*offset_minus_one_child) && !is<HTML::HTMLImageElement>(*offset_minus_one_child)) {
            node = *offset_minus_one_child;
            offset = node->length();
            continue;
        }

        // 6. Otherwise, break from this loop.
        break;
    }

    // 5. If node is a Text node and offset is not zero, or if node is a block node that has a child
    //    with index offset − 1 and that child is a br or hr or img:
    bool block_node_child_is_relevant_type = false;
    if (is_block_node(*node)) {
        if (auto* child_node = node->child_at_index(offset - 1)) {
            auto& child_element = static_cast<DOM::Element&>(*child_node);
            block_node_child_is_relevant_type = child_element.local_name().is_one_of(HTML::TagNames::br, HTML::TagNames::hr, HTML::TagNames::img);
        }
    }
    if ((is<DOM::Text>(*node) && offset != 0) || block_node_child_is_relevant_type) {
        // 1. Call collapse(node, offset) on the context object's selection.
        MUST(selection.collapse(node, offset));

        // 2. Call extend(node, offset − 1) on the context object's selection.
        MUST(selection.extend(*node, offset - 1));

        // 3. Delete the selection.
        delete_the_selection(selection);

        // 4. Return true.
        return true;
    }

    // 6. If node is an inline node, return true.
    if (is_inline_node(*node))
        return true;

    // 7. If node is an li or dt or dd and is the first child of its parent, and offset is zero:
    auto& node_element = static_cast<DOM::Element&>(*node);
    if (offset == 0 && node->index() == 0
        && node_element.local_name().is_one_of(HTML::TagNames::li, HTML::TagNames::dt, HTML::TagNames::dd)) {
        // 1. Let items be a list of all lis that are ancestors of node.
        Vector<GC::Ref<DOM::Node>> items;
        node->for_each_ancestor([&items](GC::Ref<DOM::Node> ancestor) {
            if (is<HTML::HTMLLIElement>(*ancestor))
                items.append(ancestor);
            return IterationDecision::Continue;
        });

        // 2. Normalize sublists of each item in items.
        for (auto item : items)
            normalize_sublists_in_node(item);

        // 3. Record the values of the one-node list consisting of node, and let values be the
        //    result.
        auto values = record_the_values_of_nodes({ *node });

        // 4. Split the parent of the one-node list consisting of node.
        split_the_parent_of_nodes({ *node });

        // 5. Restore the values from values.
        restore_the_values_of_nodes(values);

        // 6. If node is a dd or dt, and it is not an allowed child of any of its ancestors in the
        //    same editing host, set the tag name of node to the default single-line container name
        //    and let node be the result.
        if (node_element.local_name().is_one_of(HTML::TagNames::dd, HTML::TagNames::dt)) {
            bool allowed_child_of_any_ancestor = false;
            node->for_each_ancestor([&](GC::Ref<DOM::Node> ancestor) {
                if (is_in_same_editing_host(*node, ancestor) && is_allowed_child_of_node(GC::Ref { *node }, ancestor)) {
                    allowed_child_of_any_ancestor = true;
                    return IterationDecision::Break;
                }
                return IterationDecision::Continue;
            });
            if (!allowed_child_of_any_ancestor)
                node = set_the_tag_name(node_element, document.default_single_line_container_name());
        }

        // 7. Fix disallowed ancestors of node.
        fix_disallowed_ancestors_of_node(*node);

        // 8. Return true.
        return true;
    }

    // 8. Let start node equal node and let start offset equal offset.
    auto start_node = node;
    auto start_offset = offset;

    // 9. Repeat the following steps:
    while (true) {
        // AD-HOC: If start node is not a Node, return false. This prevents a crash by dereferencing a null pointer in
        //         step 1 below. Edits outside of <body> might be prohibited: https://github.com/w3c/editing/issues/405
        if (!start_node)
            return false;

        // 1. If start offset is zero, set start offset to the index of start node and then set
        //    start node to its parent.
        if (start_offset == 0) {
            start_offset = start_node->index();
            start_node = start_node->parent();
            continue;
        }

        // 2. Otherwise, if start node has an editable invisible child with index start offset minus
        //    one, remove it from start node and subtract one from start offset.
        offset_minus_one_child = start_node->child_at_index(start_offset - 1);
        if (offset_minus_one_child && offset_minus_one_child->is_editable() && is_invisible_node(*offset_minus_one_child)) {
            offset_minus_one_child->remove();
            --start_offset;
            continue;
        }

        // 3. Otherwise, break from this loop.
        break;
    }

    // 10. If offset is zero, and node has an editable inclusive ancestor in the same editing host
    //     that's an indentation element:
    if (offset == 0) {
        bool has_matching_inclusive_ancestor = false;
        node->for_each_inclusive_ancestor([&](GC::Ref<DOM::Node> ancestor) {
            if (ancestor->is_editable() && is_in_same_editing_host(ancestor, *node)
                && is_indentation_element(ancestor)) {
                has_matching_inclusive_ancestor = true;
                return IterationDecision::Break;
            }
            return IterationDecision::Continue;
        });
        if (has_matching_inclusive_ancestor) {
            // 1. Block-extend the range whose start and end are both (node, 0), and let new range be
            //    the result.
            auto new_range = block_extend_a_range(DOM::Range::create(*node, 0, *node, 0));

            // 2. Let node list be a list of nodes, initially empty.
            Vector<GC::Ref<DOM::Node>> node_list;

            // 3. For each node current node contained in new range, append current node to node list if
            //    the last member of node list (if any) is not an ancestor of current node, and current
            //    node is editable but has no editable descendants.
            new_range->for_each_contained([&node_list](GC::Ref<DOM::Node> current_node) {
                if (!node_list.is_empty() && node_list.last()->is_ancestor_of(current_node))
                    return IterationDecision::Continue;

                if (!current_node->is_editable())
                    return IterationDecision::Continue;

                bool has_editable_descendant = false;
                current_node->for_each_in_subtree([&](DOM::Node const& descendant) {
                    if (descendant.is_editable()) {
                        has_editable_descendant = true;
                        return TraversalDecision::Break;
                    }
                    return TraversalDecision::Continue;
                });
                if (!has_editable_descendant)
                    node_list.append(current_node);

                return IterationDecision::Continue;
            });

            // 4. Outdent each node in node list.
            for (auto current_node : node_list)
                outdent(current_node);

            // 5. Return true.
            return true;
        }
    }

    // 11. If the child of start node with index start offset is a table, return true.
    if (is<HTML::HTMLTableElement>(start_node->child_at_index(start_offset)))
        return true;

    // 12. If start node has a child with index start offset − 1, and that child is a table:
    offset_minus_one_child = start_node->child_at_index(start_offset - 1);
    if (is<HTML::HTMLTableElement>(offset_minus_one_child.ptr())) {
        // 1. Call collapse(start node, start offset − 1) on the context object's selection.
        MUST(selection.collapse(start_node, start_offset - 1));

        // 2. Call extend(start node, start offset) on the context object's selection.
        MUST(selection.extend(*start_node, start_offset));

        // 3. Return true.
        return true;
    }

    // 13. If offset is zero; and either the child of start node with index start offset minus one
    //     is an hr, or the child is a br whose previousSibling is either a br or not an inline
    //     node:
    if (offset == 0 && is<DOM::Element>(offset_minus_one_child.ptr())) {
        auto& child_element = static_cast<DOM::Element&>(*offset_minus_one_child);
        auto* previous_sibling = child_element.previous_sibling();
        if (is<HTML::HTMLHRElement>(child_element)
            || (is<HTML::HTMLBRElement>(child_element) && previous_sibling && (is<HTML::HTMLBRElement>(*previous_sibling) || !is_inline_node(*previous_sibling)))) {
            // 1. Call collapse(start node, start offset − 1) on the context object's selection.
            MUST(selection.collapse(start_node, start_offset - 1));

            // 2. Call extend(start node, start offset) on the context object's selection.
            MUST(selection.extend(*start_node, start_offset));

            // 3. Delete the selection.
            delete_the_selection(selection);

            // 4. Call collapse(node, offset) on the selection.
            MUST(selection.collapse(node, offset));

            // 5. Return true.
            return true;
        }
    }

    // 14. If the child of start node with index start offset is an li or dt or dd, and that child's
    //     firstChild is an inline node, and start offset is not zero:
    // NOTE: step 9 above guarantees start_offset cannot be 0 here.
    auto is_li_dt_or_dd = [](DOM::Element const& node) {
        return node.local_name().is_one_of(HTML::TagNames::li, HTML::TagNames::dt, HTML::TagNames::dd);
    };
    auto* start_offset_child = start_node->child_at_index(start_offset);
    if (is<DOM::Element>(start_offset_child) && is_li_dt_or_dd(static_cast<DOM::Element&>(*start_offset_child))
        && start_offset_child->has_children() && is_inline_node(*start_offset_child->first_child())) {
        // 1. Let previous item be the child of start node with index start offset minus one.
        GC::Ref<DOM::Node> previous_item = *start_node->child_at_index(start_offset - 1);

        // 2. If previous item's lastChild is an inline node other than a br, call
        //    createElement("br") on the context object and append the result as the last child of
        //    previous item.
        GC::Ptr<DOM::Node> previous_item_last_child = previous_item->last_child();
        if (previous_item_last_child && is_inline_node(*previous_item_last_child) && !is<HTML::HTMLBRElement>(*previous_item_last_child)) {
            auto br_element = MUST(DOM::create_element(previous_item->document(), HTML::TagNames::br, Namespace::HTML));
            MUST(previous_item->append_child(br_element));
        }

        // 3. If previous item's lastChild is an inline node, call createElement("br") on the
        //    context object and append the result as the last child of previous item.
        if (previous_item_last_child && is_inline_node(*previous_item_last_child)) {
            auto br_element = MUST(DOM::create_element(previous_item->document(), HTML::TagNames::br, Namespace::HTML));
            MUST(previous_item->append_child(br_element));
        }
    }

    // 15. If start node's child with index start offset is an li or dt or dd, and that child's
    //     previousSibling is also an li or dt or dd:
    if (is<DOM::Element>(start_offset_child) && is_li_dt_or_dd(static_cast<DOM::Element&>(*start_offset_child))
        && is<DOM::Element>(start_offset_child->previous_sibling())
        && is_li_dt_or_dd(static_cast<DOM::Element&>(*start_offset_child->previous_sibling()))) {
        // 1. Call cloneRange() on the active range, and let original range be the result.
        auto original_range = active_range.clone_range();

        // 2. Set start node to its child with index start offset − 1.
        start_node = start_node->child_at_index(start_offset - 1);

        // 3. Set start offset to start node's length.
        start_offset = start_node->length();

        // 4. Set node to start node's nextSibling.
        node = start_node->next_sibling();

        // 5. Call collapse(start node, start offset) on the context object's selection.
        MUST(selection.collapse(start_node, start_offset));

        // 6. Call extend(node, 0) on the context object's selection.
        MUST(selection.extend(*node, 0));

        // 7. Delete the selection.
        delete_the_selection(selection);

        // 8. Call removeAllRanges() on the context object's selection.
        selection.remove_all_ranges();

        // 9. Call addRange(original range) on the context object's selection.
        selection.add_range(original_range);

        // 10. Return true.
        return true;
    }

    // 16. While start node has a child with index start offset minus one:
    while (start_node->child_at_index(start_offset - 1) != nullptr) {
        // 1. If start node's child with index start offset minus one is editable and invisible,
        //    remove it from start node, then subtract one from start offset.
        offset_minus_one_child = start_node->child_at_index(start_offset - 1);
        if (offset_minus_one_child->is_editable() && is_invisible_node(*offset_minus_one_child)) {
            offset_minus_one_child->remove();
            --start_offset;
        }

        // 2. Otherwise, set start node to its child with index start offset minus one, then set
        //    start offset to the length of start node.
        else {
            start_node = *offset_minus_one_child;
            start_offset = start_node->length();
        }
    }

    // 17. Call collapse(start node, start offset) on the context object's selection.
    MUST(selection.collapse(start_node, start_offset));

    // 18. Call extend(node, offset) on the context object's selection.
    MUST(selection.extend(*node, offset));

    // 19. Delete the selection, with direction "backward".
    delete_the_selection(selection, true, true, Selection::Direction::Backwards);

    // 20. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-fontname-command
bool command_font_name_action(DOM::Document& document, String const& value)
{
    // Set the selection's value to value, then return true.
    set_the_selections_value(document, CommandNames::fontName, value);
    return true;
}

enum class FontSizeMode : u8 {
    Absolute,
    RelativePlus,
    RelativeMinus,
};

// https://w3c.github.io/editing/docs/execCommand/#the-fontsize-command
bool command_font_size_action(DOM::Document& document, String const& value)
{
    // 1. Strip leading and trailing whitespace from value.
    auto resulting_value = MUST(value.trim_ascii_whitespace());

    // 2. If value is not a valid floating point number, and would not be a valid floating point number if a single
    //    leading "+" character were stripped, return false.
    if (!HTML::is_valid_floating_point_number(resulting_value)) {
        if (!resulting_value.starts_with_bytes("+"sv)
            || !HTML::is_valid_floating_point_number(MUST(resulting_value.substring_from_byte_offset(1))))
            return false;
    }

    // 3. If the first character of value is "+", delete the character and let mode be "relative-plus".
    auto mode = FontSizeMode::Absolute;
    if (resulting_value.starts_with_bytes("+"sv)) {
        resulting_value = MUST(resulting_value.substring_from_byte_offset(1));
        mode = FontSizeMode::RelativePlus;
    }

    // 4. Otherwise, if the first character of value is "-", delete the character and let mode be "relative-minus".
    else if (resulting_value.starts_with_bytes("-"sv)) {
        resulting_value = MUST(resulting_value.substring_from_byte_offset(1));
        mode = FontSizeMode::RelativeMinus;
    }

    // 5. Otherwise, let mode be "absolute".
    // NOTE: This is the default set in step 3.

    // 6. Apply the rules for parsing non-negative integers to value, and let number be the result.
    i64 number = HTML::parse_non_negative_integer(resulting_value).release_value();

    // 7. If mode is "relative-plus", add three to number.
    if (mode == FontSizeMode::RelativePlus)
        number += 3;

    // 8. If mode is "relative-minus", negate number, then add three to it.
    if (mode == FontSizeMode::RelativeMinus)
        number = -number + 3;

    // 9. If number is less than one, let number equal 1.
    number = AK::max(number, 1);

    // 10. If number is greater than seven, let number equal 7.
    number = AK::min(number, 7);

    // 11. Set value to the string here corresponding to number:
    // 1: x-small
    // 2: small
    // 3: medium
    // 4: large
    // 5: x-large
    // 6: xx-large
    // 7: xxx-large
    auto const& font_sizes = named_font_sizes();
    resulting_value = MUST(String::from_utf8(font_sizes[number - 1]));

    // 12. Set the selection's value to value.
    set_the_selections_value(document, CommandNames::fontSize, resulting_value);

    // 13. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-fontsize-command
String command_font_size_value(DOM::Document const& document)
{
    // 1. If the active range is null, return the empty string.
    auto range = active_range(document);
    if (!range)
        return {};

    // 2. Let pixel size be the effective command value of the first formattable node that is effectively contained in
    //    the active range, or if there is no such node, the effective command value of the active range's start node,
    //    in either case interpreted as a number of pixels.
    auto first_formattable_node = first_formattable_node_effectively_contained(range);
    auto value = effective_command_value(
        first_formattable_node ? first_formattable_node : *range->start_container(),
        CommandNames::fontSize);
    auto pixel_size = font_size_to_pixel_size(value.value_or({}));

    // 3. Return the legacy font size for pixel size.
    return legacy_font_size(pixel_size.to_int());
}

// https://w3c.github.io/editing/docs/execCommand/#the-forecolor-command
bool command_fore_color_action(DOM::Document& document, String const& value)
{
    // 1. If value is not a valid CSS color, prepend "#" to it.
    auto resulting_value = value;
    if (!Color::from_string(resulting_value).has_value()) {
        resulting_value = MUST(String::formatted("#{}", resulting_value));

        // 2. If value is still not a valid CSS color, or if it is currentColor, return false.
        if (!Color::from_string(resulting_value).has_value()) {
            // FIXME: Also return false in case of currentColor.
            return false;
        }
    }

    // 3. Set the selection's value to value.
    set_the_selections_value(document, CommandNames::foreColor, resulting_value);

    // 4. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-formatblock-command
bool command_format_block_action(DOM::Document& document, String const& value)
{
    // 1. If value begins with a "<" character and ends with a ">" character, remove the first and last characters from
    //    it.
    auto resulting_value = value;
    if (resulting_value.starts_with_bytes("<"sv) && resulting_value.ends_with_bytes(">"sv))
        resulting_value = MUST(resulting_value.substring_from_byte_offset(1, resulting_value.bytes_as_string_view().length() - 2));

    // 2. Let value be converted to ASCII lowercase.
    resulting_value = resulting_value.to_ascii_lowercase();

    // 3. If value is not a formattable block name, return false.
    if (!is_formattable_block_name(resulting_value))
        return false;

    // 4. Block-extend the active range, and let new range be the result.
    auto new_range = block_extend_a_range(*active_range(document));

    // 5. Let node list be an empty list of nodes.
    Vector<GC::Ref<DOM::Node>> node_list;

    // 6. For each node node contained in new range, append node to node list if it is editable, the last member of
    //    original node list (if any) is not an ancestor of node, node is either a non-list single-line container or an
    //    allowed child of "p" or a dd or dt, and node is not the ancestor of a prohibited paragraph child.
    auto is_ancestor_of_prohibited_paragraph_child = [](GC::Ref<DOM::Node> node) {
        bool result = false;
        node->for_each_in_subtree([&result](GC::Ref<DOM::Node> descendant) {
            if (is_prohibited_paragraph_child(descendant)) {
                result = true;
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });
        return result;
    };
    new_range->for_each_contained([&](GC::Ref<DOM::Node> node) {
        if (auto const* element = as_if<DOM::Element>(*node); node->is_editable()
            && (node_list.is_empty() || !node_list.last()->is_ancestor_of(node))
            && (is_non_list_single_line_container(node) || is_allowed_child_of_node(node, HTML::TagNames::p)
                || (element && element->local_name().is_one_of(HTML::TagNames::dd, HTML::TagNames::dt)))
            && !is_ancestor_of_prohibited_paragraph_child(node)) {
            node_list.append(node);
        }
        return IterationDecision::Continue;
    });

    // 7. Record the values of node list, and let values be the result.
    auto values = record_the_values_of_nodes(node_list);

    // 8. For each node in node list, while node is the descendant of an editable HTML element in the same editing host,
    //    whose local name is a formattable block name, and which is not the ancestor of a prohibited paragraph child,
    //    split the parent of the one-node list consisting of node.
    for (auto node : node_list) {
        while (true) {
            bool is_matching_descendant = false;
            node->for_each_ancestor([&](GC::Ref<DOM::Node> ancestor) {
                if (ancestor->is_editable() && is<HTML::HTMLElement>(*ancestor) && is_in_same_editing_host(node, ancestor)
                    && is_formattable_block_name(static_cast<DOM::Element&>(*ancestor).local_name())
                    && !is_ancestor_of_prohibited_paragraph_child(ancestor)) {
                    is_matching_descendant = true;
                    return IterationDecision::Break;
                }
                return IterationDecision::Continue;
            });
            if (!is_matching_descendant)
                break;

            split_the_parent_of_nodes({ node });
        }
    }

    // 9. Restore the values from values.
    restore_the_values_of_nodes(values);

    // 10. While node list is not empty:
    while (!node_list.is_empty()) {
        Vector<GC::Ref<DOM::Node>> sublist;

        // 1. If the first member of node list is a single-line container:
        if (is_single_line_container(node_list.first())) {
            // AD-HOC: The spec makes note of single-line containers without children, and how they should probably
            //         disappear given that Firefox and Opera did this at the time. We're going to follow their lead and
            //         remove the node if it has no children.
            if (!node_list.first()->has_children()) {
                node_list.take_first()->remove();
                continue;
            }

            // 1. Let sublist be the children of the first member of node list.
            node_list.first()->for_each_child([&sublist](GC::Ref<DOM::Node> child) {
                sublist.append(child);
                return IterationDecision::Continue;
            });

            // 2. Record the values of sublist, and let values be the result.
            auto values = record_the_values_of_nodes(sublist);

            // 3. Remove the first member of node list from its parent, preserving its descendants.
            remove_node_preserving_its_descendants(node_list.first());

            // 4. Restore the values from values.
            restore_the_values_of_nodes(values);

            // 5. Remove the first member from node list.
            node_list.take_first();
        }

        // 2. Otherwise:
        else {
            // 1. Let sublist be an empty list of nodes.
            // 2. Remove the first member of node list and append it to sublist.
            sublist.append(node_list.take_first());

            // 3. While node list is not empty, and the first member of node list is the nextSibling of the last member
            //    of sublist, and the first member of node list is not a single-line container, and the last member of
            //    sublist is not a br, remove the first member of node list and append it to sublist.
            while (!node_list.is_empty() && node_list.first().ptr() == sublist.last()->next_sibling()
                && !is_single_line_container(node_list.first()) && !is<HTML::HTMLBRElement>(*sublist.last()))
                sublist.append(node_list.take_first());
        }

        // 3. Wrap sublist. If value is "div" or "p", sibling criteria returns false; otherwise it returns true for an
        //    HTML element with local name value and no attributes, and false otherwise. New parent instructions return
        //    the result of running createElement(value) on the context object. Then fix disallowed ancestors of the
        //    result.
        auto result = wrap(
            sublist,
            [&](GC::Ref<DOM::Node> sibling) {
                if (resulting_value.is_one_of("div"sv, "p"sv))
                    return false;
                auto const* html_element = as_if<HTML::HTMLElement>(*sibling);
                return html_element && html_element->local_name() == resulting_value && !html_element->has_attributes();
            },
            [&] { return MUST(DOM::create_element(document, resulting_value, Namespace::HTML)); });
        if (result)
            fix_disallowed_ancestors_of_node(*result);
    }

    // 11. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-formatblock-command
bool command_format_block_indeterminate(DOM::Document const& document)
{
    // 1. If the active range is null, return the empty string.
    // AD-HOC: We're returning false instead. See https://github.com/w3c/editing/issues/474
    auto range = active_range(document);
    if (!range)
        return false;

    // 2. Block-extend the active range, and let new range be the result.
    auto new_range = block_extend_a_range(*range);

    // 3. Let node list be all visible editable nodes that are contained in new range and have no children.
    Vector<GC::Ref<DOM::Node>> node_list;
    new_range->for_each_contained([&](GC::Ref<DOM::Node> node) {
        if (is_visible_node(node) && node->is_editable() && !node->has_children())
            node_list.append(node);
        return IterationDecision::Continue;
    });

    // 4. If node list is empty, return false.
    if (node_list.is_empty())
        return false;

    // 5. Let type be null.
    Optional<FlyString const&> type;

    // 6. For each node in node list:
    for (auto node : node_list) {
        // 1. While node's parent is editable and in the same editing host as node, and node is not an HTML element
        //    whose local name is a formattable block name, set node to its parent.
        while (node->parent() && node->parent()->is_editable() && is_in_same_editing_host(node, *node->parent())
            && !(is<HTML::HTMLElement>(*node) && is_formattable_block_name(static_cast<DOM::Element&>(*node).local_name())))
            node = *node->parent();

        // 2. Let current type be the empty string.
        FlyString current_type;

        // 3. If node is an editable HTML element whose local name is a formattable block name, and node is not the
        //    ancestor of a prohibited paragraph child, set current type to node's local name.
        if (auto const* html_element = as_if<HTML::HTMLElement>(*node); node->is_editable() && html_element
            && is_formattable_block_name(html_element->local_name()))
            current_type = html_element->local_name();

        // 4. If type is null, set type to current type.
        if (!type.has_value()) {
            type = current_type;
        }

        // 5. Otherwise, if type does not equal current type, return true.
        else if (type.value() != current_type) {
            return true;
        }
    }

    // 7. Return false.
    return false;
}

// https://w3c.github.io/editing/docs/execCommand/#the-formatblock-command
String command_format_block_value(DOM::Document const& document)
{
    // 1. If the active range is null, return the empty string.
    auto range = active_range(document);
    if (!range)
        return {};

    // 2. Block-extend the active range, and let new range be the result.
    auto new_range = block_extend_a_range(*range);

    // 3. Let node be the first visible editable node that is contained in new range and has no children. If there is no
    //    such node, return the empty string.
    GC::Ptr<DOM::Node> node;
    new_range->for_each_contained([&](GC::Ref<DOM::Node> contained_node) {
        if (is_visible_node(contained_node) && contained_node->is_editable() && !contained_node->has_children()) {
            node = contained_node;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (!node)
        return {};

    // 4. While node's parent is editable and in the same editing host as node, and node is not an HTML element whose
    //    local name is a formattable block name, set node to its parent.
    while (node->parent() && node->parent()->is_editable() && is_in_same_editing_host(*node, *node->parent())
        && !(is<HTML::HTMLElement>(*node) && is_formattable_block_name(static_cast<DOM::Element&>(*node).local_name())))
        node = node->parent();

    // 5. If node is an editable HTML element whose local name is a formattable block name, and node is not the ancestor
    //    of a prohibited paragraph child, return node's local name, converted to ASCII lowercase.
    if (auto const* html_element = as_if<HTML::HTMLElement>(*node); node->is_editable() && html_element
        && is_formattable_block_name(html_element->local_name())) {
        bool is_ancestor_of_prohibited_paragraph_child = false;
        node->for_each_in_subtree([&is_ancestor_of_prohibited_paragraph_child](GC::Ref<DOM::Node> descendant) {
            if (is_prohibited_paragraph_child(descendant)) {
                is_ancestor_of_prohibited_paragraph_child = true;
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });
        if (!is_ancestor_of_prohibited_paragraph_child)
            return html_element->local_name().to_string().to_ascii_lowercase();
    }

    // 6. Return the empty string.
    return {};
}

// https://w3c.github.io/editing/docs/execCommand/#the-forwarddelete-command
bool command_forward_delete_action(DOM::Document& document, String const&)
{
    // 1. If the active range is not collapsed, delete the selection and return true.
    auto& selection = *document.get_selection();
    auto& active_range = *selection.range();
    if (!active_range.collapsed()) {
        delete_the_selection(selection);
        return true;
    }

    // 2. Canonicalize whitespace at the active range's start.
    canonicalize_whitespace(active_range.start());

    // 3. Let node and offset be the active range's start node and offset.
    auto node = active_range.start().node;
    auto offset = active_range.start().offset;

    // 4. Repeat the following steps:
    while (true) {
        // 1. If offset is the length of node and node's nextSibling is an editable invisible node, remove node's
        //    nextSibling from its parent.
        if (offset == node->length() && node->next_sibling() && node->next_sibling()->is_editable()
            && is_invisible_node(*node->next_sibling())) {
            node->next_sibling()->remove();
            continue;
        }

        // 2. Otherwise, if node has a child with index offset and that child is an editable invisible node, remove that
        //    child from node.
        auto* child_at_offset = node->child_at_index(offset);
        if (child_at_offset && child_at_offset->is_editable() && is_invisible_node(*child_at_offset)) {
            child_at_offset->remove();
            continue;
        }

        // 3. Otherwise, if offset is the length of node and node is an inline node, or if node is invisible, set offset
        //    to one plus the index of node, then set node to its parent.
        if (node->parent() && ((offset == node->length() && is_inline_node(node)) || is_invisible_node(node))) {
            offset = node->index() + 1;
            node = *node->parent();
            continue;
        }

        // 4. Otherwise, if node has a child with index offset and that child is neither a block node nor a br nor an
        //    img nor a collapsed block prop, set node to that child, then set offset to zero.
        if (child_at_offset && !is_block_node(*child_at_offset) && !is<HTML::HTMLBRElement>(*child_at_offset)
            && !is<HTML::HTMLImageElement>(*child_at_offset) && !is_collapsed_block_prop(*child_at_offset)) {
            node = *child_at_offset;
            offset = 0;
            continue;
        }

        // 5. Otherwise, break from this loop.
        break;
    }

    // 5. If node is a Text node and offset is not node's length:
    if (is<DOM::Text>(*node) && offset != node->length()) {
        // 1. Let end offset be offset plus one.
        auto end_offset = offset + 1;

        // FIXME: 2. While end offset is not node's length and the end offsetth code unit of node's data has general category M
        //    when interpreted as a Unicode code point, add one to end offset.

        // 3. Call collapse(node, offset) on the context object's selection.
        MUST(selection.collapse(node, offset));

        // 4. Call extend(node, end offset) on the context object's selection.
        MUST(selection.extend(node, end_offset));

        // 5. Delete the selection.
        delete_the_selection(selection);

        // 6. Return true.
        return true;
    }

    // 6. If node is an inline node, return true.
    if (is_inline_node(node))
        return true;

    // 7. If node has a child with index offset and that child is a br or hr or img, but is not a collapsed block prop:
    if (auto child_at_offset = node->child_at_index(offset);
        (is<HTML::HTMLBRElement>(child_at_offset) || is<HTML::HTMLHRElement>(child_at_offset) || is<HTML::HTMLImageElement>(child_at_offset))
        && !is_collapsed_block_prop(*child_at_offset)) {
        // 1. Call collapse(node, offset) on the context object's selection.
        MUST(selection.collapse(node, offset));

        // 2. Call extend(node, offset + 1) on the context object's selection.
        MUST(selection.extend(node, offset + 1));

        // 3. Delete the selection.
        delete_the_selection(selection);

        // 4. Return true.
        return true;
    }

    // 8. Let end node equal node and let end offset equal offset.
    auto end_node = node;
    auto end_offset = offset;

    // 9. If end node has a child with index end offset, and that child is a collapsed block prop, add one to end
    //    offset.
    if (auto child_at_offset = end_node->child_at_index(end_offset); child_at_offset && is_collapsed_block_prop(*child_at_offset))
        ++end_offset;

    // 10. Repeat the following steps:
    while (true) {
        // 1. If end offset is the length of end node, set end offset to one plus the index of end node and then set end
        //    node to its parent.
        if (end_node->parent() && end_offset == end_node->length()) {
            end_offset = end_node->index() + 1;
            end_node = *end_node->parent();
            continue;
        }

        // 2. Otherwise, if end node has an editable invisible child with index end offset, remove it from end node.
        if (auto child_at_offset = end_node->child_at_index(end_offset); child_at_offset && child_at_offset->is_editable()
            && is_invisible_node(*child_at_offset)) {
            child_at_offset->remove();
            continue;
        }

        // 3. Otherwise, break from this loop.
        break;
    }

    // 11. If the child of end node with index end offset minus one is a table, return true.
    if (is<HTML::HTMLTableElement>(end_node->child_at_index(end_offset - 1)))
        return true;

    // 12. If the child of end node with index end offset is a table:
    if (is<HTML::HTMLTableElement>(end_node->child_at_index(end_offset))) {
        // 1. Call collapse(end node, end offset) on the context object's selection.
        MUST(selection.collapse(end_node, end_offset));

        // 2. Call extend(end node, end offset + 1) on the context object's selection.
        MUST(selection.extend(end_node, end_offset + 1));

        // 3. Return true.
        return true;
    }

    // 13. If offset is the length of node, and the child of end node with index end offset is an hr or br:
    if (auto child_at_offset = end_node->child_at_index(end_offset); offset == node->length()
        && (is<HTML::HTMLHRElement>(child_at_offset) || is<HTML::HTMLBRElement>(child_at_offset))) {
        // 1. Call collapse(end node, end offset) on the context object's selection.
        MUST(selection.collapse(end_node, end_offset));

        // 2. Call extend(end node, end offset + 1) on the context object's selection.
        MUST(selection.extend(end_node, end_offset + 1));

        // 3. Delete the selection.
        delete_the_selection(selection);

        // 4. Call collapse(node, offset) on the selection.
        MUST(selection.collapse(node, offset));

        // 5. Return true.
        return true;
    }

    // 14. While end node has a child with index end offset:
    while (auto child_at_offset = end_node->child_at_index(end_offset)) {
        // 1. If end node's child with index end offset is editable and invisible, remove it from end node.
        if (child_at_offset->is_editable() && is_invisible_node(*child_at_offset)) {
            child_at_offset->remove();
        }

        // 2. Otherwise, set end node to its child with index end offset and set end offset to zero.
        else {
            end_node = *child_at_offset;
            end_offset = 0;
        }
    }

    // 15. Call collapse(node, offset) on the context object's selection.
    MUST(selection.collapse(node, offset));

    // 16. Call extend(end node, end offset) on the context object's selection.
    MUST(selection.extend(end_node, end_offset));

    // 17. Delete the selection.
    delete_the_selection(selection);

    // AD-HOC: Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-indent-command
bool command_indent_action(DOM::Document& document, String const&)
{
    // 1. Let items be a list of all lis that are inclusive ancestors of the active range's start and/or end node.
    Vector<GC::Ref<DOM::Node>> items;
    auto add_all_lis = [&items](GC::Ref<DOM::Node> node) {
        node->for_each_inclusive_ancestor([&items](GC::Ref<DOM::Node> ancestor) {
            if (is<HTML::HTMLLIElement>(*ancestor) && !items.contains_slow(ancestor))
                items.append(ancestor);
            return IterationDecision::Continue;
        });
    };
    auto range = active_range(document);
    add_all_lis(range->start_container());
    add_all_lis(range->end_container());

    // 2. For each item in items, normalize sublists of item.
    for (auto item : items)
        normalize_sublists_in_node(item);

    // 3. Block-extend the active range, and let new range be the result.
    auto new_range = block_extend_a_range(*active_range(document));

    // 4. Let node list be a list of nodes, initially empty.
    Vector<GC::Ref<DOM::Node>> node_list;

    // 5. For each node node contained in new range, if node is editable and is an allowed child of "div" or "ol" and if
    //    the last member of node list (if any) is not an ancestor of node, append node to node list.
    new_range->for_each_contained([&](GC::Ref<DOM::Node> node) {
        if (node->is_editable()
            && (is_allowed_child_of_node(node, HTML::TagNames::div) || is_allowed_child_of_node(node, HTML::TagNames::ol))
            && (node_list.is_empty() || !node_list.last()->is_ancestor_of(node)))
            node_list.append(node);
        return IterationDecision::Continue;
    });

    // 6. If the first visible member of node list is an li whose parent is an ol or ul:
    auto first_visible = node_list.first_matching([](GC::Ref<DOM::Node> node) { return is_visible_node(node); });
    if (first_visible.has_value() && is<HTML::HTMLLIElement>(*first_visible.value()) && first_visible.value()->parent()) {
        GC::Ref<DOM::Node> parent = *first_visible.value()->parent();
        if (is<HTML::HTMLOListElement>(*parent) || is<HTML::HTMLUListElement>(*parent)) {
            // 1. Let sibling be node list's first visible member's previousSibling.
            GC::Ptr<DOM::Node> sibling = first_visible.value()->previous_sibling();

            // 2. While sibling is invisible, set sibling to its previousSibling.
            while (sibling && is_invisible_node(*sibling))
                sibling = sibling->previous_sibling();

            // 3. If sibling is an li, normalize sublists of sibling.
            if (sibling && is<HTML::HTMLLIElement>(*sibling))
                normalize_sublists_in_node(*sibling);
        }
    }

    // 7. While node list is not empty:
    while (!node_list.is_empty()) {
        // 1. Let sublist be a list of nodes, initially empty.
        Vector<GC::Ref<DOM::Node>> sublist;

        // 2. Remove the first member of node list and append it to sublist.
        sublist.append(node_list.take_first());

        // 3. While the first member of node list is the nextSibling of the last member of sublist, remove the first
        //    member of node list and append it to sublist.
        while (!node_list.is_empty() && node_list.first().ptr() == sublist.last()->next_sibling())
            sublist.append(node_list.take_first());

        // 4. Indent sublist.
        indent(sublist);
    }

    // AD-HOC: Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-inserthorizontalrule-command
bool command_insert_horizontal_rule_action(DOM::Document& document, String const&)
{
    // 1. Let start node, start offset, end node, and end offset be the active range's start and end nodes and offsets.
    auto range = active_range(document);
    auto start = range->start();
    auto end = range->end();

    // 2. While start offset is 0 and start node's parent is not null, set start offset to start node's index, then set
    //    start node to its parent.
    while (start.offset == 0 && start.node->parent()) {
        start.offset = start.node->index();
        start.node = *start.node->parent();
    }

    // 3. While end offset is end node's length, and end node's parent is not null, set end offset to one plus end
    //    node's index, then set end node to its parent.
    while (end.offset == end.node->length() && end.node->parent()) {
        end.offset = end.node->index() + 1;
        end.node = *end.node->parent();
    }

    // 4. Call collapse(start node, start offset) on the context object's selection.
    auto& selection = *document.get_selection();
    MUST(selection.collapse(start.node, start.offset));

    // 5. Call extend(end node, end offset) on the context object's selection.
    MUST(selection.extend(end.node, end.offset));

    // 6. Delete the selection, with block merging false.
    delete_the_selection(selection, false);

    // 7. If the active range's start node is neither editable nor an editing host, return true.
    range = active_range(document);
    start = range->start();
    if (!start.node->is_editable_or_editing_host())
        return true;

    // 8. If the active range's start node is a Text node and its start offset is zero, call collapse() on the context
    //    object's selection, with first argument the active range's start node's parent and second argument the active
    //    range's start node's index.
    if (is<DOM::Text>(*start.node) && start.offset == 0)
        MUST(selection.collapse(start.node->parent(), start.node->index()));

    // 9. If the active range's start node is a Text node and its start offset is the length of its start node, call
    //    collapse() on the context object's selection, with first argument the active range's start node's parent, and
    //    the second argument one plus the active range's start node's index.
    range = active_range(document);
    start = range->start();
    if (is<DOM::Text>(*start.node) && start.offset == start.node->length())
        MUST(selection.collapse(start.node->parent(), start.node->index() + 1));

    // 10. Let hr be the result of calling createElement("hr") on the context object.
    auto hr = MUST(DOM::create_element(document, HTML::TagNames::hr, Namespace::HTML));

    // 11. Run insertNode(hr) on the active range.
    MUST(active_range(document)->insert_node(hr));

    // 12. Fix disallowed ancestors of hr.
    fix_disallowed_ancestors_of_node(hr);

    // 13. Run collapse() on the context object's selection, with first argument hr's parent and the second argument
    //     equal to one plus hr's index.
    MUST(selection.collapse(hr->parent(), hr->index() + 1));

    // 14. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-inserthtml-command
bool command_insert_html_action(DOM::Document& document, String const& value)
{
    // FIXME: 1. Set value to the result of invoking get trusted types compliant string with TrustedHTML, this's relevant
    //    global object, value, "Document execCommand", and "script".
    auto resulting_value = value;

    // 2. Delete the selection.
    auto& selection = *document.get_selection();
    delete_the_selection(selection);

    // 3. If the active range's start node is neither editable nor an editing host, return true.
    auto range = active_range(document);
    if (!range->start_container()->is_editable_or_editing_host())
        return true;

    // 4. Let frag be the result of calling createContextualFragment(value) on the active range.
    auto frag = MUST(range->create_contextual_fragment(resulting_value));

    // 5. Let last child be the lastChild of frag.
    GC::Ptr<DOM::Node> last_child = frag->last_child();

    // 6. If last child is null, return true.
    if (!last_child)
        return true;

    // 7. Let descendants be all descendants of frag.
    Vector<GC::Ref<DOM::Node>> descendants;
    frag->for_each_in_subtree([&descendants](GC::Ref<DOM::Node> descendant) {
        descendants.append(descendant);
        return TraversalDecision::Continue;
    });

    // 8. If the active range's start node is a block node:
    if (is_block_node(range->start_container())) {
        // 1. Let collapsed block props be all editable collapsed block prop children of the active range's start node
        //    that have index greater than or equal to the active range's start offset.
        Vector<GC::Ref<DOM::Node>> collapsed_block_props;
        range->start_container()->for_each_child([&](GC::Ref<DOM::Node> child) {
            if (child->is_editable() && is_collapsed_block_prop(child) && child->index() >= range->start_offset())
                collapsed_block_props.append(child);
            return IterationDecision::Continue;
        });

        // 2. For each node in collapsed block props, remove node from its parent.
        for (auto node : collapsed_block_props)
            node->remove();
    }

    // 9. Call insertNode(frag) on the active range.
    MUST(range->insert_node(frag));

    // 10. If the active range's start node is a block node with no visible children, call createElement("br") on the
    //     context object and append the result as the last child of the active range's start node.
    range = active_range(document);
    if (is_block_node(range->start_container()) && !has_visible_children(range->start_container())) {
        auto br = MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML));
        MUST(range->start_container()->append_child(br));
    }

    // 11. Call collapse() on the context object's selection, with last child's parent as the first argument and one
    //     plus its index as the second.
    MUST(selection.collapse(last_child->parent(), last_child->index() + 1));

    // 12. Fix disallowed ancestors of each member of descendants.
    for (auto member : descendants)
        fix_disallowed_ancestors_of_node(member);

    // 13. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-insertimage-command
bool command_insert_image_action(DOM::Document& document, String const& value)
{
    // 1. If value is the empty string, return false.
    if (value.is_empty())
        return false;

    // 2. Delete the selection, with strip wrappers false.
    auto& selection = *document.get_selection();
    delete_the_selection(selection, true, false);

    // 3. Let range be the active range.
    auto range = active_range(document);

    // 4. If the active range's start node is neither editable nor an editing host, return true.
    if (!range->start_container()->is_editable_or_editing_host())
        return true;

    // 5. If range's start node is a block node whose sole child is a br, and its start offset is 0, remove its start
    //    node's child from it.
    auto start_node = range->start_container();
    if (is_block_node(start_node) && start_node->child_count() == 1 && is<HTML::HTMLBRElement>(*start_node->first_child())
        && range->start_offset() == 0)
        start_node->first_child()->remove();

    // 6. Let img be the result of calling createElement("img") on the context object.
    auto img = MUST(DOM::create_element(document, HTML::TagNames::img, Namespace::HTML));

    // 7. Run setAttribute("src", value) on img.
    MUST(img->set_attribute(HTML::AttributeNames::src, value));

    // 8. Run insertNode(img) on range.
    MUST(range->insert_node(img));

    // 9. Let selection be the result of calling getSelection() on the context object.
    // NOTE: Already done so in step 2.

    // 10. Run collapse() on selection, with first argument equal to the parent of img and the second argument equal to
    //     one plus the index of img.
    MUST(selection.collapse(img->parent(), img->index() + 1));

    // 11. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-insertlinebreak-command
bool command_insert_linebreak_action(DOM::Document& document, String const&)
{
    // 1. Delete the selection, with strip wrappers false.
    auto& selection = *document.get_selection();
    delete_the_selection(selection, true, false);

    // 2. If the active range's start node is neither editable nor an editing host, return true.
    auto& active_range = *selection.range();
    auto start_node = active_range.start_container();
    if (!start_node->is_editable_or_editing_host())
        return true;

    // 3. If the active range's start node is an Element, and "br" is not an allowed child of it, return true.
    if (is<DOM::Element>(*start_node) && !is_allowed_child_of_node(HTML::TagNames::br, start_node))
        return true;

    // 4. If the active range's start node is not an Element, and "br" is not an allowed child of the active range's
    //    start node's parent, return true.
    if (!is<DOM::Element>(*start_node) && start_node->parent() && !is_allowed_child_of_node(HTML::TagNames::br, GC::Ref { *start_node->parent() }))
        return true;

    // 5. If the active range's start node is a Text node and its start offset is zero, call collapse() on the context
    //    object's selection, with first argument equal to the active range's start node's parent and second argument
    //    equal to the active range's start node's index.
    if (is<DOM::Text>(*start_node) && active_range.start_offset() == 0)
        MUST(selection.collapse(start_node->parent(), start_node->index()));

    // 6. If the active range's start node is a Text node and its start offset is the length of its start node, call
    //    collapse() on the context object's selection, with first argument equal to the active range's start node's
    //    parent and second argument equal to one plus the active range's start node's index.
    if (is<DOM::Text>(*start_node) && active_range.start_offset() == start_node->length())
        MUST(selection.collapse(start_node->parent(), start_node->index() + 1));

    // AD-HOC: If the active range's start node is a Text node and its resolved value for "white-space" is one of "pre",
    //         "pre-line" or "pre-wrap":
    //         * Insert a newline (\n) character at the active range's start offset;
    //         * Collapse the selection with active range's start node as the first argument and one plus active range's
    //           start offset as the second argument
    //         * Insert another newline (\n) character if the active range's start offset is equal to the length of the
    //           active range's start node.
    //         * Return true.
    if (auto* text_node = as_if<DOM::Text>(*start_node); text_node) {
        auto resolved_white_space = resolved_keyword(*start_node, CSS::PropertyID::WhiteSpace);
        if (resolved_white_space.has_value()
            && first_is_one_of(resolved_white_space.value(), CSS::Keyword::Pre, CSS::Keyword::PreLine, CSS::Keyword::PreWrap)) {
            MUST(text_node->insert_data(active_range.start_offset(), "\n"_string));
            MUST(selection.collapse(start_node, active_range.start_offset() + 1));
            if (selection.range()->start_offset() == start_node->length())
                MUST(text_node->insert_data(active_range.start_offset(), "\n"_string));
            return true;
        }
    }

    // 7. Let br be the result of calling createElement("br") on the context object.
    auto br = MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML));

    // 8. Call insertNode(br) on the active range.
    MUST(active_range.insert_node(br));

    // 9. Call collapse() on the context object's selection, with br's parent as the first argument and one plus br's
    //    index as the second argument.
    MUST(selection.collapse(br->parent(), br->index() + 1));

    // 10. If br is a collapsed line break, call createElement("br") on the context object and let extra br be the
    //     result, then call insertNode(extra br) on the active range.
    if (is_collapsed_line_break(br)) {
        auto extra_br = MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML));
        MUST(active_range.insert_node(extra_br));
    }

    // 11. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-insertorderedlist-command
bool command_insert_ordered_list_action(DOM::Document& document, String const&)
{
    // Toggle lists with tag name "ol", then return true.
    toggle_lists(document, HTML::TagNames::ol);
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-insertorderedlist-command
bool command_insert_ordered_list_indeterminate(DOM::Document const& document)
{
    // True if the selection's list state is "mixed" or "mixed ol", false otherwise.
    return first_is_one_of(selections_list_state(document), SelectionsListState::Mixed, SelectionsListState::MixedOl);
}

// https://w3c.github.io/editing/docs/execCommand/#the-insertorderedlist-command
bool command_insert_ordered_list_state(DOM::Document const& document)
{
    // True if the selection's list state is "ol", false otherwise.
    return selections_list_state(document) == SelectionsListState::Ol;
}

// https://w3c.github.io/editing/docs/execCommand/#the-insertparagraph-command
bool command_insert_paragraph_action(DOM::Document& document, String const&)
{
    // 1. Delete the selection.
    auto& selection = *document.get_selection();
    delete_the_selection(selection);

    // 2. If the active range's start node is neither editable nor an editing host, return true.
    GC::Ref<DOM::Range> active_range = *selection.range();
    GC::Ptr<DOM::Node> node = active_range->start_container();
    if (!node->is_editable_or_editing_host())
        return true;

    // 3. Let node and offset be the active range's start node and offset.
    // NOTE: node is set in step 2
    auto offset = active_range->start_offset();

    // 4. If node is a Text node, and offset is neither 0 nor the length of node, call splitText(offset) on node.
    if (is<DOM::Text>(*node) && offset != 0 && offset != node->length())
        MUST(static_cast<DOM::Text&>(*node).split_text(offset));

    // 5. If node is a Text node and offset is its length, set offset to one plus the index of node, then set node to
    //    its parent.
    if (is<DOM::Text>(*node) && offset == node->length()) {
        offset = node->index() + 1;
        node = node->parent();
    }

    // 6. If node is a Text or Comment node, set offset to the index of node, then set node to its parent.
    if (is<DOM::Text>(*node) || is<DOM::Comment>(*node)) {
        offset = node->index();
        node = node->parent();
    }

    // 7. Call collapse(node, offset) on the context object's selection.
    MUST(selection.collapse(node, offset));
    active_range = *selection.range();

    // 8. Let container equal node.
    auto container = node;

    // 9. While container is not a single-line container, and container's parent is editable and in the same editing
    //    host as node, set container to its parent.
    while (!is_single_line_container(*container)) {
        auto container_parent = container->parent();
        if (!container_parent->is_editable() || !is_in_same_editing_host(*node, *container_parent))
            break;
        container = container_parent;
    }

    // 10. If container is an editable single-line container in the same editing host as node, and its local name is "p"
    //     or "div":
    if (container->is_editable() && is_single_line_container(*container) && is_in_same_editing_host(*container, *node)
        && is<DOM::Element>(*container)
        && static_cast<DOM::Element&>(*container).local_name().is_one_of(HTML::TagNames::p, HTML::TagNames::div)) {
        // 1. Let outer container equal container.
        auto outer_container = container;

        // 2. While outer container is not a dd or dt or li, and outer container's parent is editable, set outer
        //    container to its parent.
        auto is_li_dt_or_dd = [](DOM::Element const& node) {
            return node.local_name().is_one_of(HTML::TagNames::li, HTML::TagNames::dt, HTML::TagNames::dd);
        };
        while (!is<DOM::Element>(*outer_container) || !is_li_dt_or_dd(static_cast<DOM::Element&>(*outer_container))) {
            auto outer_container_parent = outer_container->parent();
            if (!outer_container_parent->is_editable())
                break;
            outer_container = outer_container_parent;
        }

        // 3. If outer container is a dd or dt or li, set container to outer container.
        if (is<DOM::Element>(*outer_container) && is_li_dt_or_dd(static_cast<DOM::Element&>(*outer_container)))
            container = outer_container;
    }

    // 11. If container is not editable or not in the same editing host as node or is not a single-line container:
    if (!container->is_editable() || !is_in_same_editing_host(*container, *node) || !is_single_line_container(*container)) {
        // 1. Let tag be the default single-line container name.
        auto tag = document.default_single_line_container_name();

        // 2. Block-extend the active range, and let new range be the result.
        auto new_range = block_extend_a_range(active_range);

        // 3. Let node list be a list of nodes, initially empty.
        Vector<GC::Ref<DOM::Node>> node_list;

        // 4. Append to node list the first node in tree order that is contained in new range and is an allowed child of
        //    "p", if any.
        new_range->for_each_contained([&node_list](GC::Ref<DOM::Node> node) {
            if (is_allowed_child_of_node(node, HTML::TagNames::p)) {
                node_list.append(node);
                return IterationDecision::Break;
            }
            return IterationDecision::Continue;
        });

        // 5. If node list is empty:
        if (node_list.is_empty()) {
            // 1. If tag is not an allowed child of the active range's start node, return true.
            if (!is_allowed_child_of_node(tag, active_range->start_container()))
                return true;

            // 2. Set container to the result of calling createElement(tag) on the context object.
            container = MUST(DOM::create_element(document, tag, Namespace::HTML));

            // 3. Call insertNode(container) on the active range.
            MUST(active_range->insert_node(*container));

            // 4. Call createElement("br") on the context object, and append the result as the last child of container.
            MUST(container->append_child(MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML))));

            // 5. Call collapse(container, 0) on the context object's selection.
            MUST(selection.collapse(container, 0));

            // 6. Return true.
            return true;
        }

        // 6. While the nextSibling of the last member of node list is not null and is an allowed child of "p", append
        //    it to node list.
        auto next_sibling = node_list.last()->next_sibling();
        while (next_sibling && is_allowed_child_of_node(GC::Ref { *next_sibling }, HTML::TagNames::p)) {
            node_list.append(*next_sibling);
            next_sibling = next_sibling->next_sibling();
        }

        // 7. Wrap node list, with sibling criteria returning false and new parent instructions returning the result of
        //    calling createElement(tag) on the context object. Set container to the result.
        wrap(
            node_list,
            [](auto) { return false; },
            [&] { return MUST(DOM::create_element(document, tag, Namespace::HTML)); });
    }

    // 12. If container's local name is "address", "listing", or "pre":
    if (is<DOM::Element>(*container)
        && static_cast<DOM::Element&>(*container)
            .local_name()
            .is_one_of(HTML::TagNames::address, HTML::TagNames::listing, HTML::TagNames::pre)) {
        // 1. Let br be the result of calling createElement("br") on the context object.
        auto br = MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML));

        // 2. Call insertNode(br) on the active range.
        MUST(active_range->insert_node(br));

        // 3. Call collapse(node, offset + 1) on the context object's selection.
        MUST(selection.collapse(node, offset + 1));
        active_range = *selection.range();

        // 4. If br is the last descendant of container, let br be the result of calling createElement("br") on the
        //    context object, then call insertNode(br) on the active range.
        GC::Ptr<DOM::Node> last_descendant = container->last_child();
        while (last_descendant->has_children())
            last_descendant = last_descendant->last_child();
        if (br == last_descendant) {
            br = MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML));
            MUST(active_range->insert_node(br));
        }

        // 5. Return true.
        return true;
    }

    // 13. If container's local name is "li", "dt", or "dd"; and either it has no children or it has a single child and
    //     that child is a br:
    if (is<DOM::Element>(*container)
        && static_cast<DOM::Element&>(*container).local_name().is_one_of(HTML::TagNames::li, HTML::TagNames::dt, HTML::TagNames::dd)
        && (!container->has_children() || (container->child_count() == 1 && is<HTML::HTMLBRElement>(container->first_child())))) {
        // 1. Split the parent of the one-node list consisting of container.
        split_the_parent_of_nodes({ *container });

        // 2. If container has no children, call createElement("br") on the context object and append the result as the
        //    last child of container.
        if (!container->has_children())
            MUST(container->append_child(MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML))));

        // 3. If container is a dd or dt, and it is not an allowed child of any of its ancestors in the same editing
        //    host, set the tag name of container to the default single-line container name and let container be the
        //    result.
        if (static_cast<DOM::Element&>(*container).local_name().is_one_of(HTML::TagNames::dd, HTML::TagNames::dt)) {
            bool allowed_child_of_any_ancestor = false;
            container->for_each_ancestor([&](GC::Ref<DOM::Node> ancestor) {
                if (is_allowed_child_of_node(GC::Ref { *container }, ancestor)
                    && is_in_same_editing_host(*container, ancestor)) {
                    allowed_child_of_any_ancestor = true;
                    return IterationDecision::Break;
                }
                return IterationDecision::Continue;
            });
            if (!allowed_child_of_any_ancestor)
                container = set_the_tag_name(static_cast<DOM::Element&>(*container), document.default_single_line_container_name());
        }

        // 4. Fix disallowed ancestors of container.
        fix_disallowed_ancestors_of_node(*container);

        // 5. Return true.
        return true;
    }

    // 14. Let new line range be a new range whose start is the same as the active range's, and whose end is (container,
    //     length of container).
    auto new_line_range = DOM::Range::create(active_range->start_container(), active_range->start_offset(), *container, container->length());

    // 15. While new line range's start offset is zero and its start node is not a prohibited paragraph child, set its
    //     start to (parent of start node, index of start node).
    GC::Ptr<DOM::Node> start_container = new_line_range->start_container();
    while (start_container->parent() && new_line_range->start_offset() == 0 && !is_prohibited_paragraph_child(*start_container)) {
        MUST(new_line_range->set_start(*start_container->parent(), start_container->index()));
        start_container = start_container->parent();
    }

    // 16. While new line range's start offset is the length of its start node and its start node is not a prohibited
    //     paragraph child, set its start to (parent of start node, 1 + index of start node).
    start_container = new_line_range->start_container();
    while (start_container->parent() && new_line_range->start_offset() == start_container->length()
        && !is_prohibited_paragraph_child(*start_container)) {
        MUST(new_line_range->set_start(*start_container->parent(), start_container->index() + 1));
        start_container = start_container->parent();
    }

    // 17. Let end of line be true if new line range contains either nothing or a single br, and false otherwise.
    auto end_of_line = new_line_range->collapsed()
        || ((new_line_range->start_container() == new_line_range->end_container() && new_line_range->start_offset() == new_line_range->end_offset() - 1)
            && is<HTML::HTMLBRElement>(*new_line_range->start_container()));

    auto& container_element = as<DOM::Element>(*container);
    auto new_container_name = [&] -> FlyString {
        // 18. If the local name of container is "h1", "h2", "h3", "h4", "h5", or "h6", and end of line is true, let new
        //     container name be the default single-line container name.
        if (end_of_line && is_heading(container_element.local_name()))
            return document.default_single_line_container_name();

        // 19. Otherwise, if the local name of container is "dt" and end of line is true, let new container name be "dd".
        if (container_element.local_name() == HTML::TagNames::dt && end_of_line)
            return HTML::TagNames::dd;

        // 20. Otherwise, if the local name of container is "dd" and end of line is true, let new container name be "dt".
        if (container_element.local_name() == HTML::TagNames::dd && end_of_line)
            return HTML::TagNames::dt;

        // 21. Otherwise, let new container name be the local name of container.
        return container_element.local_name();
    }();

    // 22. Let new container be the result of calling createElement(new container name) on the context object.
    auto new_container = MUST(DOM::create_element(document, new_container_name, Namespace::HTML));

    // 23. Copy all attributes of container to new container.
    container_element.for_each_attribute([&new_container](FlyString const& name, String const& value) {
        MUST(new_container->set_attribute(name, value));
    });

    // 24. If new container has an id attribute, unset it.
    if (new_container->has_attribute(HTML::AttributeNames::id))
        new_container->remove_attribute(HTML::AttributeNames::id);

    // 25. Insert new container into the parent of container immediately after container.
    container->parent()->insert_before(*new_container, container->next_sibling());

    // 26. Let contained nodes be all nodes contained in new line range.
    Vector<GC::Ref<DOM::Node>> contained_nodes;
    new_line_range->for_each_contained([&contained_nodes](GC::Ref<DOM::Node> node) {
        contained_nodes.append(node);
        return IterationDecision::Continue;
    });

    // 27. Let frag be the result of calling extractContents() on new line range.
    auto frag = MUST(new_line_range->extract_contents());

    // 28. Unset the id attribute (if any) of each Element descendant of frag that is not in contained nodes.
    frag->for_each_in_subtree_of_type<DOM::Element>([&contained_nodes](GC::Ref<DOM::Element> descendant) {
        if (!contained_nodes.contains_slow(descendant))
            descendant->remove_attribute(HTML::AttributeNames::id);
        return TraversalDecision::Continue;
    });

    // 29. Call appendChild(frag) on new container.
    MUST(new_container->append_child(frag));

    // 30. While container's lastChild is a prohibited paragraph child, set container to its lastChild.
    while (container->last_child() && is_prohibited_paragraph_child(*container->last_child()))
        container = container->last_child();

    // 31. While new container's lastChild is a prohibited paragraph child, set new container to its lastChild.
    while (new_container->last_child() && is_prohibited_paragraph_child(*new_container->last_child())) {
        // NOTE: is_prohibited_paragraph_child() ensures that last_child() is an HTML::HTMLElement
        new_container = static_cast<HTML::HTMLElement&>(*new_container->last_child());
    }

    // 32. If container has no visible children, call createElement("br") on the context object, and append the result
    //     as the last child of container.
    if (!has_visible_children(*container))
        MUST(container->append_child(MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML))));

    // 33. If new container has no visible children, call createElement("br") on the context object, and append the
    //     result as the last child of new container.
    if (!has_visible_children(*new_container))
        MUST(new_container->append_child(MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML))));

    // 34. Call collapse(new container, 0) on the context object's selection.
    MUST(document.get_selection()->collapse(new_container, 0));

    // 35. Return true
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-inserttext-command
bool command_insert_text_action(DOM::Document& document, String const& value)
{
    // 1. Delete the selection, with strip wrappers false.
    auto& selection = *document.get_selection();
    delete_the_selection(selection, true, false);

    // 2. If the active range's start node is neither editable nor an editing host, return true.
    auto range = active_range(document);
    if (!range->start_container()->is_editable_or_editing_host())
        return true;

    // 3. If value's length is greater than one:
    if (value.code_points().length() > 1) {
        // 1. For each code unit el in value, take the action for the insertText command, with value equal to el.
        for (auto el : value.code_points())
            command_insert_text_action(document, String::from_code_point(el));

        // 2. Return true.
        return true;
    }

    // 4. If value is the empty string, return true.
    if (value.is_empty())
        return true;

    // 5. If value is a newline (U+000A), take the action for the insertParagraph command and return true.
    if (value == "\n"sv) {
        command_insert_paragraph_action(document, {});
        return true;
    }

    // 6. Let node and offset be the active range's start node and offset.
    auto node = range->start_container();
    auto offset = range->start_offset();

    // 7. If node has a child whose index is offset − 1, and that child is a Text node, set node to that child, then set
    //    offset to node's length.
    if (is<DOM::Text>(node->child_at_index(offset - 1))) {
        node = *node->child_at_index(offset - 1);
        offset = node->length();
    }

    // 8. If node has a child whose index is offset, and that child is a Text node, set node to that child, then set
    //    offset to zero.
    if (is<DOM::Text>(node->child_at_index(offset))) {
        node = *node->child_at_index(offset);
        offset = 0;
    }

    // 9. Record current overrides, and let overrides be the result.
    auto overrides = record_current_overrides(document);

    // 10. Call collapse(node, offset) on the context object's selection.
    MUST(selection.collapse(node, offset));

    // 11. Canonicalize whitespace at (node, offset).
    canonicalize_whitespace({ node, offset });

    // 12. Let (node, offset) be the active range's start.
    range = *active_range(document);
    node = range->start_container();
    offset = range->start_offset();

    // 13. If node is a Text node:
    if (is<DOM::Text>(*node)) {
        // 1. Call insertData(offset, value) on node.
        MUST(static_cast<DOM::Text&>(*node).insert_data(offset, value));

        // 2. Call collapse(node, offset) on the context object's selection.
        MUST(selection.collapse(node, offset));

        // 3. Call extend(node, offset + 1) on the context object's selection.
        MUST(selection.extend(node, offset + 1));
    }

    // 14. Otherwise:
    else {
        // 1. If node has only one child, which is a collapsed line break, remove its child from it.
        if (node->child_count() == 1 && is_collapsed_line_break(*node->first_child()))
            node->first_child()->remove();

        // 2. Let text be the result of calling createTextNode(value) on the context object.
        auto text = document.create_text_node(value);

        // 3. Call insertNode(text) on the active range.
        MUST(active_range(document)->insert_node(text));

        // 4. Call collapse(text, 0) on the context object's selection.
        MUST(selection.collapse(text, 0));

        // 5. Call extend(text, 1) on the context object's selection.
        MUST(selection.extend(text, 1));
    }

    // 15. Restore states and values from overrides.
    restore_states_and_values(document, overrides);

    // 16. Canonicalize whitespace at the active range's start, with fix collapsed space false.
    canonicalize_whitespace(active_range(document)->start(), false);

    // 17. Canonicalize whitespace at the active range's end, with fix collapsed space false.
    canonicalize_whitespace(active_range(document)->end(), false);

    // 18. If value is a space character, autolink the active range's start.
    if (value == " "sv)
        autolink(active_range(document)->start());

    // 19. Call collapseToEnd() on the context object's selection.
    MUST(selection.collapse_to_end());

    // 20. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-insertunorderedlist-command
bool command_insert_unordered_list_action(DOM::Document& document, String const&)
{
    // Toggle lists with tag name "ul", then return true.
    toggle_lists(document, HTML::TagNames::ul);
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-insertunorderedlist-command
bool command_insert_unordered_list_indeterminate(DOM::Document const& document)
{
    // True if the selection's list state is "mixed" or "mixed ul", false otherwise.
    return first_is_one_of(selections_list_state(document), SelectionsListState::Mixed, SelectionsListState::MixedUl);
}

// https://w3c.github.io/editing/docs/execCommand/#the-insertunorderedlist-command
bool command_insert_unordered_list_state(DOM::Document const& document)
{
    // True if the selection's list state is "ul", false otherwise.
    return selections_list_state(document) == SelectionsListState::Ul;
}

// https://w3c.github.io/editing/docs/execCommand/#the-italic-command
bool command_italic_action(DOM::Document& document, String const&)
{
    // If queryCommandState("italic") returns true, set the selection's value to "normal".
    if (MUST(document.query_command_state(CommandNames::italic))) {
        set_the_selections_value(document, CommandNames::italic, "normal"_string);
    }

    // Otherwise set the selection's value to "italic".
    else {
        set_the_selections_value(document, CommandNames::italic, "italic"_string);
    }

    // Either way, return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifycenter-command
static bool justify_indeterminate(DOM::Document const& document, JustifyAlignment alignment)
{
    // NOTE: This definition is taken from the "justifyCenter" spec and was made generic.

    // Return false if the active range is null. Otherwise, block-extend the active range.
    auto range = active_range(document);
    if (!range)
        return false;
    range = block_extend_a_range(*range);

    // Return true if among visible editable nodes that are contained in the result and have no children, at least one
    // has alignment value "[alignment]" and at least one does not. Otherwise return false.
    Vector<GC::Ref<DOM::Node>> matching_nodes;
    range->for_each_contained([&matching_nodes](GC::Ref<DOM::Node> node) {
        if (is_visible_node(node) && node->is_editable() && !node->has_children())
            matching_nodes.append(node);
        return IterationDecision::Continue;
    });
    return any_of(matching_nodes, [&](GC::Ref<DOM::Node> node) {
        return alignment_value_of_node(node) == alignment;
    }) && any_of(matching_nodes, [&](GC::Ref<DOM::Node> node) {
        return alignment_value_of_node(node) != alignment;
    });
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifycenter-command
static bool justify_state(DOM::Document const& document, JustifyAlignment alignment)
{
    // NOTE: This definition is taken from the "justifyCenter" spec and was made generic.

    // Return false if the active range is null. Otherwise, block-extend the active range.
    auto range = active_range(document);
    if (!range)
        return false;
    range = block_extend_a_range(*range);

    // Return true if there is at least one visible editable node that is contained in the result and has no children,
    // and all such nodes have alignment value "[alignment]". Otherwise return false.
    Vector<GC::Ref<DOM::Node>> matching_nodes;
    range->for_each_contained([&matching_nodes](GC::Ref<DOM::Node> node) {
        if (is_visible_node(node) && node->is_editable() && !node->has_children())
            matching_nodes.append(node);
        return IterationDecision::Continue;
    });
    return !matching_nodes.is_empty() && all_of(matching_nodes, [&](GC::Ref<DOM::Node> node) {
        return alignment_value_of_node(node) == alignment;
    });
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifycenter-command
static String justify_value(DOM::Document const& document)
{
    // NOTE: This definition is taken from the "justifyCenter" spec and was made generic.

    // Return the empty string if the active range is null. Otherwise, block-extend the active range,
    auto range = active_range(document);
    if (!range)
        return {};
    range = block_extend_a_range(*range);

    // and return the alignment value of the first visible editable node that is contained in the result and has no
    // children.
    GC::Ptr<DOM::Node> first_match;
    range->for_each_contained([&first_match](GC::Ref<DOM::Node> node) {
        if (is_visible_node(node) && node->is_editable() && !node->has_children()) {
            first_match = node;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (first_match)
        return justify_alignment_to_string(alignment_value_of_node(first_match));

    // If there is no such node, return "left".
    return justify_alignment_to_string(JustifyAlignment::Left);
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifycenter-command
bool command_justify_center_action(DOM::Document& document, String const&)
{
    // Justify the selection with alignment "center", then return true.
    justify_the_selection(document, JustifyAlignment::Center);
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifycenter-command
bool command_justify_center_indeterminate(DOM::Document const& document)
{
    return justify_indeterminate(document, JustifyAlignment::Center);
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifycenter-command
bool command_justify_center_state(DOM::Document const& document)
{
    return justify_state(document, JustifyAlignment::Center);
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifycenter-command
String command_justify_center_value(DOM::Document const& document)
{
    return justify_value(document);
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifyfull-command
bool command_justify_full_action(DOM::Document& document, String const&)
{
    // Justify the selection with alignment "justify", then return true.
    justify_the_selection(document, JustifyAlignment::Justify);
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifyfull-command
bool command_justify_full_indeterminate(DOM::Document const& document)
{
    return justify_indeterminate(document, JustifyAlignment::Justify);
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifyfull-command
bool command_justify_full_state(DOM::Document const& document)
{
    return justify_state(document, JustifyAlignment::Justify);
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifyfull-command
String command_justify_full_value(DOM::Document const& document)
{
    return justify_value(document);
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifyleft-command
bool command_justify_left_action(DOM::Document& document, String const&)
{
    // Justify the selection with alignment "left", then return true.
    justify_the_selection(document, JustifyAlignment::Left);
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifyleft-command
bool command_justify_left_indeterminate(DOM::Document const& document)
{
    return justify_indeterminate(document, JustifyAlignment::Left);
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifyleft-command
bool command_justify_left_state(DOM::Document const& document)
{
    return justify_state(document, JustifyAlignment::Left);
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifyleft-command
String command_justify_left_value(DOM::Document const& document)
{
    return justify_value(document);
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifyright-command
bool command_justify_right_action(DOM::Document& document, String const&)
{
    // Justify the selection with alignment "right", then return true.
    justify_the_selection(document, JustifyAlignment::Right);
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifyright-command
bool command_justify_right_indeterminate(DOM::Document const& document)
{
    return justify_indeterminate(document, JustifyAlignment::Right);
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifyright-command
bool command_justify_right_state(DOM::Document const& document)
{
    return justify_state(document, JustifyAlignment::Right);
}

// https://w3c.github.io/editing/docs/execCommand/#the-justifyright-command
String command_justify_right_value(DOM::Document const& document)
{
    return justify_value(document);
}

// https://w3c.github.io/editing/docs/execCommand/#the-outdent-command
bool command_outdent_action(DOM::Document& document, String const&)
{
    // 1. Let items be a list of all lis that are inclusive ancestors of the active range's start and/or end node.
    Vector<GC::Ref<DOM::Node>> items;
    auto add_all_lis = [&items](GC::Ref<DOM::Node> node) {
        node->for_each_inclusive_ancestor([&items](GC::Ref<DOM::Node> ancestor) {
            if (is<HTML::HTMLLIElement>(*ancestor) && !items.contains_slow(ancestor))
                items.append(ancestor);
            return IterationDecision::Continue;
        });
    };
    auto range = active_range(document);
    add_all_lis(range->start_container());
    add_all_lis(range->end_container());

    // 2. For each item in items, normalize sublists of item.
    for (auto item : items)
        normalize_sublists_in_node(item);

    // 3. Block-extend the active range, and let new range be the result.
    auto new_range = block_extend_a_range(*active_range(document));

    // 4. Let node list be a list of nodes, initially empty.
    Vector<GC::Ref<DOM::Node>> node_list;

    // 5. For each node node contained in new range, append node to node list if the last member of node list (if any)
    //    is not an ancestor of node; node is editable; and either node has no editable descendants, or is an ol or ul,
    //    or is an li whose parent is an ol or ul.
    auto is_ol_or_ul = [](GC::Ptr<DOM::Node> node) {
        return is<HTML::HTMLOListElement>(node.ptr()) || is<HTML::HTMLUListElement>(node.ptr());
    };
    new_range->for_each_contained([&](GC::Ref<DOM::Node> node) {
        bool has_editable_descendants = false;
        node->for_each_in_subtree([&has_editable_descendants](GC::Ref<DOM::Node> descendant) {
            if (descendant->is_editable()) {
                has_editable_descendants = true;
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });

        if ((node_list.is_empty() || !node_list.last()->is_ancestor_of(node))
            && node->is_editable()
            && (!has_editable_descendants || is_ol_or_ul(node)
                || (is<HTML::HTMLLIElement>(*node) && is_ol_or_ul(node->parent()))))
            node_list.append(node);

        return IterationDecision::Continue;
    });

    // 6. While node list is not empty:
    while (!node_list.is_empty()) {
        // 1. While the first member of node list is an ol or ul or is not the child of an ol or ul, outdent it and
        //    remove it from node list.
        while (!node_list.is_empty() && (is_ol_or_ul(node_list.first()) || !is_ol_or_ul(node_list.first()->parent())))
            outdent(node_list.take_first());

        // 2. If node list is empty, break from these substeps.
        if (node_list.is_empty())
            break;

        // 3. Let sublist be a list of nodes, initially empty.
        Vector<GC::Ref<DOM::Node>> sublist;

        // 4. Remove the first member of node list and append it to sublist.
        sublist.append(node_list.take_first());

        // 5. While the first member of node list is the nextSibling of the last member of sublist, and the first member
        //    of node list is not an ol or ul, remove the first member of node list and append it to sublist.
        while (!node_list.is_empty() && node_list.first().ptr() == sublist.last()->next_sibling() && !is_ol_or_ul(node_list.first()))
            sublist.append(node_list.take_first());

        // 6. Record the values of sublist, and let values be the result.
        auto values = record_the_values_of_nodes(sublist);

        // 7. Split the parent of sublist.
        split_the_parent_of_nodes(sublist);

        // 8. Fix disallowed ancestors of each member of sublist.
        for (auto member : sublist)
            fix_disallowed_ancestors_of_node(member);

        // 9. Restore the values from values.
        restore_the_values_of_nodes(values);
    }

    // 7. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-removeformat-command
bool command_remove_format_action(DOM::Document& document, String const&)
{
    // 1. Let elements to remove be a list of every removeFormat candidate effectively contained in the active range.
    Vector<GC::Ref<DOM::Element>> elements_to_remove;
    for_each_node_effectively_contained_in_range(active_range(document), [&](GC::Ref<DOM::Node> descendant) {
        if (is_remove_format_candidate(descendant))
            elements_to_remove.append(static_cast<DOM::Element&>(*descendant));
        return TraversalDecision::Continue;
    });

    // 2. For each element in elements to remove:
    for (auto element : elements_to_remove) {
        // 1. While element has children, insert the first child of element into the parent of element immediately
        //    before element, preserving ranges.
        auto element_index = element->index();
        while (element->has_children())
            move_node_preserving_ranges(*element->first_child(), *element->parent(), element_index++);

        // 2. Remove element from its parent.
        element->remove();
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

    // 5. Let node list consist of all editable nodes effectively contained in the active range.
    Vector<GC::Ref<DOM::Node>> node_list;
    for_each_node_effectively_contained_in_range(active_range(document), [&](GC::Ref<DOM::Node> descendant) {
        if (descendant->is_editable())
            node_list.append(descendant);
        return TraversalDecision::Continue;
    });

    // 6. For each node in node list, while node's parent is a removeFormat candidate in the same editing host as node,
    //    split the parent of the one-node list consisting of node.
    for (auto node : node_list) {
        while (node->parent() && is_remove_format_candidate(*node->parent()) && is_in_same_editing_host(*node->parent(), node))
            split_the_parent_of_nodes({ node });
    }

    // 7. For each of the entries in the following list, in the given order, set the selection's value to null, with
    //    command as given.
    //    1. subscript
    //    2. bold
    //    3. fontName
    //    4. fontSize
    //    5. foreColor
    //    6. hiliteColor
    //    7. italic
    //    8. strikethrough
    //    9. underline
    for (auto command_name : { CommandNames::subscript, CommandNames::bold, CommandNames::fontName,
             CommandNames::fontSize, CommandNames::foreColor, CommandNames::hiliteColor, CommandNames::italic,
             CommandNames::strikethrough, CommandNames::underline }) {
        set_the_selections_value(document, command_name, {});
    }

    // 8. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-selectall-command
bool command_select_all_action(DOM::Document& document, String const&)
{
    // NOTE: The spec mentions "This is totally broken". So fair warning :^)

    // 1. Let target be the body element of the context object.
    GC::Ptr<DOM::Node> target = document.body();

    // 2. If target is null, let target be the context object's documentElement.
    if (!target)
        target = document.document_element();

    // 3. If target is null, call getSelection() on the context object, and call removeAllRanges() on the result.
    auto& selection = *document.get_selection();
    if (!target) {
        selection.remove_all_ranges();
    }

    // 4. Otherwise, call getSelection() on the context object, and call selectAllChildren(target) on the result.
    else {
        MUST(selection.select_all_children(*target));
    }

    // 5. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-strikethrough-command
bool command_strikethrough_action(DOM::Document& document, String const&)
{
    // If queryCommandState("strikethrough") returns true, set the selection's value to null.
    if (MUST(document.query_command_state(CommandNames::strikethrough))) {
        set_the_selections_value(document, CommandNames::strikethrough, {});
    }

    // Otherwise set the selection's value to "line-through".
    else {
        set_the_selections_value(document, CommandNames::strikethrough, "line-through"_string);
    }

    // Either way, return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-stylewithcss-command
bool command_style_with_css_action(DOM::Document& document, String const& value)
{
    // If value is an ASCII case-insensitive match for the string "false", set the CSS styling flag to false.
    // Otherwise, set the CSS styling flag to true.
    document.set_css_styling_flag(!value.equals_ignoring_ascii_case("false"sv));

    // Either way, return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-stylewithcss-command
bool command_style_with_css_state(DOM::Document const& document)
{
    // True if the CSS styling flag is true, otherwise false.
    return document.css_styling_flag();
}

// https://w3c.github.io/editing/docs/execCommand/#the-subscript-command
bool command_subscript_action(DOM::Document& document, String const&)
{
    // 1. Call queryCommandState("subscript"), and let state be the result.
    auto state = MUST(document.query_command_state(CommandNames::subscript));

    // 2. Set the selection's value to null.
    set_the_selections_value(document, CommandNames::subscript, {});

    // 3. If state is false, set the selection's value to "subscript".
    if (!state)
        set_the_selections_value(document, CommandNames::subscript, "subscript"_string);

    // 4. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-subscript-command
bool command_subscript_indeterminate(DOM::Document const& document)
{
    // True if either among formattable nodes that are effectively contained in the active range, there is at least one
    // with effective command value "subscript" and at least one with some other effective command value;
    bool has_subscript_value = false;
    bool has_other_value = false;
    bool has_mixed_value = false;
    for_each_node_effectively_contained_in_range(active_range(document), [&](GC::Ref<DOM::Node> descendant) {
        if (!is_formattable_node(descendant))
            return TraversalDecision::Continue;

        auto node_value = effective_command_value(descendant, CommandNames::subscript);
        if (!node_value.has_value())
            return TraversalDecision::Continue;

        if (node_value.value() == "subscript"sv) {
            has_subscript_value = true;
        } else {
            has_other_value = true;
            if (!has_mixed_value && node_value.value() == "mixed"sv)
                has_mixed_value = true;
        }
        if (has_subscript_value && has_other_value)
            return TraversalDecision::Break;

        return TraversalDecision::Continue;
    });
    if (has_subscript_value && has_other_value)
        return true;

    // or if there is some formattable node effectively contained in the active range with effective command value
    // "mixed". Otherwise false.
    return has_mixed_value;
}

// https://w3c.github.io/editing/docs/execCommand/#the-superscript-command
bool command_superscript_action(DOM::Document& document, String const&)
{
    // 1. Call queryCommandState("superscript"), and let state be the result.
    auto state = MUST(document.query_command_state(CommandNames::superscript));

    // 2. Set the selection's value to null.
    set_the_selections_value(document, CommandNames::superscript, {});

    // 3. If state is false, set the selection's value to "superscript".
    if (!state)
        set_the_selections_value(document, CommandNames::superscript, "superscript"_string);

    // 4. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-superscript-command
bool command_superscript_indeterminate(DOM::Document const& document)
{
    // True if either among formattable nodes that are effectively contained in the active range, there is at least one
    // with effective command value "superscript" and at least one with some other effective command value;
    bool has_superscript_value = false;
    bool has_other_value = false;
    bool has_mixed_value = false;
    for_each_node_effectively_contained_in_range(active_range(document), [&](GC::Ref<DOM::Node> descendant) {
        if (!is_formattable_node(descendant))
            return TraversalDecision::Continue;

        auto node_value = effective_command_value(descendant, CommandNames::superscript);
        if (!node_value.has_value())
            return TraversalDecision::Continue;

        if (node_value.value() == "superscript"sv) {
            has_superscript_value = true;
        } else {
            has_other_value = true;
            if (!has_mixed_value && node_value.value() == "mixed"sv)
                has_mixed_value = true;
        }
        if (has_superscript_value && has_other_value)
            return TraversalDecision::Break;

        return TraversalDecision::Continue;
    });
    if (has_superscript_value && has_other_value)
        return true;

    // or if there is some formattable node effectively contained in the active range with effective command value
    // "mixed". Otherwise false.
    return has_mixed_value;
}

// https://w3c.github.io/editing/docs/execCommand/#the-underline-command
bool command_underline_action(DOM::Document& document, String const&)
{
    // If queryCommandState("underline") returns true, set the selection's value to null.
    if (MUST(document.query_command_state(CommandNames::underline))) {
        set_the_selections_value(document, CommandNames::underline, {});
    }

    // Otherwise set the selection's value to "underline".
    else {
        set_the_selections_value(document, CommandNames::underline, "underline"_string);
    }

    // Either way, return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-unlink-command
bool command_unlink_action(DOM::Document& document, String const&)
{
    // 1. Let hyperlinks be a list of every a element that has an href attribute and is contained in the active range or
    //    is an ancestor of one of its boundary points.
    Vector<GC::Ref<DOM::Element>> hyperlinks;
    if (auto range = active_range(document)) {
        auto node_matches = [](GC::Ref<DOM::Node> node) {
            return is<HTML::HTMLAnchorElement>(*node)
                && static_cast<HTML::HTMLAnchorElement&>(*node).has_attribute(HTML::AttributeNames::href);
        };
        range->for_each_contained([&](GC::Ref<DOM::Node> node) {
            if (node_matches(node))
                hyperlinks.append(static_cast<DOM::Element&>(*node));
            return IterationDecision::Continue;
        });
        auto add_matching_ancestors = [&](GC::Ref<DOM::Node> node) {
            node->for_each_ancestor([&](GC::Ref<DOM::Node> ancestor) {
                if (node_matches(ancestor))
                    hyperlinks.append(static_cast<DOM::Element&>(*ancestor));
                return IterationDecision::Continue;
            });
        };
        add_matching_ancestors(range->start_container());
        add_matching_ancestors(range->end_container());
    }

    // 2. Clear the value of each member of hyperlinks.
    for (auto member : hyperlinks)
        clear_the_value(CommandNames::unlink, member);

    // 3. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#the-usecss-command
bool command_use_css_action(DOM::Document& document, String const& value)
{
    // If value is an ASCII case-insensitive match for the string "false", set the CSS styling flag to true.
    // Otherwise, set the CSS styling flag to false.
    document.set_css_styling_flag(value.equals_ignoring_ascii_case("false"sv));

    // Either way, return true.
    return true;
}

static Array const commands {
    // https://w3c.github.io/editing/docs/execCommand/#the-backcolor-command
    CommandDefinition {
        .command = CommandNames::backColor,
        .action = command_back_color_action,
        .relevant_css_property = CSS::PropertyID::BackgroundColor,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-bold-command
    CommandDefinition {
        .command = CommandNames::bold,
        .action = command_bold_action,
        .relevant_css_property = CSS::PropertyID::FontWeight,
        .inline_activated_values = { "bold"sv, "600"sv, "700"sv, "800"sv, "900"sv },
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-createlink-command
    CommandDefinition {
        .command = CommandNames::createLink,
        .action = command_create_link_action,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-delete-command
    CommandDefinition {
        .command = CommandNames::delete_,
        .action = command_delete_action,
        .preserves_overrides = true,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-defaultparagraphseparator-command
    CommandDefinition {
        .command = CommandNames::defaultParagraphSeparator,
        .action = command_default_paragraph_separator_action,
        .value = command_default_paragraph_separator_value,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-fontname-command
    CommandDefinition {
        .command = CommandNames::fontName,
        .action = command_font_name_action,
        .relevant_css_property = CSS::PropertyID::FontFamily,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-fontsize-command
    CommandDefinition {
        .command = CommandNames::fontSize,
        .action = command_font_size_action,
        .value = command_font_size_value,
        .relevant_css_property = CSS::PropertyID::FontSize,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-forecolor-command
    CommandDefinition {
        .command = CommandNames::foreColor,
        .action = command_fore_color_action,
        .relevant_css_property = CSS::PropertyID::Color,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-formatblock-command
    CommandDefinition {
        .command = CommandNames::formatBlock,
        .action = command_format_block_action,
        .indeterminate = command_format_block_indeterminate,
        .value = command_format_block_value,
        .preserves_overrides = true,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-forwarddelete-command
    CommandDefinition {
        .command = CommandNames::forwardDelete,
        .action = command_forward_delete_action,
        .preserves_overrides = true,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-hilitecolor-command
    CommandDefinition {
        .command = CommandNames::hiliteColor,
        .action = command_back_color_action, // For historical reasons, backColor and hiliteColor behave identically.
        .relevant_css_property = CSS::PropertyID::BackgroundColor,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-indent-command
    CommandDefinition {
        .command = CommandNames::indent,
        .action = command_indent_action,
        .preserves_overrides = true,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-inserthorizontalrule-command
    CommandDefinition {
        .command = CommandNames::insertHorizontalRule,
        .action = command_insert_horizontal_rule_action,
        .preserves_overrides = true,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-inserthtml-command
    CommandDefinition {
        .command = CommandNames::insertHTML,
        .action = command_insert_html_action,
        .preserves_overrides = true,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-insertimage-command
    CommandDefinition {
        .command = CommandNames::insertImage,
        .action = command_insert_image_action,
        .preserves_overrides = true,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-insertlinebreak-command
    CommandDefinition {
        .command = CommandNames::insertLineBreak,
        .action = command_insert_linebreak_action,
        .preserves_overrides = true,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-insertorderedlist-command
    CommandDefinition {
        .command = CommandNames::insertOrderedList,
        .action = command_insert_ordered_list_action,
        .indeterminate = command_insert_ordered_list_indeterminate,
        .state = command_insert_ordered_list_state,
        .preserves_overrides = true,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-insertparagraph-command
    CommandDefinition {
        .command = CommandNames::insertParagraph,
        .action = command_insert_paragraph_action,
        .preserves_overrides = true,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-inserttext-command
    CommandDefinition {
        .command = CommandNames::insertText,
        .action = command_insert_text_action,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-insertunorderedlist-command
    CommandDefinition {
        .command = CommandNames::insertUnorderedList,
        .action = command_insert_unordered_list_action,
        .indeterminate = command_insert_unordered_list_indeterminate,
        .state = command_insert_unordered_list_state,
        .preserves_overrides = true,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-italic-command
    CommandDefinition {
        .command = CommandNames::italic,
        .action = command_italic_action,
        .relevant_css_property = CSS::PropertyID::FontStyle,
        .inline_activated_values = { "italic"sv, "oblique"sv },
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-justifycenter-command
    CommandDefinition {
        .command = CommandNames::justifyCenter,
        .action = command_justify_center_action,
        .indeterminate = command_justify_center_indeterminate,
        .state = command_justify_center_state,
        .value = command_justify_center_value,
        .preserves_overrides = true,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-justifyfull-command
    CommandDefinition {
        .command = CommandNames::justifyFull,
        .action = command_justify_full_action,
        .indeterminate = command_justify_full_indeterminate,
        .state = command_justify_full_state,
        .value = command_justify_full_value,
        .preserves_overrides = true,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-justifyleft-command
    CommandDefinition {
        .command = CommandNames::justifyLeft,
        .action = command_justify_left_action,
        .indeterminate = command_justify_left_indeterminate,
        .state = command_justify_left_state,
        .value = command_justify_left_value,
        .preserves_overrides = true,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-justifyright-command
    CommandDefinition {
        .command = CommandNames::justifyRight,
        .action = command_justify_right_action,
        .indeterminate = command_justify_right_indeterminate,
        .state = command_justify_right_state,
        .value = command_justify_right_value,
        .preserves_overrides = true,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-outdent-command
    CommandDefinition {
        .command = CommandNames::outdent,
        .action = command_outdent_action,
        .preserves_overrides = true,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-removeformat-command
    CommandDefinition {
        .command = CommandNames::removeFormat,
        .action = command_remove_format_action,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-selectall-command
    CommandDefinition {
        .command = CommandNames::selectAll,
        .action = command_select_all_action,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-strikethrough-command
    CommandDefinition {
        .command = CommandNames::strikethrough,
        .action = command_strikethrough_action,
        .inline_activated_values = { "line-through"sv },
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-stylewithcss-command
    CommandDefinition {
        .command = CommandNames::styleWithCSS,
        .action = command_style_with_css_action,
        .state = command_style_with_css_state,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-subscript-command
    CommandDefinition {
        .command = CommandNames::subscript,
        .action = command_subscript_action,
        .indeterminate = command_subscript_indeterminate,
        .inline_activated_values = { "subscript"sv },
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-superscript-command
    CommandDefinition {
        .command = CommandNames::superscript,
        .action = command_superscript_action,
        .indeterminate = command_superscript_indeterminate,
        .inline_activated_values = { "superscript"sv },
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-underline-command
    CommandDefinition {
        .command = CommandNames::underline,
        .action = command_underline_action,
        .inline_activated_values = { "underline"sv },
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-unlink-command
    CommandDefinition {
        .command = CommandNames::unlink,
        .action = command_unlink_action,
    },
    // https://w3c.github.io/editing/docs/execCommand/#the-usecss-command
    CommandDefinition {
        .command = CommandNames::useCSS,
        .action = command_use_css_action,
    },
};

Optional<CommandDefinition const&> find_command_definition(FlyString const& command)
{
    for (auto& definition : commands) {
        if (command.equals_ignoring_ascii_case(definition.command))
            return definition;
    }
    return {};
}

}
