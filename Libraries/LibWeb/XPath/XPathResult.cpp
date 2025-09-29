/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/XPathResultPrototype.h>
#include <LibWeb/DOM/Node.h>

#include "XPathResult.h"

namespace Web::XPath {

GC_DEFINE_ALLOCATOR(XPathResult);

XPathResult::XPathResult(JS::Realm& realm)
    : Web::Bindings::PlatformObject(realm)
{
    m_node_set_iter = m_node_set.end();
}

void XPathResult::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(XPathResult);
    Base::initialize(realm);
}

void XPathResult::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_node_set);
}

XPathResult::~XPathResult() = default;

GC::Ptr<DOM::Node> XPathResult::iterate_next()
{
    if (m_node_set_iter == m_node_set.end())
        return nullptr;

    return *m_node_set_iter++;
}

GC::Ptr<DOM::Node> XPathResult::snapshot_item(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= m_node_set.size())
        return nullptr;

    return m_node_set.at(index);
}

}
