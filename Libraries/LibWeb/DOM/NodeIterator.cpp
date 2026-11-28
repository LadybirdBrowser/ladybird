/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NodeIterator.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(NodeIterator);

NodeIterator::NodeIterator(Node& root)
    : m_node_iterator_core {
        .traverser = { .root = root, .what_to_show = 0, .flags = RustFFI::NODE_ITERATOR_POINTER_BEFORE_REFERENCE },
        .reference = root,
        .candidate_reference = nullptr,
    }
{
    // NB: libweb_rust reads the core of a NodeIterator in place.
    static_assert(offsetof(NodeIterator, m_node_iterator_core) == RustFFI::TRAVERSER_CORE_OFFSET);

    // NB: The fields libweb_rust reads start after all of the base class, where every C++ ABI places them, and not in
    //     its tail padding, where only the Itanium ABI could.
    static_assert(offsetof(NodeIterator, m_node_iterator_core) >= sizeof(Bindings::GCAllocatedWrappable));

    root.document().register_node_iterator({}, *this);
}

// NB: The Rust definitions of the cores assert the same layout.
static_assert(sizeof(RustFFI::TraverserCore) == 12 && alignof(RustFFI::TraverserCore) == 4);
static_assert(offsetof(RustFFI::TraverserCore, root) == 0);
static_assert(offsetof(RustFFI::TraverserCore, what_to_show) == 4);
static_assert(offsetof(RustFFI::TraverserCore, flags) == 8);
static_assert(sizeof(RustFFI::NodeIteratorCore) == 20 && alignof(RustFFI::NodeIteratorCore) == 4);
static_assert(offsetof(RustFFI::NodeIteratorCore, reference) == 12);
static_assert(offsetof(RustFFI::NodeIteratorCore, candidate_reference) == 16);

NodeIterator::~NodeIterator() = default;

void NodeIterator::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_filter);
    visitor.visit(m_node_iterator_core.traverser.root);
    visitor.visit(m_node_iterator_core.reference);
    visitor.visit(m_node_iterator_core.candidate_reference);
}

// https://dom.spec.whatwg.org/#dom-document-createnodeiterator
GC::Ref<NodeIterator> NodeIterator::create(Node& root, unsigned what_to_show, GC::Ptr<NodeFilter> filter)
{
    // 1. Let iterator be a new NodeIterator object.
    // 2. Set iterator’s root and iterator’s reference to root.
    // 3. Set iterator’s pointer before reference to true.
    auto iterator = GC::Heap::the().allocate<NodeIterator>(root);

    // 4. Set iterator’s whatToShow to whatToShow.
    iterator->m_node_iterator_core.traverser.what_to_show = what_to_show;

    // 5. Set iterator’s filter to filter.
    iterator->m_filter = filter;

    // 6. Return iterator.
    return iterator;
}

// https://dom.spec.whatwg.org/#dom-nodeiterator-detach
void NodeIterator::detach()
{
    // The detach() method steps are to do nothing.
    // Its functionality (disabling a NodeIterator object) was removed, but the method itself is preserved for compatibility.
}

// https://dom.spec.whatwg.org/#concept-nodeiterator-traverse
TraversalResult NodeIterator::traverse(TraversalFilter const& traversal_filter, RustFFI::NodeIteratorDirection direction)
{
    // NB: libweb_rust runs the steps, see Rust/src/dom/traversal.rs. It calls the filter through the traversal filter,
    //     which it is handed only if the filter is non-null.
    return TraversalResult::from_rust(RustFFI::rust_dom_node_iterator_traverse(this, m_filter ? &traversal_filter : nullptr, direction));
}

// https://dom.spec.whatwg.org/#concept-traversal-filter
GC::Ptr<NodeFilter> NodeIterator::filter() const
{
    return m_filter;
}

// https://dom.spec.whatwg.org/#dom-nodeiterator-nextnode
TraversalResult NodeIterator::next_node(TraversalFilter const& traversal_filter)
{
    return traverse(traversal_filter, RustFFI::NodeIteratorDirection::Next);
}

// https://dom.spec.whatwg.org/#dom-nodeiterator-previousnode
TraversalResult NodeIterator::previous_node(TraversalFilter const& traversal_filter)
{
    return traverse(traversal_filter, RustFFI::NodeIteratorDirection::Previous);
}

}
