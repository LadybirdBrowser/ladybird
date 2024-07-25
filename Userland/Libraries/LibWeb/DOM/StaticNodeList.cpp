/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/Error.h>
#include <LibWeb/DOM/StaticNodeList.h>

namespace Web::DOM {

JS_DEFINE_ALLOCATOR(StaticNodeList);

JS::NonnullGCPtr<NodeList> StaticNodeList::create(JS::Realm& realm, Vector<JS::Handle<Node>> static_nodes)
{
    return realm.heap().allocate<StaticNodeList>(realm, realm, move(static_nodes));
}

StaticNodeList::StaticNodeList(JS::Realm& realm, Vector<JS::Handle<Node>> static_nodes)
    : NodeList(realm)
{
    for (auto& node : static_nodes)
        m_static_nodes.append(*node);
}

StaticNodeList::~StaticNodeList() = default;

void StaticNodeList::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_static_nodes);
}

// https://dom.spec.whatwg.org/#dom-nodelist-length
u32 StaticNodeList::length() const
{
    return m_static_nodes.size();
}

// https://dom.spec.whatwg.org/#dom-nodelist-item
Node const* StaticNodeList::item(u32 index) const
{
    // The item(index) method must return the indexth node in the collection. If there is no indexth node in the collection, then the method must return null.
    if (index >= m_static_nodes.size())
        return nullptr;
    return m_static_nodes[index];
}

}
