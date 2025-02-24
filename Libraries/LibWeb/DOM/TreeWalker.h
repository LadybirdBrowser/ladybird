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
    GC_DECLARE_ALLOCATOR(TreeWalker);

public:
    [[nodiscard]] static GC::Ref<TreeWalker> create(JS::Realm&, Node& root, unsigned what_to_show, GC::Ptr<NodeFilter>);

    virtual ~TreeWalker() override;

    GC::Ref<Node> current_node() const;
    void set_current_node(Node&);

    JS::ThrowCompletionOr<GC::Ptr<Node>> parent_node();
    JS::ThrowCompletionOr<GC::Ptr<Node>> first_child();
    JS::ThrowCompletionOr<GC::Ptr<Node>> last_child();
    JS::ThrowCompletionOr<GC::Ptr<Node>> previous_sibling();
    JS::ThrowCompletionOr<GC::Ptr<Node>> next_sibling();
    JS::ThrowCompletionOr<GC::Ptr<Node>> previous_node();
    JS::ThrowCompletionOr<GC::Ptr<Node>> next_node();

    GC::Ref<Node> root() { return m_root; }

    JS::Object* filter() const;

    unsigned what_to_show() const { return m_what_to_show; }

private:
    explicit TreeWalker(JS::Realm&, Node& root);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    enum class ChildTraversalType {
        First,
        Last,
    };
    JS::ThrowCompletionOr<GC::Ptr<Node>> traverse_children(ChildTraversalType);

    enum class SiblingTraversalType {
        Next,
        Previous,
    };
    JS::ThrowCompletionOr<GC::Ptr<Node>> traverse_siblings(SiblingTraversalType);

    JS::ThrowCompletionOr<NodeFilter::Result> filter(Node&);

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
