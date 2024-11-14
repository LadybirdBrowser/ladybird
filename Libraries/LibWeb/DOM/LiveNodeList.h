/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibWeb/DOM/NodeList.h>

namespace Web::DOM {

// FIXME: Just like HTMLCollection, LiveNodeList currently does no caching.

class LiveNodeList : public NodeList {
    WEB_PLATFORM_OBJECT(LiveNodeList, NodeList);
    GC_DECLARE_ALLOCATOR(LiveNodeList);

public:
    enum class Scope {
        Children,
        Descendants,
    };

    [[nodiscard]] static GC::Ref<NodeList> create(JS::Realm&, Node const& root, Scope, ESCAPING Function<bool(Node const&)> filter);
    virtual ~LiveNodeList() override;

    virtual u32 length() const override;
    virtual Node const* item(u32 index) const override;

protected:
    LiveNodeList(JS::Realm&, Node const& root, Scope, ESCAPING Function<bool(Node const&)> filter);

    Node* first_matching(Function<bool(Node const&)> const& filter) const;

private:
    virtual void visit_edges(Cell::Visitor&) override;

    GC::MarkedVector<Node*> collection() const;

    GC::Ref<Node const> m_root;
    Function<bool(Node const&)> m_filter;
    Scope m_scope { Scope::Descendants };
};

}
