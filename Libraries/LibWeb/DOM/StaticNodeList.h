/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/NodeList.h>

namespace Web::DOM {

class StaticNodeList final : public NodeList {
    WEB_NON_IDL_WRAPPABLE(StaticNodeList, NodeList);
    GC_DECLARE_ALLOCATOR(StaticNodeList);

public:
    [[nodiscard]] static GC::Ref<NodeList> create(Vector<GC::Root<Node>>);

    virtual ~StaticNodeList() override;

    virtual u32 length() const override;
    virtual Node const* item(u32 index) const override;

private:
    StaticNodeList(Vector<GC::Root<Node>>);

    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual size_t external_memory_size() const override;

    Vector<GC::Ref<Node>> m_static_nodes;
};

}
