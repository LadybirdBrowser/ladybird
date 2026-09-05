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
    // The nodes must be kept alive by something else until the list is created, such as the live tree they
    // were collected from; the list itself is the only owner they need afterwards.
    [[nodiscard]] static GC::Ref<NodeList> create(Vector<GC::RawRef<Node>>);
    [[nodiscard]] static GC::Ref<NodeList> create(ReadonlySpan<GC::Root<Node>>);

    virtual ~StaticNodeList() override;

    virtual u32 length() const override;
    virtual Node const* item(u32 index) const override;

private:
    explicit StaticNodeList(Vector<GC::RawRef<Node>>);

    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual size_t external_memory_size() const override;

    Vector<GC::Ref<Node>> m_static_nodes;
};

}
