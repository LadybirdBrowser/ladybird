/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/NodeFilter.h>
#include <LibWeb/DOM/RustFFI.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#nodeiterator
class NodeIterator final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(NodeIterator, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(NodeIterator);

public:
    static constexpr GC::MemberOffset root_offset() { return GC_FIELD_OFFSET(NodeIterator, m_node_iterator_core.traverser.root); }

    static GC::Ref<NodeIterator> create(Node& root, unsigned what_to_show, GC::Ptr<NodeFilter>);

    virtual ~NodeIterator() override;

    GC::Ref<Node> root() { return *m_node_iterator_core.traverser.root; }
    GC::Ref<Node> reference_node() { return *m_node_iterator_core.reference; }
    bool pointer_before_reference_node() const { return m_node_iterator_core.traverser.flags & RustFFI::NODE_ITERATOR_POINTER_BEFORE_REFERENCE; }
    unsigned what_to_show() const { return m_node_iterator_core.traverser.what_to_show; }

    GC::Ptr<NodeFilter> filter() const;

    TraversalResult next_node(TraversalFilter const&);
    TraversalResult previous_node(TraversalFilter const&);

    void detach();

private:
    explicit NodeIterator(Node& root);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    TraversalResult traverse(TraversalFilter const&, RustFFI::NodeIteratorDirection);

    // https://dom.spec.whatwg.org/#concept-traversal-root
    // https://dom.spec.whatwg.org/#concept-traversal-whattoshow
    // https://dom.spec.whatwg.org/#concept-traversal-active
    // https://dom.spec.whatwg.org/#nodeiterator-reference
    // https://dom.spec.whatwg.org/#nodeiterator-candidate-reference
    // NB: libweb_rust reads and writes these in place, see Rust/src/dom/traversal.rs.
    RustFFI::NodeIteratorCore m_node_iterator_core;

    // https://dom.spec.whatwg.org/#concept-traversal-filter
    GC::Ptr<NodeFilter> m_filter;

    // NB: The index of the iterator's entry in the NodeIterators of its root's node document, see WeakCellRegistry.
    template<typename>
    friend class WeakCellRegistry;
    size_t& weak_cell_registry_index() { return m_weak_cell_registry_index; }
    size_t m_weak_cell_registry_index { 0 };
};

}
