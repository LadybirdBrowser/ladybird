/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/CommandNames.h>
#include <LibWeb/Editing/Commands.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLTableElement.h>
#include <LibWeb/Namespace.h>

namespace Web::Editing {

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
    canonicalize_whitespace(active_range.start_container(), active_range.start_offset());

    // 3. Let node and offset be the active range's start node and offset.
    auto node = active_range.start_container();
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
        if ((offset == 0 && is_inline_node(node)) || is_invisible_node(node)) {
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
    if (is_block_node(node)) {
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
    if (is_inline_node(node))
        return true;

    // 7. If node is an li or dt or dd and is the first child of its parent, and offset is zero:
    auto& node_element = static_cast<DOM::Element&>(*node);
    if (offset == 0 && node->index() == 0
        && node_element.local_name().is_one_of(HTML::TagNames::li, HTML::TagNames::dt, HTML::TagNames::dd)) {
        // 1. Let items be a list of all lis that are ancestors of node.
        auto items = Vector<GC::Ref<DOM::Element>>();
        GC::Ptr<DOM::Node> ancestor = node->parent();
        do {
            auto& ancestor_element = static_cast<DOM::Element&>(*ancestor);
            if (ancestor_element.local_name() == HTML::TagNames::li)
                items.append(ancestor_element);
            ancestor = ancestor->parent();
        } while (ancestor);

        // 2. Normalize sublists of each item in items.
        for (auto item : items)
            normalize_sublists_in_node(*item);

        // 3. Record the values of the one-node list consisting of node, and let values be the
        //    result.
        auto values = record_the_values_of_nodes({ node });

        // 4. Split the parent of the one-node list consisting of node.
        split_the_parent_of_nodes({ node });

        // 5. Restore the values from values.
        restore_the_values_of_nodes(values);

        // FIXME: 6. If node is a dd or dt, and it is not an allowed child of any of its ancestors in the
        //    same editing host, set the tag name of node to the default single-line container name
        //    and let node be the result.

        // FIXME: 7. Fix disallowed ancestors of node.

        // 8. Return true.
        return true;
    }

    // 8. Let start node equal node and let start offset equal offset.
    auto start_node = node;
    auto start_offset = offset;

    // 9. Repeat the following steps:
    while (true) {
        // 1. If start offset is zero, set start offset to the index of start node and then set
        //    start node to its parent.
        if (start_offset == 0) {
            start_offset = start_node->index();
            start_node = *start_node->parent();
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

    // FIXME: 10. If offset is zero, and node has an editable inclusive ancestor in the same editing host
    //     that's an indentation element:
    if (false) {
        // FIXME: 1. Block-extend the range whose start and end are both (node, 0), and let new range be
        //    the result.

        // FIXME: 2. Let node list be a list of nodes, initially empty.

        // FIXME: 3. For each node current node contained in new range, append current node to node list if
        //    the last member of node list (if any) is not an ancestor of current node, and current
        //    node is editable but has no editable descendants.

        // FIXME: 4. Outdent each node in node list.

        // 5. Return true.
        return true;
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
        if (child_element.local_name() == HTML::TagNames::hr
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
    auto is_li_dt_or_dd = [](DOM::Element const& node) {
        return node.local_name().is_one_of(HTML::TagNames::li, HTML::TagNames::dt, HTML::TagNames::dd);
    };
    auto* start_offset_child = start_node->child_at_index(start_offset);
    if (start_offset != 0 && is<DOM::Element>(start_offset_child)
        && is_li_dt_or_dd(static_cast<DOM::Element&>(*start_offset_child))
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

    // FIXME: 15. If start node's child with index start offset is an li or dt or dd, and that child's
    //     previousSibling is also an li or dt or dd:
    if (false) {
        // FIXME: 1. Call cloneRange() on the active range, and let original range be the result.

        // FIXME: 2. Set start node to its child with index start offset − 1.

        // FIXME: 3. Set start offset to start node's length.

        // FIXME: 4. Set node to start node's nextSibling.

        // FIXME: 5. Call collapse(start node, start offset) on the context object's selection.

        // FIXME: 6. Call extend(node, 0) on the context object's selection.

        // FIXME: 7. Delete the selection.

        // FIXME: 8. Call removeAllRanges() on the context object's selection.

        // FIXME: 9. Call addRange(original range) on the context object's selection.

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

    // FIXME: 19. Delete the selection, with direction "backward".
    delete_the_selection(selection);

    // 20. Return true.
    return true;
}

static Array const commands {
    CommandDefinition { CommandNames::delete_, command_delete_action, {}, {}, {} },
    CommandDefinition { CommandNames::defaultParagraphSeparator, command_default_paragraph_separator_action, {}, {}, command_default_paragraph_separator_value },
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
