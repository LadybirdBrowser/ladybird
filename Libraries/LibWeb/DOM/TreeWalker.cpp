/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NodeFilter.h>
#include <LibWeb/DOM/TreeWalker.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(TreeWalker);

TreeWalker::TreeWalker(Node& root)
    : m_tree_walker_core {
        .traverser = { .root = root, .what_to_show = 0, .flags = 0 },
        .current = root,
    }
{
    // NB: libweb_rust reads the core of a TreeWalker in place.
    static_assert(offsetof(TreeWalker, m_tree_walker_core) == RustFFI::TRAVERSER_CORE_OFFSET);

    // NB: The fields libweb_rust reads start after all of the base class, where every C++ ABI places them, and not in
    //     its tail padding, where only the Itanium ABI could.
    static_assert(offsetof(TreeWalker, m_tree_walker_core) >= sizeof(Bindings::GCAllocatedWrappable));
}

// NB: The Rust definition of the core asserts the same layout.
static_assert(sizeof(RustFFI::TreeWalkerCore) == 16 && alignof(RustFFI::TreeWalkerCore) == 4);
static_assert(offsetof(RustFFI::TreeWalkerCore, current) == 12);

TreeWalker::~TreeWalker() = default;

void TreeWalker::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_filter);
    RustFFI::rust_dom_trace_tree_walker_core(&m_tree_walker_core, reinterpret_cast<GCVisitor*>(&visitor));
}

// https://dom.spec.whatwg.org/#dom-document-createtreewalker
GC::Ref<TreeWalker> TreeWalker::create(Node& root, unsigned what_to_show, GC::Ptr<NodeFilter> filter)
{
    // 1. Let walker be a new TreeWalker object.
    // 2. Set walker’s root and walker’s current to root.
    auto walker = GC::Heap::the().allocate<TreeWalker>(root);

    // 3. Set walker’s whatToShow to whatToShow.
    walker->m_tree_walker_core.traverser.what_to_show = what_to_show;

    // 4. Set walker’s filter to filter.
    walker->m_filter = filter;

    // 5. Return walker.
    return walker;
}

// https://dom.spec.whatwg.org/#dom-treewalker-currentnode
GC::Ref<Node> TreeWalker::current_node() const
{
    return *m_tree_walker_core.current;
}

// https://dom.spec.whatwg.org/#dom-treewalker-currentnode
void TreeWalker::set_current_node(Node& node)
{
    m_tree_walker_core.current = node;
}

// https://dom.spec.whatwg.org/#concept-traversal-filter
GC::Ptr<NodeFilter> TreeWalker::filter() const
{
    return m_filter;
}

TraversalResult TreeWalker::traverse(TraversalFilter const& traversal_filter, RustFFI::TreeWalkerTraversal traversal)
{
    // NB: libweb_rust runs the steps, see Rust/src/dom/traversal.rs. It calls the filter through the traversal filter,
    //     which it is handed only if the filter is non-null.
    return TraversalResult::from_rust(RustFFI::rust_dom_tree_walker_traverse(this, m_filter ? &traversal_filter : nullptr, traversal));
}

// https://dom.spec.whatwg.org/#dom-treewalker-parentnode
TraversalResult TreeWalker::parent_node(TraversalFilter const& traversal_filter)
{
    return traverse(traversal_filter, RustFFI::TreeWalkerTraversal::ParentNode);
}

// https://dom.spec.whatwg.org/#dom-treewalker-firstchild
TraversalResult TreeWalker::first_child(TraversalFilter const& traversal_filter)
{
    return traverse(traversal_filter, RustFFI::TreeWalkerTraversal::FirstChild);
}

// https://dom.spec.whatwg.org/#dom-treewalker-lastchild
TraversalResult TreeWalker::last_child(TraversalFilter const& traversal_filter)
{
    return traverse(traversal_filter, RustFFI::TreeWalkerTraversal::LastChild);
}

// https://dom.spec.whatwg.org/#dom-treewalker-previoussibling
TraversalResult TreeWalker::previous_sibling(TraversalFilter const& traversal_filter)
{
    return traverse(traversal_filter, RustFFI::TreeWalkerTraversal::PreviousSibling);
}

// https://dom.spec.whatwg.org/#dom-treewalker-nextsibling
TraversalResult TreeWalker::next_sibling(TraversalFilter const& traversal_filter)
{
    return traverse(traversal_filter, RustFFI::TreeWalkerTraversal::NextSibling);
}

// https://dom.spec.whatwg.org/#dom-treewalker-previousnode
TraversalResult TreeWalker::previous_node(TraversalFilter const& traversal_filter)
{
    return traverse(traversal_filter, RustFFI::TreeWalkerTraversal::PreviousNode);
}

// https://dom.spec.whatwg.org/#dom-treewalker-nextnode
TraversalResult TreeWalker::next_node(TraversalFilter const& traversal_filter)
{
    return traverse(traversal_filter, RustFFI::TreeWalkerTraversal::NextNode);
}

}
