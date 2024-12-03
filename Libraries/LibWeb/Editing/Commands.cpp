/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
        auto items = Vector<GC::Ref<DOM::Element>>();
        GC::Ptr<DOM::Node> ancestor = node->parent();
        while (ancestor) {
            if (is<HTML::HTMLLIElement>(*ancestor))
                items.append(static_cast<DOM::Element&>(*ancestor));
            ancestor = ancestor->parent();
        }

        // 2. Normalize sublists of each item in items.
        for (auto item : items)
            normalize_sublists_in_node(*item);

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
            ancestor = node->parent();
            bool allowed_child_of_any_ancestor = false;
            while (ancestor) {
                if (is_in_same_editing_host(*node, *ancestor) && is_allowed_child_of_node(GC::Ref { *node }, GC::Ref { *ancestor })) {
                    allowed_child_of_any_ancestor = true;
                    break;
                }
                ancestor = ancestor->parent();
            }
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

// https://w3c.github.io/editing/docs/execCommand/#the-insertparagraph-command
bool command_insert_paragraph_action(DOM::Document& document, String const&)
{
    // 1. Delete the selection.
    auto& selection = *document.get_selection();
    delete_the_selection(selection);

    // 2. If the active range's start node is neither editable nor an editing host, return true.
    auto& active_range = *selection.range();
    GC::Ptr<DOM::Node> node = active_range.start_container();
    if (!node->is_editable() && !is_editing_host(*node))
        return true;

    // 3. Let node and offset be the active range's start node and offset.
    // NOTE: node is set in step 2
    auto offset = active_range.start_offset();

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
        new_range->start_container()->for_each_in_inclusive_subtree([&](DOM::Node& node) {
            if (is_allowed_child_of_node(GC::Ref { node }, HTML::TagNames::p) && new_range->contains_node(node)) {
                node_list.append(node);
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });

        // 5. If node list is empty:
        if (node_list.is_empty()) {
            // 1. If tag is not an allowed child of the active range's start node, return true.
            if (!is_allowed_child_of_node(tag, active_range.start_container()))
                return true;

            // 2. Set container to the result of calling createElement(tag) on the context object.
            container = MUST(DOM::create_element(document, tag, Namespace::HTML));

            // 3. Call insertNode(container) on the active range.
            MUST(active_range.insert_node(*container));

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
        MUST(active_range.insert_node(br));

        // 3. Call collapse(node, offset + 1) on the context object's selection.
        MUST(selection.collapse(node, offset + 1));

        // 4. If br is the last descendant of container, let br be the result of calling createElement("br") on the
        //    context object, then call insertNode(br) on the active range.
        GC::Ptr<DOM::Node> last_descendant = container->last_child();
        while (last_descendant->has_children())
            last_descendant = last_descendant->last_child();
        if (br == last_descendant) {
            br = MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML));
            MUST(active_range.insert_node(br));
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
            GC::Ptr<DOM::Node> ancestor = container->parent();
            while (ancestor) {
                if (is_allowed_child_of_node(GC::Ref { *container }, GC::Ref { *ancestor })
                    && is_in_same_editing_host(*container, *ancestor)) {
                    allowed_child_of_any_ancestor = true;
                    break;
                }
                ancestor = ancestor->parent();
            }
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
    auto new_line_range = DOM::Range::create(active_range.start_container(), active_range.start_offset(), *container, container->length());

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

    VERIFY(is<DOM::Element>(*container));
    auto& container_element = static_cast<DOM::Element&>(*container);
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
    GC::Ptr<DOM::Element> new_container = MUST(DOM::create_element(document, new_container_name, Namespace::HTML));

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
    // FIXME: this is probably wildly inefficient
    Vector<GC::Ref<DOM::Node>> contained_nodes;
    auto common_ancestor = new_line_range->common_ancestor_container();
    common_ancestor->for_each_in_subtree([&](GC::Ref<DOM::Node> child_node) {
        if (new_line_range->contains_node(child_node))
            contained_nodes.append(child_node);
        return TraversalDecision::Continue;
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
        new_container = static_cast<HTML::HTMLElement*>(new_container->last_child());
    }

    // 32. If container has no visible children, call createElement("br") on the context object, and append the result
    //     as the last child of container.
    bool has_visible_child = false;
    container->for_each_child([&has_visible_child](GC::Ref<DOM::Node> child) {
        if (is_visible_node(child)) {
            has_visible_child = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (!has_visible_child)
        MUST(container->append_child(MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML))));

    // 33. If new container has no visible children, call createElement("br") on the context object, and append the
    //     result as the last child of new container.
    has_visible_child = false;
    new_container->for_each_child([&has_visible_child](GC::Ref<DOM::Node> child) {
        if (is_visible_node(child)) {
            has_visible_child = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (!has_visible_child)
        MUST(new_container->append_child(MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML))));

    // 34. Call collapse(new container, 0) on the context object's selection.
    MUST(document.get_selection()->collapse(new_container, 0));

    // 35. Return true
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

static Array const commands {
    CommandDefinition { CommandNames::delete_, command_delete_action, {}, {}, {} },
    CommandDefinition { CommandNames::defaultParagraphSeparator, command_default_paragraph_separator_action, {}, {}, command_default_paragraph_separator_value },
    CommandDefinition { CommandNames::insertParagraph, command_insert_paragraph_action, {}, {}, {} },
    CommandDefinition { CommandNames::styleWithCSS, command_style_with_css_action, {}, command_style_with_css_state, {} },
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
