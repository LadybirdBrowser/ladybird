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
    static constexpr bool OVERRIDES_FINALIZE = true;

    static GC::Ref<NodeIterator> create(JS::Realm& realm, Node& root, unsigned what_to_show, GC::Ptr<NodeFilter>);

    virtual ~NodeIterator() override;

    GC::Ref<Node> root() { return m_root; }
    GC::Ref<Node> reference_node() { return m_reference.node; }
    bool pointer_before_reference_node() const { return m_reference.pointer_before; }
    unsigned what_to_show() const { return m_what_to_show; }

    GC::Ptr<NodeFilter> filter() const;

    JS::ThrowCompletionOr<GC::Ptr<Node>> next_node();
    JS::ThrowCompletionOr<GC::Ptr<Node>> previous_node();

    void detach();

    void run_pre_removing_steps(Node&);

private:
    // https://dom.spec.whatwg.org/#node-pointer
    // A node pointer is a tuple consisting of a node (a node) and a pointer before (a boolean).
    struct NodePointer {
        GC::Ref<Node> node;
        bool pointer_before { true };
    };

    explicit NodeIterator(JS::Realm&, GC::Ref<Node> root, NodePointer);

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

    NodePointer adjust_node_pointer(NodePointer, Node& to_be_removed_node);

    // https://dom.spec.whatwg.org/#nodeiterator-reference
    NodePointer m_reference;

    // https://dom.spec.whatwg.org/#nodeiterator-candidate-reference
    Optional<NodePointer> m_candidate_reference;

    // https://dom.spec.whatwg.org/#concept-traversal-whattoshow
    unsigned m_what_to_show { 0 };

    // https://dom.spec.whatwg.org/#concept-traversal-filter
    GC::Ptr<NodeFilter> m_filter;

    // https://dom.spec.whatwg.org/#concept-traversal-active
    bool m_active { false };
};

}
