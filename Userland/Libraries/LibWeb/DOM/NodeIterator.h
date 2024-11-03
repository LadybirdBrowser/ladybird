/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>
#include <LibWeb/DOM/NodeFilter.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#nodeiterator
class NodeIterator final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(NodeIterator, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(NodeIterator);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<NodeIterator>> create(Node& root, unsigned what_to_show, JS::GCPtr<NodeFilter>);

    virtual ~NodeIterator() override;

    JS::NonnullGCPtr<Node> root() { return m_root; }
    JS::NonnullGCPtr<Node> reference_node() { return m_reference.node; }
    bool pointer_before_reference_node() const { return m_reference.is_before_node; }
    unsigned what_to_show() const { return m_what_to_show; }

    NodeFilter* filter() { return m_filter.ptr(); }

    JS::ThrowCompletionOr<JS::GCPtr<Node>> next_node();
    JS::ThrowCompletionOr<JS::GCPtr<Node>> previous_node();

    void detach();

    void run_pre_removing_steps(Node&);

private:
    explicit NodeIterator(Node& root);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    enum class Direction {
        Next,
        Previous,
    };

    JS::ThrowCompletionOr<JS::GCPtr<Node>> traverse(Direction);

    JS::ThrowCompletionOr<NodeFilter::Result> filter(Node&);

    // https://dom.spec.whatwg.org/#concept-traversal-root
    JS::NonnullGCPtr<Node> m_root;

    struct NodePointer {
        JS::NonnullGCPtr<Node> node;

        // https://dom.spec.whatwg.org/#nodeiterator-pointer-before-reference
        bool is_before_node { true };
    };

    void run_pre_removing_steps_with_node_pointer(Node&, NodePointer&);

    // https://dom.spec.whatwg.org/#nodeiterator-reference
    NodePointer m_reference;

    // While traversal is ongoing, we keep track of the current node pointer.
    // This allows us to adjust it during traversal if calling the filter ends up removing the node from the DOM.
    Optional<NodePointer> m_traversal_pointer;

    // https://dom.spec.whatwg.org/#concept-traversal-whattoshow
    unsigned m_what_to_show { 0 };

    // https://dom.spec.whatwg.org/#concept-traversal-filter
    JS::GCPtr<NodeFilter> m_filter;

    // https://dom.spec.whatwg.org/#concept-traversal-active
    bool m_active { false };
};

}
