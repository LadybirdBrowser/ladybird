/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibGC/Function.h>
#include <LibGC/WeakContainer.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/DOM/RustFFI.h>

namespace Web::DOM {

class Document;

// NB: libweb_rust walks the subtree and keeps the list's state, see Rust/src/dom/collection.rs.
class LiveNodeList
    : public NodeList
    , public GC::WeakContainer {
    WEB_NON_IDL_WRAPPABLE(LiveNodeList, NodeList);
    GC_DECLARE_ALLOCATOR(LiveNodeList);

public:
    enum class Scope : u8 {
        Children = to_underlying(RustFFI::CollectionScope::Children),
        Descendants = to_underlying(RustFFI::CollectionScope::Descendants),
    };

    enum class Kind : u8 {
        Generic,
        Labels,
        FormControls,
    };

    using Filter = GC::Ref<GC::Function<bool(Node const&)>>;

    [[nodiscard]] static GC::Ref<NodeList> create(Node const& root, Scope, Filter, Kind = Kind::Generic);
    // A list of the children of root, as Node::child_nodes() returns it.
    [[nodiscard]] static GC::Ref<NodeList> create_child_nodes(Node const& root);
    // NB: The core forgets the nodes the collection found when the collection is swept, not when it is finalized, as
    //     the C++ members that held them did, so that the finalizer of another dead cell can still read them.
    GC_ALLOW_CELL_DESTRUCTOR virtual ~LiveNodeList() override;

    virtual u32 length() const override;
    virtual Node const* item(u32 index) const override;

    // Whether the filter matches node, for libweb_rust.
    bool node_matches_filter(Node const& node) const { return (*m_filter)(node); }

protected:
    LiveNodeList(Node const& root, Scope, Filter, Kind = Kind::Generic);

    Node* first_matching(Function<bool(Node const&)> const& filter) const;

private:
    explicit LiveNodeList(Node const& root);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // ^GC::WeakContainer
    virtual GC::Cell const& owner_cell(Badge<GC::Heap>) const override;
    virtual void remove_dead_cells(Badge<GC::Heap>) override;

    // NB: The core's byte for C++ holds the kind of the list.
    Kind kind() const { return static_cast<Kind>(m_core.cpp_state); }
    u64 invalidation_version(Document const&) const;
    void forget_nodes_if_kind_changed() const
    {
        if (kind() != Kind::Generic) [[unlikely]]
            forget_nodes_if_document_state_changed();
    }
    void forget_nodes_if_document_state_changed() const;
    bool is_list_of_labels_of_document() const;
    void find_nodes_among_labels_of_document_if_needed() const;

    // https://dom.spec.whatwg.org/#concept-collection
    // NB: libweb_rust reads and writes this in place, see Rust/src/dom/collection.rs.
    mutable RustFFI::CollectionCore m_core;

    GC::Ptr<GC::Function<bool(Node const&)>> m_filter;
    // The version of the state of the document a list of another kind than Generic reads.
    mutable u64 m_cached_kind_version { 0 };
};

}
