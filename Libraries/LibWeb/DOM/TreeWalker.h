/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/NodeFilter.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#treewalker
class TreeWalker final : public Bindings::Wrappable {
    WEB_WRAPPABLE(TreeWalker, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(TreeWalker);

public:
    [[nodiscard]] static GC::Ref<TreeWalker> create(Node& root, unsigned what_to_show, GC::Ptr<NodeFilter>);

    virtual ~TreeWalker() override;

    GC::Ref<Node> current_node() const;
    void set_current_node(Node&);

    JS::ThrowCompletionOr<GC::Ptr<Node>> parent_node(JS::Realm&);
    JS::ThrowCompletionOr<GC::Ptr<Node>> first_child(JS::Realm&);
    JS::ThrowCompletionOr<GC::Ptr<Node>> last_child(JS::Realm&);
    JS::ThrowCompletionOr<GC::Ptr<Node>> previous_sibling(JS::Realm&);
    JS::ThrowCompletionOr<GC::Ptr<Node>> next_sibling(JS::Realm&);
    JS::ThrowCompletionOr<GC::Ptr<Node>> previous_node(JS::Realm&);
    JS::ThrowCompletionOr<GC::Ptr<Node>> next_node(JS::Realm&);

    GC::Ref<Node> root() { return m_root; }

    GC::Ptr<NodeFilter> filter() const;

    unsigned what_to_show() const { return m_what_to_show; }

private:
    explicit TreeWalker(Node& root);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    enum class ChildTraversalType {
        First,
        Last,
    };
    JS::ThrowCompletionOr<GC::Ptr<Node>> traverse_children(JS::Realm&, ChildTraversalType);

    enum class SiblingTraversalType {
        Next,
        Previous,
    };
    JS::ThrowCompletionOr<GC::Ptr<Node>> traverse_siblings(JS::Realm&, SiblingTraversalType);

    JS::ThrowCompletionOr<NodeFilter::Result> filter(JS::Realm&, Node&);

    // https://dom.spec.whatwg.org/#concept-traversal-root
    GC::Ref<Node> m_root;

    // https://dom.spec.whatwg.org/#treewalker-current
    GC::Ref<Node> m_current;

    // https://dom.spec.whatwg.org/#concept-traversal-whattoshow
    unsigned m_what_to_show { 0 };

    // https://dom.spec.whatwg.org/#concept-traversal-filter
    GC::Ptr<NodeFilter> m_filter;

    // https://dom.spec.whatwg.org/#concept-traversal-active
    bool m_active { false };
};

}
