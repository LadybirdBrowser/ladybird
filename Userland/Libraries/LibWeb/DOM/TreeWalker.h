/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/NodeFilter.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#treewalker
class TreeWalker final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TreeWalker, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(TreeWalker);

public:
    [[nodiscard]] static JS::NonnullGCPtr<TreeWalker> create(Node& root, unsigned what_to_show, JS::GCPtr<NodeFilter>);

    virtual ~TreeWalker() override;

    JS::NonnullGCPtr<Node> current_node() const;
    void set_current_node(Node&);

    JS::ThrowCompletionOr<JS::GCPtr<Node>> parent_node();
    JS::ThrowCompletionOr<JS::GCPtr<Node>> first_child();
    JS::ThrowCompletionOr<JS::GCPtr<Node>> last_child();
    JS::ThrowCompletionOr<JS::GCPtr<Node>> previous_sibling();
    JS::ThrowCompletionOr<JS::GCPtr<Node>> next_sibling();
    JS::ThrowCompletionOr<JS::GCPtr<Node>> previous_node();
    JS::ThrowCompletionOr<JS::GCPtr<Node>> next_node();

    JS::NonnullGCPtr<Node> root() { return m_root; }

    NodeFilter* filter() { return m_filter.ptr(); }

    unsigned what_to_show() const { return m_what_to_show; }

private:
    explicit TreeWalker(Node& root);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    enum class ChildTraversalType {
        First,
        Last,
    };
    JS::ThrowCompletionOr<JS::GCPtr<Node>> traverse_children(ChildTraversalType);

    enum class SiblingTraversalType {
        Next,
        Previous,
    };
    JS::ThrowCompletionOr<JS::GCPtr<Node>> traverse_siblings(SiblingTraversalType);

    JS::ThrowCompletionOr<NodeFilter::Result> filter(Node&);

    // https://dom.spec.whatwg.org/#concept-traversal-root
    JS::NonnullGCPtr<Node> m_root;

    // https://dom.spec.whatwg.org/#treewalker-current
    JS::NonnullGCPtr<Node> m_current;

    // https://dom.spec.whatwg.org/#concept-traversal-whattoshow
    unsigned m_what_to_show { 0 };

    // https://dom.spec.whatwg.org/#concept-traversal-filter
    JS::GCPtr<NodeFilter> m_filter;

    // https://dom.spec.whatwg.org/#concept-traversal-active
    bool m_active { false };
};

}
