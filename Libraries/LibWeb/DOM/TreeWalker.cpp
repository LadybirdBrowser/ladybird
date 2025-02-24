/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TreeWalkerPrototype.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NodeFilter.h>
#include <LibWeb/DOM/TreeWalker.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(TreeWalker);

TreeWalker::TreeWalker(JS::Realm& realm, Node& root)
    : PlatformObject(realm)
    , m_root(root)
    , m_current(root)
{
}

TreeWalker::~TreeWalker() = default;

void TreeWalker::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TreeWalker);
}

void TreeWalker::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_filter);
    visitor.visit(m_root);
    visitor.visit(m_current);
}

// https://dom.spec.whatwg.org/#dom-document-createtreewalker
GC::Ref<TreeWalker> TreeWalker::create(JS::Realm& realm, Node& root, unsigned what_to_show, GC::Ptr<NodeFilter> filter)
{
    // 1. Let walker be a new TreeWalker object.
    // 2. Set walker’s root and walker’s current to root.
    auto walker = realm.create<TreeWalker>(realm, root);

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
JS::ThrowCompletionOr<GC::Ptr<Node>> TreeWalker::parent_node()
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
            auto result = TRY(filter(*node));
            if (result == NodeFilter::Result::FILTER_ACCEPT) {
                m_current = *node;
                return node;
            }
        }
    }

    return nullptr;
}

// https://dom.spec.whatwg.org/#dom-treewalker-firstchild
JS::ThrowCompletionOr<GC::Ptr<Node>> TreeWalker::first_child()
{
    return traverse_children(ChildTraversalType::First);
}

// https://dom.spec.whatwg.org/#dom-treewalker-lastchild
JS::ThrowCompletionOr<GC::Ptr<Node>> TreeWalker::last_child()
{
    return traverse_children(ChildTraversalType::Last);
}

// https://dom.spec.whatwg.org/#dom-treewalker-previoussibling
JS::ThrowCompletionOr<GC::Ptr<Node>> TreeWalker::previous_sibling()
{
    return traverse_siblings(SiblingTraversalType::Previous);
}

// https://dom.spec.whatwg.org/#dom-treewalker-nextsibling
JS::ThrowCompletionOr<GC::Ptr<Node>> TreeWalker::next_sibling()
{
    return traverse_siblings(SiblingTraversalType::Next);
}

// https://dom.spec.whatwg.org/#dom-treewalker-previousnode
JS::ThrowCompletionOr<GC::Ptr<Node>> TreeWalker::previous_node()
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
            auto result = TRY(filter(*node));

            // 3. While result is not FILTER_REJECT and node has a child:
            while (result != NodeFilter::Result::FILTER_REJECT && node->has_children()) {
                // 1. Set node to node’s last child.
                node = *node->last_child();

                // 2. Set result to the result of filtering node within this.
                result = TRY(filter(*node));
            }

            // 4. If result is FILTER_ACCEPT, then set this’s current to node and return node.
            if (result == NodeFilter::Result::FILTER_ACCEPT) {
                m_current = node;
                return node;
            }

            // 5. Set sibling to node’s previous sibling.
            sibling = node->previous_sibling();
        }

        // 3. If node is this’s root or node’s parent is null, then return null.
        if (node == m_root || !node->parent())
            return nullptr;

        // 4. Set node to node’s parent.
        node = *node->parent();

        // 5. If the return value of filtering node within this is FILTER_ACCEPT, then set this’s current to node and return node.
        if (TRY(filter(*node)) == NodeFilter::Result::FILTER_ACCEPT) {
            m_current = node;
            return node;
        }
    }
    // 3. Return null.
    return nullptr;
}

// https://dom.spec.whatwg.org/#dom-treewalker-nextnode
JS::ThrowCompletionOr<GC::Ptr<Node>> TreeWalker::next_node()
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
            result = TRY(filter(*node));

            // 3. If result is FILTER_ACCEPT, then set this’s current to node and return node.
            if (result == NodeFilter::Result::FILTER_ACCEPT) {
                m_current = *node;
                return node;
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
                return nullptr;

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
                return nullptr;
            }
        }

        // 5. Set result to the result of filtering node within this.
        result = TRY(filter(*node));

        // 6. If result is FILTER_ACCEPT, then set this’s current to node and return node.
        if (result == NodeFilter::Result::FILTER_ACCEPT) {
            m_current = *node;
            return node;
        }
    }
}

// https://dom.spec.whatwg.org/#concept-traversal-filter
JS::Object* TreeWalker::filter() const
{
    if (!m_filter)
        return nullptr;

    return m_filter->callback().callback;
}

// https://dom.spec.whatwg.org/#concept-node-filter
JS::ThrowCompletionOr<NodeFilter::Result> TreeWalker::filter(Node& node)
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

// https://dom.spec.whatwg.org/#concept-traverse-children
JS::ThrowCompletionOr<GC::Ptr<Node>> TreeWalker::traverse_children(ChildTraversalType type)
{
    // 1. Let node be walker’s current.
    GC::Ptr<Node> node = m_current;

    // 2. Set node to node’s first child if type is first, and node’s last child if type is last.
    node = type == ChildTraversalType::First ? node->first_child() : node->last_child();

    // 3. While node is non-null:
    while (node) {
        // 1. Let result be the result of filtering node within walker.
        auto result = TRY(filter(*node));

        // 2. If result is FILTER_ACCEPT, then set walker’s current to node and return node.
        if (result == NodeFilter::Result::FILTER_ACCEPT) {
            m_current = *node;
            return node;
        }

        // 3. If result is FILTER_SKIP, then:
        if (result == NodeFilter::Result::FILTER_SKIP) {
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
                return nullptr;

            // 5. Set node to parent.
            node = parent;
        }
    }

    // 4. Return null.
    return nullptr;
}

// https://dom.spec.whatwg.org/#concept-traverse-siblings
JS::ThrowCompletionOr<GC::Ptr<Node>> TreeWalker::traverse_siblings(SiblingTraversalType type)
{
    // 1. Let node be walker’s current.
    GC::Ptr<Node> node = m_current;

    // 2. If node is root, then return null.
    if (node == m_root)
        return nullptr;

    // 3. While true:
    while (true) {
        // 1. Let sibling be node’s next sibling if type is next, and node’s previous sibling if type is previous.
        GC::Ptr<Node> sibling = type == SiblingTraversalType::Next ? node->next_sibling() : node->previous_sibling();

        // 2. While sibling is non-null:
        while (sibling) {
            // 1. Set node to sibling.
            node = sibling;

            // 2. Let result be the result of filtering node within walker.
            auto result = TRY(filter(*node));

            // 3. If result is FILTER_ACCEPT, then set walker’s current to node and return node.
            if (result == NodeFilter::Result::FILTER_ACCEPT) {
                m_current = *node;
                return node;
            }

            // 4. Set sibling to node’s first child if type is next, and node’s last child if type is previous.
            sibling = type == SiblingTraversalType::Next ? node->first_child() : node->last_child();

            // 5. If result is FILTER_REJECT or sibling is null, then set sibling to node’s next sibling if type is next, and node’s previous sibling if type is previous.
            if (result == NodeFilter::Result::FILTER_REJECT || !sibling)
                sibling = type == SiblingTraversalType::Next ? node->next_sibling() : node->previous_sibling();
        }

        // 3. Set node to node’s parent.
        node = node->parent();

        // 4. If node is null or walker’s root, then return null.
        if (!node || node == m_root)
            return nullptr;

        // 5. If the return value of filtering node within walker is FILTER_ACCEPT, then return null.
        if (TRY(filter(*node)) == NodeFilter::Result::FILTER_ACCEPT)
            return nullptr;
    }
}

}
