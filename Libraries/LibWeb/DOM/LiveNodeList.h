/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibGC/Weak.h>
#include <LibGC/WeakContainer.h>
#include <LibWeb/DOM/NodeList.h>

namespace Web::DOM {

class Document;

class LiveNodeList
    : public NodeList
    , public GC::WeakContainer {
    WEB_NON_IDL_WRAPPABLE(LiveNodeList, NodeList);
    GC_DECLARE_ALLOCATOR(LiveNodeList);

public:
    enum class Scope {
        Children,
        Descendants,
    };

    enum class Kind {
        Generic,
        Labels,
        FormControls,
    };

    [[nodiscard]] static GC::Ref<NodeList> create(Node const& root, Scope, ESCAPING Function<bool(Node const&)> filter, Kind = Kind::Generic);
    virtual ~LiveNodeList() override;

    virtual u32 length() const override;
    virtual Node const* item(u32 index) const override;

protected:
    LiveNodeList(Node const& root, Scope, ESCAPING Function<bool(Node const&)> filter, Kind = Kind::Generic);

    Node* first_matching(Function<bool(Node const&)> const& filter) const;

private:
    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual void remove_dead_cells(Badge<GC::Heap>) override;
    virtual GC::Cell const& owner_cell(Badge<GC::Heap>) const override;

    void update_cache_if_needed() const;
    u64 invalidation_version(Document const&) const;

    mutable GC::Weak<Document const> m_cached_document;
    mutable u64 m_cached_dom_tree_version { 0 };
    mutable u64 m_cached_invalidation_version { 0 };
    mutable Vector<GC::RawPtr<Node>> m_cached_nodes;

    GC::Ref<Node const> m_root;
    Function<bool(Node const&)> m_filter;
    Scope m_scope { Scope::Descendants };
    Kind m_kind { Kind::Generic };
};

}
