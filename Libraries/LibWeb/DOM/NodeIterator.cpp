/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/NodeIteratorPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NodeIterator.h>
#include <LibWeb/WebIDL/AbstractOperations.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(NodeIterator);

NodeIterator::NodeIterator(JS::Realm& realm, Node& root)
    : PlatformObject(realm)
    , m_root(root)
    , m_reference({ root })
{
    root.document().register_node_iterator({}, *this);
}

NodeIterator::~NodeIterator() = default;

void NodeIterator::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(NodeIterator);
}

void NodeIterator::finalize()
{
    Base::finalize();
    m_root->document().unregister_node_iterator({}, *this);
}

void NodeIterator::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_filter);
    visitor.visit(m_root);
    visitor.visit(m_reference.node);

    if (m_traversal_pointer.has_value())
        visitor.visit(m_traversal_pointer->node);
}

// https://dom.spec.whatwg.org/#dom-document-createnodeiterator
GC::Ref<NodeIterator> NodeIterator::create(JS::Realm& realm, Node& root, unsigned what_to_show, GC::Ptr<NodeFilter> filter)
{
    // 1. Let iterator be a new NodeIterator object.
    // 2. Set iterator’s root and iterator’s reference to root.
    // 3. Set iterator’s pointer before reference to true.
    auto iterator = realm.create<NodeIterator>(realm, root);

    // 4. Set iterator’s whatToShow to whatToShow.
    iterator->m_what_to_show = what_to_show;

    // 5. Set iterator’s filter to filter.
    iterator->m_filter = filter;

    // 6. Return iterator.
    return iterator;
}

// https://dom.spec.whatwg.org/#dom-nodeiterator-detach
void NodeIterator::detach()
{
    // The detach() method steps are to do nothing.
    // Its functionality (disabling a NodeIterator object) was removed, but the method itself is preserved for compatibility.
}

// https://dom.spec.whatwg.org/#concept-nodeiterator-traverse
JS::ThrowCompletionOr<GC::Ptr<Node>> NodeIterator::traverse(Direction direction)
{
    // 1. Let node be iterator’s reference.
    // 2. Let beforeNode be iterator’s pointer before reference.
    m_traversal_pointer = m_reference;

    GC::Ptr<Node> candidate;

    // 3. While true:
    while (true) {
        // 4. Branch on direction:
        if (direction == Direction::Next) {
            // next
            // If beforeNode is false, then set node to the first node following node in iterator’s iterator collection.
            // If there is no such node, then return null.
            if (!m_traversal_pointer->is_before_node) {
                auto* next_node = m_traversal_pointer->node->next_in_pre_order(m_root.ptr());
                if (!next_node)
                    return nullptr;
                m_traversal_pointer->node = *next_node;
            } else {
                // If beforeNode is true, then set it to false.
                m_traversal_pointer->is_before_node = false;
            }
        } else {
            // previous
            // If beforeNode is true, then set node to the first node preceding node in iterator’s iterator collection.
            // If there is no such node, then return null.
            if (m_traversal_pointer->is_before_node) {
                if (m_traversal_pointer->node.ptr() == m_root.ptr())
                    return nullptr;
                auto* previous_node = m_traversal_pointer->node->previous_in_pre_order();
                if (!previous_node)
                    return nullptr;
                m_traversal_pointer->node = *previous_node;
            } else {
                // If beforeNode is false, then set it to true.
                m_traversal_pointer->is_before_node = true;
            }
        }

        // NOTE: If the NodeFilter deletes the iterator's current traversal pointer,
        //       we will automatically retarget it. However, in that case, we're expected
        //       to return the node passed to the filter, not the adjusted traversal pointer's
        //       node after the filter returns!
        candidate = m_traversal_pointer->node;

        // 2. Let result be the result of filtering node within iterator.
        auto result = TRY(filter(*m_traversal_pointer->node));

        // 3. If result is FILTER_ACCEPT, then break.
        if (result == NodeFilter::Result::FILTER_ACCEPT)
            break;
    }

    // 4. Set iterator’s reference to node.
    // 5. Set iterator’s pointer before reference to beforeNode.
    m_reference = m_traversal_pointer.release_value();

    // 6. Return node.
    return candidate;
}

// https://dom.spec.whatwg.org/#concept-traversal-filter
JS::Object* NodeIterator::filter() const
{
    if (!m_filter)
        return nullptr;

    return m_filter->callback().callback;
}

// https://dom.spec.whatwg.org/#concept-node-filter
JS::ThrowCompletionOr<NodeFilter::Result> NodeIterator::filter(Node& node)
{
    // 1. If traverser’s active flag is set, then throw an "InvalidStateError" DOMException.
    if (m_active)
        return throw_completion(WebIDL::InvalidStateError::create(realm(), "NodeIterator is already active"_string));

    // 2. Let n be node’s nodeType attribute value − 1.
    auto n = node.node_type() - 1;

    // 3. If the nth bit (where 0 is the least significant bit) of traverser’s whatToShow is not set, then return FILTER_SKIP.
    if (!(m_what_to_show & (1u << n)))
        return NodeFilter::Result::FILTER_SKIP;

    // 4. If traverser’s filter is null, then return FILTER_ACCEPT.
    if (!m_filter)
        return NodeFilter::Result::FILTER_ACCEPT;

    // 5. Set traverser’s active flag.
    m_active = true;

    // 6. Let result be the return value of call a user object’s operation with traverser’s filter, "acceptNode", and « node ».
    //    If this throws an exception, then unset traverser’s active flag and rethrow the exception.
    auto result = WebIDL::call_user_object_operation(m_filter->callback(), "acceptNode"_string, {}, &node);
    if (result.is_abrupt()) {
        m_active = false;
        return result;
    }

    // 7. Unset traverser’s active flag.
    m_active = false;

    // 8. Return result.
    auto result_value = TRY(result.value()->to_i32(vm()));
    return static_cast<NodeFilter::Result>(result_value);
}

// https://dom.spec.whatwg.org/#dom-nodeiterator-nextnode
JS::ThrowCompletionOr<GC::Ptr<Node>> NodeIterator::next_node()
{
    return traverse(Direction::Next);
}

// https://dom.spec.whatwg.org/#dom-nodeiterator-previousnode
JS::ThrowCompletionOr<GC::Ptr<Node>> NodeIterator::previous_node()
{
    return traverse(Direction::Previous);
}

void NodeIterator::run_pre_removing_steps_with_node_pointer(Node& to_be_removed_node, NodePointer& pointer)
{
    // NOTE: This function tries to match the behavior of other engines, but not the DOM specification
    //       as it's a known issue that the spec doesn't match how major browsers behave.
    //       Spec bug: https://github.com/whatwg/dom/issues/907

    if (!to_be_removed_node.is_descendant_of(root()))
        return;

    if (!to_be_removed_node.is_inclusive_ancestor_of(pointer.node))
        return;

    if (pointer.is_before_node) {
        if (auto* node = to_be_removed_node.next_in_pre_order(root())) {
            while (node && node->is_descendant_of(to_be_removed_node))
                node = node->next_in_pre_order(root());
            if (node)
                pointer.node = *node;
            return;
        }
        if (auto* node = to_be_removed_node.previous_in_pre_order()) {
            if (to_be_removed_node.is_ancestor_of(pointer.node)) {
                while (node && node->is_descendant_of(to_be_removed_node))
                    node = node->previous_in_pre_order();
            }
            if (node) {
                pointer = {
                    .node = *node,
                    .is_before_node = false,
                };
            }
        }
        return;
    }

    if (auto* node = to_be_removed_node.previous_in_pre_order()) {
        if (to_be_removed_node.is_ancestor_of(pointer.node)) {
            while (node && node->is_descendant_of(to_be_removed_node))
                node = node->previous_in_pre_order();
        }
        if (node)
            pointer.node = *node;
        return;
    }
    auto* node = to_be_removed_node.next_in_pre_order(root());
    if (to_be_removed_node.is_ancestor_of(pointer.node)) {
        while (node && node->is_descendant_of(to_be_removed_node))
            node = node->previous_in_pre_order();
    }
    if (node)
        pointer.node = *node;
}

// https://dom.spec.whatwg.org/#nodeiterator-pre-removing-steps
void NodeIterator::run_pre_removing_steps(Node& to_be_removed_node)
{
    // NOTE: If we're in the middle of traversal, we have to adjust the traversal pointer in response to node removal.
    if (m_traversal_pointer.has_value())
        run_pre_removing_steps_with_node_pointer(to_be_removed_node, *m_traversal_pointer);

    run_pre_removing_steps_with_node_pointer(to_be_removed_node, m_reference);
}

}
