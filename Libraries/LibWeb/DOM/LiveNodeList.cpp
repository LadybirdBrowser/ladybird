/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/LiveNodeList.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/HTML/HTMLLabelElement.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(LiveNodeList);

GC::Ref<NodeList> LiveNodeList::create(Node const& root, Scope scope, Filter filter, Kind kind)
{
    return GC::Heap::the().allocate<LiveNodeList>(root, scope, filter, kind);
}

GC::Ref<NodeList> LiveNodeList::create_child_nodes(Node const& root)
{
    return GC::Heap::the().allocate<LiveNodeList>(root);
}

static RustFFI::CollectionCore make_core(Node const& root, LiveNodeList::Scope scope, RustFFI::CollectionFilter filter, RustFFI::CollectionValidity validity, LiveNodeList::Kind kind)
{
    return {
        .root = const_cast<Node*>(&root),
        .cursor = nullptr,
        .cursor_index = 0,
        .length = RustFFI::COLLECTION_LENGTH_UNKNOWN,
        .tree_version = 0,
        .attribute_version = 0,
        .stamp = 0,
        .filter = filter,
        .scope = static_cast<RustFFI::CollectionScope>(to_underlying(scope)),
        .validity = validity,
        .attribute_type = 0,
        .flags = RustFFI::COLLECTION_OF_NODES,
        .cpp_state = to_underlying(kind),
        .list = nullptr,
    };
}

// NB: The filter of a list made here can read any attribute, so the list finds its nodes again whenever the tree its root
//     is in changes.
LiveNodeList::LiveNodeList(Node const& root, Scope scope, Filter filter, Kind kind)
    : GC::WeakContainer(heap())
    , m_core(make_core(root, scope, RustFFI::CollectionFilter::Host, RustFFI::CollectionValidity::DomTree, kind))
    , m_filter(filter)
{
    // NB: libweb_rust finds the core of a list at this offset into the list's cell.
    static_assert(offsetof(LiveNodeList, m_core) == RustFFI::COLLECTION_CORE_OFFSET);

    // NB: The fields libweb_rust reads start after all of the base class, where every C++ ABI places them, and not in
    //     its tail padding, where only the Itanium ABI could.
    static_assert(offsetof(LiveNodeList, m_core) >= sizeof(Bindings::GCAllocatedWrappable));
}

LiveNodeList::LiveNodeList(Node const& root)
    : GC::WeakContainer(heap())
    , m_core(make_core(root, Scope::Children, RustFFI::CollectionFilter::All, RustFFI::CollectionValidity::NodeTree, Kind::Generic))
{
}

LiveNodeList::~LiveNodeList()
{
    RustFFI::rust_dom_collection_forget_nodes(this);
}

void LiveNodeList::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    RustFFI::rust_dom_trace_collection_core(&m_core, reinterpret_cast<GCVisitor*>(&visitor));
    visitor.visit(m_filter);
}

GC::Cell const& LiveNodeList::owner_cell(Badge<GC::Heap>) const
{
    return *this;
}

// NB: The list holds the nodes it found weakly. While the versions they were found at are current, they are all in the
//     tree below the root, which it keeps alive, so one that died means the list has to find them again anyway.
void LiveNodeList::remove_dead_cells(Badge<GC::Heap>)
{
    auto is_dead = [&](Node const* node) { return node && (!heap().is_live_cell(node) || !node->is_marked()); };
    bool found_dead_node = is_dead(m_core.cursor.ptr());
    for (u32 index = 0; m_core.list && !found_dead_node && index < m_core.length; ++index)
        found_dead_node = is_dead(m_core.list[index].ptr());
    if (found_dead_node)
        RustFFI::rust_dom_collection_forget_nodes(this);
}

u64 LiveNodeList::invalidation_version(Document const& document) const
{
    switch (kind()) {
    case Kind::Generic:
        return 0;
    case Kind::Labels:
    case Kind::FormControls:
        return document.form_controls_version();
    }
    VERIFY_NOT_REACHED();
}

// A list of another kind than Generic also reads state of the document that no tree version counts, so it finds its
// nodes again whenever that changes.
void LiveNodeList::forget_nodes_if_document_state_changed() const
{
    auto invalidation_version = this->invalidation_version(m_core.root->document());
    if (invalidation_version == m_cached_kind_version)
        return;
    m_cached_kind_version = invalidation_version;
    RustFFI::rust_dom_collection_forget_nodes(this);
}

bool LiveNodeList::is_list_of_labels_of_document() const
{
    return kind() == Kind::Labels && m_core.scope == RustFFI::CollectionScope::Descendants && m_core.root->is_document();
}

// OPTIMIZATION: Only label elements can match a list of labels, and a document keeps a list of its own, so a list of
//               labels rooted at a document finds its nodes among those, rather than by walking the document.
void LiveNodeList::find_nodes_among_labels_of_document_if_needed() const
{
    if (RustFFI::rust_dom_collection_validate(this) && m_core.list)
        return;
    GC::RootVector<GC::Ref<Node>> nodes;
    for (auto const& label : as<Document>(*m_core.root).label_elements()) {
        if (auto* label_element = label.ptr().ptr(); label_element && (*m_filter)(*label_element))
            nodes.append(*label_element);
    }
    RustFFI::rust_dom_collection_remember_nodes_in_own_order(this, nodes.span());
}

Node* LiveNodeList::first_matching(Function<bool(Node const&)> const& filter) const
{
    for (u32 index = 0;; ++index) {
        auto* node = const_cast<Node*>(item(index));
        if (!node)
            return nullptr;
        if (filter(*node))
            return node;
    }
}

// https://dom.spec.whatwg.org/#dom-nodelist-length
u32 LiveNodeList::length() const
{
    forget_nodes_if_kind_changed();
    if (is_list_of_labels_of_document()) [[unlikely]]
        find_nodes_among_labels_of_document_if_needed();
    return RustFFI::rust_dom_collection_length(this);
}

// https://dom.spec.whatwg.org/#dom-nodelist-item
Node const* LiveNodeList::item(u32 index) const
{
    // The item(index) method must return the indexth node in the collection. If there is no indexth node in the collection, then the method must return null.
    if (index >= RustFFI::COLLECTION_LENGTH_UNKNOWN)
        return nullptr;
    forget_nodes_if_kind_changed();
    if (is_list_of_labels_of_document()) [[unlikely]]
        find_nodes_among_labels_of_document_if_needed();
    return RustFFI::rust_dom_collection_item(this, index);
}

}
