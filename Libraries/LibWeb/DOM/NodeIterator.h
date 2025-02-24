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
    GC_DECLARE_ALLOCATOR(NodeIterator);

public:
    static GC::Ref<NodeIterator> create(JS::Realm& realm, Node& root, unsigned what_to_show, GC::Ptr<NodeFilter>);

    virtual ~NodeIterator() override;

    GC::Ref<Node> root() { return m_root; }
    GC::Ref<Node> reference_node() { return m_reference.node; }
    bool pointer_before_reference_node() const { return m_reference.is_before_node; }
    unsigned what_to_show() const { return m_what_to_show; }

    JS::Object* filter() const;

    JS::ThrowCompletionOr<GC::Ptr<Node>> next_node();
    JS::ThrowCompletionOr<GC::Ptr<Node>> previous_node();

    void detach();

    void run_pre_removing_steps(Node&);

private:
    explicit NodeIterator(JS::Realm&, Node& root);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    enum class Direction {
        Next,
        Previous,
    };

    JS::ThrowCompletionOr<GC::Ptr<Node>> traverse(Direction);

    JS::ThrowCompletionOr<NodeFilter::Result> filter(Node&);

    // https://dom.spec.whatwg.org/#concept-traversal-root
    GC::Ref<Node> m_root;

    struct NodePointer {
        GC::Ref<Node> node;

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
    GC::Ptr<NodeFilter> m_filter;

    // https://dom.spec.whatwg.org/#concept-traversal-active
    bool m_active { false };
};

}
