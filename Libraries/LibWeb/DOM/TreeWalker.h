/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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

    TraversalResult parent_node(TraversalFilter const&);
    TraversalResult first_child(TraversalFilter const&);
    TraversalResult last_child(TraversalFilter const&);
    TraversalResult previous_sibling(TraversalFilter const&);
    TraversalResult next_sibling(TraversalFilter const&);
    TraversalResult previous_node(TraversalFilter const&);
    TraversalResult next_node(TraversalFilter const&);

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
    TraversalResult traverse_children(TraversalFilter const&, ChildTraversalType);

    enum class SiblingTraversalType {
        Next,
        Previous,
    };
    TraversalResult traverse_siblings(TraversalFilter const&, SiblingTraversalType);

    TraversalFilterResult filter(TraversalFilter const&, Node&);

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
