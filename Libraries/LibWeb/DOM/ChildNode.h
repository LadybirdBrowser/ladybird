/*
 * Copyright (c) 2021-2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/NodeOperations.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#childnode
template<typename NodeType>
class ChildNode {
public:
    // https://dom.spec.whatwg.org/#dom-childnode-before
    WebIDL::ExceptionOr<void> before(Vector<Variant<JS::Handle<Node>, String>> const& nodes)
    {
        auto* node = static_cast<NodeType*>(this);

        // 1. Let parent be this’s parent.
        auto* parent = node->parent();

        // 2. If parent is null, then return.
        if (!parent)
            return {};

        // 3. Let viablePreviousSibling be this’s first preceding sibling not in nodes; otherwise null.
        auto viable_previous_sibling = viable_previous_sibling_for_insertion(nodes);

        // 4. Let node be the result of converting nodes into a node, given nodes and this’s node document.
        auto node_to_insert = TRY(convert_nodes_to_single_node(nodes, node->document()));

        // 5. If viablePreviousSibling is null, then set it to parent’s first child; otherwise to viablePreviousSibling’s next sibling.
        if (!viable_previous_sibling)
            viable_previous_sibling = parent->first_child();
        else
            viable_previous_sibling = viable_previous_sibling->next_sibling();

        // 6. Pre-insert node into parent before viablePreviousSibling.
        (void)TRY(parent->pre_insert(node_to_insert, viable_previous_sibling));

        return {};
    }

    // https://dom.spec.whatwg.org/#dom-childnode-after
    WebIDL::ExceptionOr<void> after(Vector<Variant<JS::Handle<Node>, String>> const& nodes)
    {
        auto* node = static_cast<NodeType*>(this);

        // 1. Let parent be this’s parent.
        auto* parent = node->parent();

        // 2. If parent is null, then return.
        if (!parent)
            return {};

        // 3. Let viableNextSibling be this’s first following sibling not in nodes; otherwise null.
        auto viable_next_sibling = viable_next_sibling_for_insertion(nodes);

        // 4. Let node be the result of converting nodes into a node, given nodes and this’s node document.
        auto node_to_insert = TRY(convert_nodes_to_single_node(nodes, node->document()));

        // 5. Pre-insert node into parent before viableNextSibling.
        (void)TRY(parent->pre_insert(node_to_insert, viable_next_sibling));

        return {};
    }

    // https://dom.spec.whatwg.org/#dom-childnode-replacewith
    WebIDL::ExceptionOr<void> replace_with(Vector<Variant<JS::Handle<Node>, String>> const& nodes)
    {
        auto* node = static_cast<NodeType*>(this);

        // 1. Let parent be this’s parent.
        auto* parent = node->parent();

        // 2. If parent is null, then return.
        if (!parent)
            return {};

        // 3. Let viableNextSibling be this’s first following sibling not in nodes; otherwise null.
        auto viable_next_sibling = viable_next_sibling_for_insertion(nodes);

        // 4. Let node be the result of converting nodes into a node, given nodes and this’s node document.
        auto node_to_insert = TRY(convert_nodes_to_single_node(nodes, node->document()));

        // 5. If this’s parent is parent, replace this with node within parent.
        // Note: This could have been inserted into node.
        if (node->parent() == parent) {
            (void)TRY(parent->replace_child(node_to_insert, *node));
            return {};
        }

        // 6. Otherwise, pre-insert node into parent before viableNextSibling.
        (void)TRY(parent->pre_insert(node_to_insert, viable_next_sibling));

        return {};
    }

    // https://dom.spec.whatwg.org/#dom-childnode-remove
    void remove_binding()
    {
        auto* node = static_cast<NodeType*>(this);

        // 1. If this’s parent is null, then return.
        if (!node->parent())
            return;

        // 2. Remove this.
        node->remove();
    }

protected:
    ChildNode() = default;

private:
    JS::GCPtr<Node> viable_previous_sibling_for_insertion(Vector<Variant<JS::Handle<Node>, String>> const& nodes)
    {
        auto* node = static_cast<NodeType*>(this);

        for (auto* sibling = node->previous_sibling(); sibling; sibling = sibling->previous_sibling()) {
            bool contained_in_nodes = false;

            for (auto const& node_or_string : nodes) {
                if (!node_or_string.template has<JS::Handle<Node>>())
                    continue;

                auto const& node_in_vector = node_or_string.template get<JS::Handle<Node>>();
                if (node_in_vector.cell() == sibling) {
                    contained_in_nodes = true;
                    break;
                }
            }

            if (!contained_in_nodes)
                return sibling;
        }

        return nullptr;
    }

    JS::GCPtr<Node> viable_next_sibling_for_insertion(Vector<Variant<JS::Handle<Node>, String>> const& nodes)
    {
        auto* node = static_cast<NodeType*>(this);

        for (auto* sibling = node->next_sibling(); sibling; sibling = sibling->next_sibling()) {
            bool contained_in_nodes = false;

            for (auto const& node_or_string : nodes) {
                if (!node_or_string.template has<JS::Handle<Node>>())
                    continue;

                auto const& node_in_vector = node_or_string.template get<JS::Handle<Node>>();
                if (node_in_vector.cell() == sibling) {
                    contained_in_nodes = true;
                    break;
                }
            }

            if (!contained_in_nodes)
                return sibling;
        }

        return nullptr;
    }
};

}
