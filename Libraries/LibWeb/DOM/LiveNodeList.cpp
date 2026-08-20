/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Runtime/Error.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/LiveNodeList.h>
#include <LibWeb/DOM/Node.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(LiveNodeList);

GC::Ref<NodeList> LiveNodeList::create(Node const& root, Scope scope, Function<bool(Node const&)> filter, Kind kind)
{
    return GC::Heap::the().allocate<LiveNodeList>(root, scope, move(filter), kind);
}

LiveNodeList::LiveNodeList(Node const& root, Scope scope, Function<bool(Node const&)> filter, Kind kind)
    : NodeList()
    , GC::WeakContainer(heap())
    , m_root(root)
    , m_filter(move(filter))
    , m_scope(scope)
    , m_kind(kind)
{
}

LiveNodeList::~LiveNodeList() = default;

void LiveNodeList::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_root);
    visitor.visit_possible_values(m_filter.raw_capture_range());
}

GC::Cell const& LiveNodeList::owner_cell(Badge<GC::Heap>) const
{
    return *this;
}

void LiveNodeList::remove_dead_cells(Badge<GC::Heap>)
{
    m_cached_nodes.remove_all_matching([&](GC::RawPtr<Node> const& node) {
        auto* block = GC::HeapBlock::from_cell(node.ptr());
        return !heap().is_live_heap_block(block) || node->state() != Cell::State::Live || !node->is_marked();
    });
}

void LiveNodeList::update_cache_if_needed() const
{
    auto& document = m_root->document();
    auto invalidation_version = this->invalidation_version(document);
    if (m_cached_document.ptr().ptr() == &document
        && m_cached_dom_tree_version == document.dom_tree_version()
        && m_cached_invalidation_version == invalidation_version) {
        return;
    }

    m_cached_nodes.clear();
    if (m_scope == Scope::Descendants) {
        m_root->for_each_in_subtree([&](auto& node) {
            if (m_filter(node))
                m_cached_nodes.append(const_cast<Node&>(node));
            return TraversalDecision::Continue;
        });
    } else {
        m_root->for_each_child([&](auto& node) {
            if (m_filter(node))
                m_cached_nodes.append(const_cast<Node&>(node));
            return IterationDecision::Continue;
        });
    }

    m_cached_document = document;
    m_cached_dom_tree_version = document.dom_tree_version();
    m_cached_invalidation_version = invalidation_version;
}

u64 LiveNodeList::invalidation_version(Document const& document) const
{
    switch (m_kind) {
    case Kind::Generic:
        return 0;
    case Kind::Labels:
    case Kind::FormControls:
        return document.form_associated_custom_element_version();
    }
    VERIFY_NOT_REACHED();
}

Node* LiveNodeList::first_matching(Function<bool(Node const&)> const& filter) const
{
    update_cache_if_needed();
    for (auto& node : m_cached_nodes) {
        if (filter(*node))
            return node.ptr();
    }
    return nullptr;
}

// https://dom.spec.whatwg.org/#dom-nodelist-length
u32 LiveNodeList::length() const
{
    update_cache_if_needed();
    return m_cached_nodes.size();
}

// https://dom.spec.whatwg.org/#dom-nodelist-item
Node const* LiveNodeList::item(u32 index) const
{
    // The item(index) method must return the indexth node in the collection. If there is no indexth node in the collection, then the method must return null.
    update_cache_if_needed();
    if (index >= m_cached_nodes.size())
        return nullptr;
    return m_cached_nodes[index].ptr();
}

}
