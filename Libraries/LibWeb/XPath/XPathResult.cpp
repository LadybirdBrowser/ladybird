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

void XPathResult::set_number(WebIDL::Double number_value)
{
    m_result_type = NUMBER_TYPE;
    m_number_value = number_value;
}
void XPathResult::set_string(String string_value)
{
    m_result_type = STRING_TYPE;
    m_string_value = move(string_value);
}

void XPathResult::set_boolean(bool boolean_value)
{
    m_result_type = BOOLEAN_TYPE;
    m_boolean_value = boolean_value;
}

void XPathResult::set_node_set(Vector<GC::Ptr<DOM::Node>> node_set, unsigned short type)
{
    if (type >= XPathResult::UNORDERED_NODE_ITERATOR_TYPE && type <= XPathResult::FIRST_ORDERED_NODE_TYPE)
        m_result_type = type;
    else
        m_result_type = UNORDERED_NODE_ITERATOR_TYPE; // Default if the caller does not explicity ask for anything else

    m_node_set = move(node_set);
    m_node_set_iter = m_node_set.begin();
}

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
