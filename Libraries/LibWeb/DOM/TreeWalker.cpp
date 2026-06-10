/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NodeFilter.h>
#include <LibWeb/DOM/TreeWalker.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(TreeWalker);

static Optional<TraversalResult> failure_from_filter_result(TraversalFilterResult result)
{
    if (result.type == TraversalFilterResult::Type::AlreadyActive)
        return TraversalResult::already_active();
    if (result.type == TraversalFilterResult::Type::CallbackException)
        return TraversalResult::callback_exception();
    return {};
}

TreeWalker::TreeWalker(Node& root)
    : m_root(root)
    , m_current(root)
{
}

TreeWalker::~TreeWalker() = default;

void TreeWalker::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_filter);
    visitor.visit(m_root);
    visitor.visit(m_current);
}

// https://dom.spec.whatwg.org/#dom-document-createtreewalker
GC::Ref<TreeWalker> TreeWalker::create(Node& root, unsigned what_to_show, GC::Ptr<NodeFilter> filter)
{
    // 1. Let walker be a new TreeWalker object.
    // 2. Set walker’s root and walker’s current to root.
    auto walker = GC::Heap::the().allocate<TreeWalker>(root);

    // 3. Set walker’s whatToShow to whatToShow.
    walker->m_what_to_show = what_to_show;

    // 4. Set walker’s filter to filter.
    walker->m_filter = filter;

    // 5. Return walker.
    return walker;
}

// https://dom.spec.whatwg.org/#dom-treewalker-currentnode
GC::Ref<Node> TreeWalker::current_node() const
{
    return *m_current;
}

// https://dom.spec.whatwg.org/#dom-treewalker-currentnode
void TreeWalker::set_current_node(Node& node)
{
    m_current = node;
}

// https://dom.spec.whatwg.org/#dom-treewalker-parentnode
TraversalResult TreeWalker::parent_node(TraversalFilter const& traversal_filter)
{
    // 1. Let node be this’s current.
    GC::Ptr<Node> node = m_current;

    // 2. While node is non-null and is not this’s root:
    while (node && node != m_root) {
        // 1. Set node to node’s parent.
        node = node->parent();

        // 2. If node is non-null and filtering node within this returns FILTER_ACCEPT,
        //    then set this’s current to node and return node.
        if (node) {
            auto result = filter(traversal_filter, *node);
            if (auto failure = failure_from_filter_result(result); failure.has_value())
                return failure.release_value();
            if (result.is(NodeFilter::Result::FILTER_ACCEPT)) {
                m_current = *node;
                return TraversalResult::from_node(node);
            }
        }
    }

    return TraversalResult::null();
}

// https://dom.spec.whatwg.org/#dom-treewalker-firstchild
TraversalResult TreeWalker::first_child(TraversalFilter const& traversal_filter)
{
    return traverse_children(traversal_filter, ChildTraversalType::First);
}

// https://dom.spec.whatwg.org/#dom-treewalker-lastchild
TraversalResult TreeWalker::last_child(TraversalFilter const& traversal_filter)
{
    return traverse_children(traversal_filter, ChildTraversalType::Last);
}

// https://dom.spec.whatwg.org/#dom-treewalker-previoussibling
TraversalResult TreeWalker::previous_sibling(TraversalFilter const& traversal_filter)
{
    return traverse_siblings(traversal_filter, SiblingTraversalType::Previous);
}

// https://dom.spec.whatwg.org/#dom-treewalker-nextsibling
TraversalResult TreeWalker::next_sibling(TraversalFilter const& traversal_filter)
{
    return traverse_siblings(traversal_filter, SiblingTraversalType::Next);
}

// https://dom.spec.whatwg.org/#dom-treewalker-previousnode
TraversalResult TreeWalker::previous_node(TraversalFilter const& traversal_filter)
{
    // 1. Let node be this’s current.
    GC::Ref<Node> node = m_current;

    // 2. While node is not this’s root:
    while (node != m_root) {
        // 1. Let sibling be node’s previous sibling.
        GC::Ptr<Node> sibling = node->previous_sibling();

        // 2. While sibling is non-null:
        while (sibling) {
            // 1. Set node to sibling.
            node = *sibling;

            // 2. Let result be the result of filtering node within this.
            auto result = filter(traversal_filter, *node);
            if (auto failure = failure_from_filter_result(result); failure.has_value())
                return failure.release_value();

            // 3. While result is not FILTER_REJECT and node has a child:
            while (!result.is(NodeFilter::Result::FILTER_REJECT) && node->has_children()) {
                // 1. Set node to node’s last child.
                node = *node->last_child();

                // 2. Set result to the result of filtering node within this.
                result = filter(traversal_filter, *node);
                if (auto failure = failure_from_filter_result(result); failure.has_value())
                    return failure.release_value();
            }

            // 4. If result is FILTER_ACCEPT, then set this’s current to node and return node.
            if (result.is(NodeFilter::Result::FILTER_ACCEPT)) {
                m_current = node;
                return TraversalResult::from_node(node);
            }

            // 5. Set sibling to node’s previous sibling.
            sibling = node->previous_sibling();
        }

        // 3. If node is this’s root or node’s parent is null, then return null.
        if (node == m_root || !node->parent())
            return TraversalResult::null();

        // 4. Set node to node’s parent.
        node = *node->parent();

        // 5. If the return value of filtering node within this is FILTER_ACCEPT, then set this’s current to node and return node.
        auto result = filter(traversal_filter, *node);
        if (auto failure = failure_from_filter_result(result); failure.has_value())
            return failure.release_value();
        if (result.is(NodeFilter::Result::FILTER_ACCEPT)) {
            m_current = node;
            return TraversalResult::from_node(node);
        }
    }
    // 3. Return null.
    return TraversalResult::null();
}

// https://dom.spec.whatwg.org/#dom-treewalker-nextnode
TraversalResult TreeWalker::next_node(TraversalFilter const& traversal_filter)
{
    // 1. Let node be this’s current.
    GC::Ref<Node> node = m_current;

    // 2. Let result be FILTER_ACCEPT.
    auto result = NodeFilter::Result::FILTER_ACCEPT;

    // 3. While true:
    while (true) {
        // 1. While result is not FILTER_REJECT and node has a child:
        while (result != NodeFilter::Result::FILTER_REJECT && node->has_children()) {
            // 1. Set node to its first child.
            node = *node->first_child();

            // 2. Set result to the result of filtering node within this.
            auto filter_result = filter(traversal_filter, *node);
            if (auto failure = failure_from_filter_result(filter_result); failure.has_value())
                return failure.release_value();
            result = filter_result.result;

            // 3. If result is FILTER_ACCEPT, then set this’s current to node and return node.
            if (result == NodeFilter::Result::FILTER_ACCEPT) {
                m_current = *node;
                return TraversalResult::from_node(node);
            }
        }

        // 2. Let sibling be null.
        GC::Ptr<Node> sibling = nullptr;

        // 3. Let temporary be node.
        GC::Ptr<Node> temporary = node;

        // 4. While temporary is non-null:
        while (temporary) {
            // 1. If temporary is this’s root, then return null.
            if (temporary == m_root)
                return TraversalResult::null();

            // 2. Set sibling to temporary’s next sibling.
            sibling = temporary->next_sibling();

            // 3. If sibling is non-null, then set node to sibling and break.
            if (sibling) {
                node = *sibling;
                break;
            }

            // 4. Set temporary to temporary’s parent.
            temporary = temporary->parent();

            // NON-STANDARD: If temporary is null, then return null.
            //               This prevents us from infinite looping if the current node is not connected.
            //               Spec bug: https://github.com/whatwg/dom/issues/1102
            if (temporary == nullptr) {
                return TraversalResult::null();
            }
        }

        // 5. Set result to the result of filtering node within this.
        auto filter_result = filter(traversal_filter, *node);
        if (auto failure = failure_from_filter_result(filter_result); failure.has_value())
            return failure.release_value();
        result = filter_result.result;

        // 6. If result is FILTER_ACCEPT, then set this’s current to node and return node.
        if (result == NodeFilter::Result::FILTER_ACCEPT) {
            m_current = *node;
            return TraversalResult::from_node(node);
        }
    }
}

// https://dom.spec.whatwg.org/#concept-traversal-filter
GC::Ptr<NodeFilter> TreeWalker::filter() const
{
    return m_filter;
}

// https://dom.spec.whatwg.org/#concept-node-filter
TraversalFilterResult TreeWalker::filter(TraversalFilter const& traversal_filter, Node& node)
{
    // 1. If traverser’s active flag is set, then throw an "InvalidStateError" DOMException.
    if (m_active)
        return TraversalFilterResult::already_active();

    // 2. Let n be node’s nodeType attribute value − 1.
    auto n = node.node_type() - 1;

    // 3. If the nth bit (where 0 is the least significant bit) of traverser’s whatToShow is not set, then return FILTER_SKIP.
    if (!(m_what_to_show & (1u << n)))
        return TraversalFilterResult::skip();

    // 4. If traverser’s filter is null, then return FILTER_ACCEPT.
    if (!m_filter)
        return TraversalFilterResult::accept();

    // 5. Set traverser’s active flag.
    m_active = true;

    // 6. Let result be the return value of call a user object’s operation with traverser’s filter, "acceptNode", and « node ».
    //    If this throws an exception, then unset traverser’s active flag and rethrow the exception.
    auto result = traversal_filter(node);
    if (!result.has_value()) {
        m_active = false;
        return TraversalFilterResult::callback_exception();
    }

    // 7. Unset traverser’s active flag.
    m_active = false;

    // 8. Return result.
    return TraversalFilterResult::from_result(*result);
}

// https://dom.spec.whatwg.org/#concept-traverse-children
TraversalResult TreeWalker::traverse_children(TraversalFilter const& traversal_filter, ChildTraversalType type)
{
    // 1. Let node be walker’s current.
    GC::Ptr<Node> node = m_current;

    // 2. Set node to node’s first child if type is first, and node’s last child if type is last.
    node = type == ChildTraversalType::First ? node->first_child() : node->last_child();

    // 3. While node is non-null:
    while (node) {
        // 1. Let result be the result of filtering node within walker.
        auto result = filter(traversal_filter, *node);
        if (auto failure = failure_from_filter_result(result); failure.has_value())
            return failure.release_value();

        // 2. If result is FILTER_ACCEPT, then set walker’s current to node and return node.
        if (result.is(NodeFilter::Result::FILTER_ACCEPT)) {
            m_current = *node;
            return TraversalResult::from_node(node);
        }

        // 3. If result is FILTER_SKIP, then:
        if (result.is(NodeFilter::Result::FILTER_SKIP)) {
            // 1. Let child be node’s first child if type is first, and node’s last child if type is last.
            GC::Ptr<Node> child = type == ChildTraversalType::First ? node->first_child() : node->last_child();

            // 2. If child is non-null, then set node to child and continue.
            if (child) {
                node = child;
                continue;
            }
        }

        // 4. While node is non-null:
        while (node) {
            // 1. Let sibling be node’s next sibling if type is first, and node’s previous sibling if type is last.
            GC::Ptr<Node> sibling = type == ChildTraversalType::First ? node->next_sibling() : node->previous_sibling();

            // 2. If sibling is non-null, then set node to sibling and break.
            if (sibling) {
                node = sibling;
                break;
            }

            // 3. Let parent be node’s parent.
            GC::Ptr<Node> parent = node->parent();

            // 4. If parent is null, walker’s root, or walker’s current, then return null.
            if (!parent || parent == m_root || parent == m_current)
                return TraversalResult::null();

            // 5. Set node to parent.
            node = parent;
        }
    }

    // 4. Return null.
    return TraversalResult::null();
}

// https://dom.spec.whatwg.org/#concept-traverse-siblings
TraversalResult TreeWalker::traverse_siblings(TraversalFilter const& traversal_filter, SiblingTraversalType type)
{
    // 1. Let node be walker’s current.
    GC::Ptr<Node> node = m_current;

    // 2. If node is root, then return null.
    if (node == m_root)
        return TraversalResult::null();

    // 3. While true:
    while (true) {
        // 1. Let sibling be node’s next sibling if type is next, and node’s previous sibling if type is previous.
        GC::Ptr<Node> sibling = type == SiblingTraversalType::Next ? node->next_sibling() : node->previous_sibling();

        // 2. While sibling is non-null:
        while (sibling) {
            // 1. Set node to sibling.
            node = sibling;

            // 2. Let result be the result of filtering node within walker.
            auto result = filter(traversal_filter, *node);
            if (auto failure = failure_from_filter_result(result); failure.has_value())
                return failure.release_value();

            // 3. If result is FILTER_ACCEPT, then set walker’s current to node and return node.
            if (result.is(NodeFilter::Result::FILTER_ACCEPT)) {
                m_current = *node;
                return TraversalResult::from_node(node);
            }

            // 4. Set sibling to node’s first child if type is next, and node’s last child if type is previous.
            sibling = type == SiblingTraversalType::Next ? node->first_child() : node->last_child();

            // 5. If result is FILTER_REJECT or sibling is null, then set sibling to node’s next sibling if type is next, and node’s previous sibling if type is previous.
            if (result.is(NodeFilter::Result::FILTER_REJECT) || !sibling)
                sibling = type == SiblingTraversalType::Next ? node->next_sibling() : node->previous_sibling();
        }

        // 3. Set node to node’s parent.
        node = node->parent();

        // 4. If node is null or walker’s root, then return null.
        if (!node || node == m_root)
            return TraversalResult::null();

        // 5. If the return value of filtering node within walker is FILTER_ACCEPT, then return null.
        auto result = filter(traversal_filter, *node);
        if (auto failure = failure_from_filter_result(result); failure.has_value())
            return failure.release_value();
        if (result.is(NodeFilter::Result::FILTER_ACCEPT))
            return TraversalResult::null();
    }
}

}
