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
class TreeWalker final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(TreeWalker, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(TreeWalker);

public:
    static constexpr GC::MemberOffset root_offset() { return GC_FIELD_OFFSET(TreeWalker, m_tree_walker_core.traverser.root); }
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

    GC::Ref<Node> root() { return *m_tree_walker_core.traverser.root; }

    GC::Ptr<NodeFilter> filter() const;

    unsigned what_to_show() const { return m_tree_walker_core.traverser.what_to_show; }

private:
    explicit TreeWalker(Node& root);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    TraversalResult traverse(TraversalFilter const&, RustFFI::TreeWalkerTraversal);

    // https://dom.spec.whatwg.org/#concept-traversal-root
    // https://dom.spec.whatwg.org/#concept-traversal-whattoshow
    // https://dom.spec.whatwg.org/#concept-traversal-active
    // https://dom.spec.whatwg.org/#treewalker-current
    // NB: libweb_rust reads and writes these in place, see Rust/src/dom/traversal.rs.
    RustFFI::TreeWalkerCore m_tree_walker_core;

    // https://dom.spec.whatwg.org/#concept-traversal-filter
    GC::Ptr<NodeFilter> m_filter;
};

}
