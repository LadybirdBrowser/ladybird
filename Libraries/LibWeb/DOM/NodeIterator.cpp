/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/NodeIterator.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NodeIterator.h>
#include <LibWeb/WebIDL/AbstractOperations.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(NodeIterator);

NodeIterator::NodeIterator(JS::Realm& realm, GC::Ref<Node> root, NodePointer reference)
    : PlatformObject(realm)
    , m_root(root)
    , m_reference(reference)
{
    m_root->document().register_node_iterator({}, *this);
}

NodeIterator::~NodeIterator() = default;

void NodeIterator::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(NodeIterator);
    Base::initialize(realm);
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

    if (m_candidate_reference.has_value())
        visitor.visit(m_candidate_reference->node);
}

// https://dom.spec.whatwg.org/#dom-document-createnodeiterator
GC::Ref<NodeIterator> NodeIterator::create(JS::Realm& realm, Node& root, unsigned what_to_show, GC::Ptr<NodeFilter> filter)
{
    // 1. Let iterator be a new NodeIterator object.
    // 2. Set iterator’s root to root.
    // 3. Set iterator’s reference to (root, true).
    auto iterator = realm.create<NodeIterator>(realm, root, NodePointer { root, true });

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
    // 1. Set iterator’s candidate reference to iterator’s reference.
    m_candidate_reference = m_reference;

    // 2. Let result be null.
    GC::Ptr<Node> result;

    // 3. While true:
    while (true) {
        // 1. If type is "next":
        if (direction == Direction::Next) {
            // 1. If iterator’s candidate reference’s pointer before is false:
            if (!m_candidate_reference->pointer_before) {
                // 1. Let following be the first node following candidate reference’s node in iterator’s iterator collection.
                //    If there is no such node, then break.
                auto* following = m_candidate_reference->node->next_in_pre_order(m_root);
                if (!following)
                    break;

                // 2. Set candidate reference to (following, false).
                m_candidate_reference = { *following, false };
            }
            // 2. Otherwise, set candidate reference to (candidate reference’s node, false).
            else {
                m_candidate_reference = { m_candidate_reference->node, false };
            }
        }
        // 2. Otherwise:
        else {
            // 1. If iterator’s candidate reference’s pointer before is true:
            if (m_candidate_reference->pointer_before) {
                // 1. Let preceding be the first node preceding candidate reference’s node in iterator’s iterator collection.
                //    If there is no such node, then break.
                if (m_candidate_reference->node == m_root)
                    break;
                auto* preceding = m_candidate_reference->node->previous_in_pre_order();
                if (!preceding)
                    break;

                // 2. Set iterator’s candidate reference to (preceding, true).
                m_candidate_reference = { *preceding, true };
            }
            // 2. Otherwise, set candidate reference to (candidate reference’s node, true).
            else {
                m_candidate_reference = { m_candidate_reference->node, true };
            }
        }

        // 3. Let node be candidate reference’s node.
        GC::Ref<Node> node = m_candidate_reference->node;

        // 4. Let filterResult be the result of filtering node within iterator. If this throws an exception,
        //    then set iterator’s candidate reference to null and rethrow that exception.
        auto filter_result = filter(node);
        if (filter_result.is_error()) {
            m_candidate_reference.clear();
            return filter_result.release_error();
        }

        // 5. If filterResult is FILTER_ACCEPT:
        if (filter_result.value() == NodeFilter::Result::FILTER_ACCEPT) {
            // 1. Set iterator’s reference to iterator’s candidate reference.
            m_reference = *m_candidate_reference;

            // 2. Set result to node.
            result = node;

            // 3. Break.
            break;
        }
    }

    // 4. Set iterator’s candidate reference to null.
    m_candidate_reference.clear();

    // 5. Return result.
    return result;
}

// https://dom.spec.whatwg.org/#concept-traversal-filter
GC::Ptr<NodeFilter> NodeIterator::filter() const
{
    return m_filter;
}

// https://dom.spec.whatwg.org/#concept-node-filter
JS::ThrowCompletionOr<NodeFilter::Result> NodeIterator::filter(Node& node)
{
    // 1. If traverser’s active flag is set, then throw an "InvalidStateError" DOMException.
    if (m_active)
        return throw_completion(WebIDL::InvalidStateError::create(realm(), "NodeIterator is already active"_utf16));

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
    auto result = WebIDL::call_user_object_operation(m_filter->callback(), "acceptNode"_utf16_fly_string, {}, { { &node } });
    if (result.is_abrupt()) {
        m_active = false;
        return result;
    }

    // 7. Unset traverser’s active flag.
    m_active = false;

    // 8. Return result.
    auto result_value = TRY(result.value().to_i32(vm()));
    return static_cast<NodeFilter::Result>(result_value);
}

// https://dom.spec.whatwg.org/#dom-nodeiterator-nextnode
JS::ThrowCompletionOr<GC::Ptr<Node>> NodeIterator::next_node()
{
    // The nextNode() method steps are to return the result of traversing with this and "next".
    return traverse(Direction::Next);
}

// https://dom.spec.whatwg.org/#dom-nodeiterator-previousnode
JS::ThrowCompletionOr<GC::Ptr<Node>> NodeIterator::previous_node()
{
    // The previousNode() method steps are to return the result of traversing with this and "previous".
    return traverse(Direction::Previous);
}

// https://dom.spec.whatwg.org/#nodeiterator-adjust
NodeIterator::NodePointer NodeIterator::adjust_node_pointer(NodePointer node_pointer, Node& to_be_removed_node)
{
    // 1. If toBeRemovedNode is not an inclusive ancestor of nodePointer’s node, or toBeRemovedNode is an inclusive
    //    ancestor of nodeIterator’s root, then return nodePointer.
    if (!to_be_removed_node.is_inclusive_ancestor_of(node_pointer.node) || to_be_removed_node.is_inclusive_ancestor_of(root()))
        return node_pointer;

    // 2. If nodePointer’s pointer before is true:
    if (node_pointer.pointer_before) {
        // 1. Let next be toBeRemovedNode’s first following node that is an inclusive descendant of nodeIterator’s root
        //    and is not an inclusive descendant of toBeRemovedNode, if there is such a node; otherwise null.
        auto* next = to_be_removed_node.next_in_pre_order(root());
        while (next && next->is_descendant_of(to_be_removed_node))
            next = next->next_in_pre_order(root());

        // 2. If next is non-null, then return (next, true).
        if (next)
            return { *next, true };
    }

    // 3. Let newNode be toBeRemovedNode’s parent, if toBeRemovedNode’s previous sibling is null; otherwise the
    //    inclusive descendant of toBeRemovedNode’s previous sibling that appears last in tree order.
    // NOTE: This is exactly toBeRemovedNode’s previous node in pre-order. It is never null here, because step 1
    //       guarantees toBeRemovedNode is a proper descendant of the root and thus has a parent.
    auto* new_node = to_be_removed_node.previous_in_pre_order();
    VERIFY(new_node);

    // 4. Return (newNode, false).
    return { *new_node, false };
}

// https://dom.spec.whatwg.org/#nodeiterator-pre-removing-steps
void NodeIterator::run_pre_removing_steps(Node& to_be_removed_node)
{
    // 1. Set nodeIterator’s reference to the result of adjusting a node pointer given nodeIterator’s reference,
    //    nodeIterator, and toBeRemovedNode.
    m_reference = adjust_node_pointer(m_reference, to_be_removed_node);

    // 2. If nodeIterator’s candidate reference is non-null, then set nodeIterator’s candidate reference to the result
    //    of adjusting a node pointer given nodeIterator’s candidate reference, nodeIterator, and toBeRemovedNode.
    if (m_candidate_reference.has_value())
        m_candidate_reference = adjust_node_pointer(*m_candidate_reference, to_be_removed_node);
}

}
