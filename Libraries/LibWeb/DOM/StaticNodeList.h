/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NodeList.h>

namespace Web::DOM {

class StaticNodeList final : public NodeList {
    WEB_PLATFORM_OBJECT(StaticNodeList, NodeList);
    GC_DECLARE_ALLOCATOR(StaticNodeList);

public:
    [[nodiscard]] static GC::Ref<NodeList> create(JS::Realm&, Vector<GC::Root<Node>> const&);

    virtual ~StaticNodeList() override;

    virtual u32 length() const override;
    virtual Node const* item(u32 index) const override;

private:
    StaticNodeList(JS::Realm&, Vector<GC::Root<Node>> const&);

    virtual void visit_edges(Cell::Visitor&) override;

    Vector<GC::Ref<Node>> m_static_nodes;
};

}
