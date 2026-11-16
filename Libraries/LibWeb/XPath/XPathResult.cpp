/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Node.h>

#include "XPathResult.h"

namespace Web::XPath {

GC_DEFINE_ALLOCATOR(XPathResult);

GC::Ref<XPathResult> XPathResult::create()
{
    return GC::Heap::the().allocate<XPathResult>();
}

XPathResult::XPathResult() = default;

void XPathResult::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_node_set);
    visitor.visit(m_root);
}

XPathResult::~XPathResult() = default;

// A result that is used again for a value that isn't a node-set must not keep the tree of the last one alive.
void XPathResult::forget_node_set()
{
    m_node_set.clear();
    m_root = nullptr;
    m_next_node = 0;
}

void XPathResult::set_number(WebIDL::Double number_value)
{
    forget_node_set();
    m_result_type = NUMBER_TYPE;
    m_number_value = number_value;
}

void XPathResult::set_string(Utf16String string_value)
{
    forget_node_set();
    m_result_type = STRING_TYPE;
    m_string_value = move(string_value);
}

void XPathResult::set_boolean(bool boolean_value)
{
    forget_node_set();
    m_result_type = BOOLEAN_TYPE;
    m_boolean_value = boolean_value;
}

void XPathResult::set_node_set(ReadonlySpan<GC::Ref<DOM::Node>> node_set, unsigned short type, DOM::Node& root)
{
    // A result nobody asked a type of is an unordered iterator.
    if (type >= UNORDERED_NODE_ITERATOR_TYPE && type <= FIRST_ORDERED_NODE_TYPE)
        m_result_type = type;
    else
        m_result_type = UNORDERED_NODE_ITERATOR_TYPE;

    m_node_set.clear_with_capacity();
    m_node_set.ensure_capacity(node_set.size());
    for (auto node : node_set)
        m_node_set.unchecked_append(node);
    m_next_node = 0;
    m_root = root;
    m_dom_tree_version = root.dom_tree_version();
    m_character_data_version = root.character_data_version();
}

static WebIDL::SimpleException wrong_type()
{
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "The XPathResult is not of the type this needs"_utf16 };
}

// https://www.w3.org/TR/DOM-Level-3-XPath/xpath.html#XPathResult-numberValue
WebIDL::ExceptionOr<WebIDL::Double> XPathResult::number_value() const
{
    if (m_result_type != NUMBER_TYPE)
        return wrong_type();
    return m_number_value;
}

// https://www.w3.org/TR/DOM-Level-3-XPath/xpath.html#XPathResult-stringValue
WebIDL::ExceptionOr<Utf16String> XPathResult::string_value() const
{
    if (m_result_type != STRING_TYPE)
        return wrong_type();
    return m_string_value;
}

// https://www.w3.org/TR/DOM-Level-3-XPath/xpath.html#XPathResult-booleanValue
WebIDL::ExceptionOr<WebIDL::Boolean> XPathResult::boolean_value() const
{
    if (m_result_type != BOOLEAN_TYPE)
        return wrong_type();
    return m_boolean_value;
}

// https://www.w3.org/TR/DOM-Level-3-XPath/xpath.html#XPathResult-singleNodeValue
WebIDL::ExceptionOr<GC::Ptr<DOM::Node>> XPathResult::single_node_value() const
{
    if (m_result_type != ANY_UNORDERED_NODE_TYPE && m_result_type != FIRST_ORDERED_NODE_TYPE)
        return wrong_type();
    return m_node_set.is_empty() ? nullptr : m_node_set.first();
}

// https://www.w3.org/TR/DOM-Level-3-XPath/xpath.html#XPathResult-invalid-iterator-state
WebIDL::Boolean XPathResult::invalid_iterator_state() const
{
    // Changing the data of a text node changes the tree as much as moving a node does.
    return is_iterator()
        && (m_root->dom_tree_version() != m_dom_tree_version || m_root->character_data_version() != m_character_data_version);
}

// https://www.w3.org/TR/DOM-Level-3-XPath/xpath.html#XPathResult-snapshot-length
WebIDL::ExceptionOr<WebIDL::UnsignedLong> XPathResult::snapshot_length() const
{
    if (!is_snapshot())
        return wrong_type();
    return m_node_set.size();
}

// https://www.w3.org/TR/DOM-Level-3-XPath/xpath.html#XPathResult-iterateNext
WebIDL::ExceptionOr<GC::Ptr<DOM::Node>> XPathResult::iterate_next()
{
    if (!is_iterator())
        return wrong_type();
    if (invalid_iterator_state())
        return WebIDL::InvalidStateError::create("The tree has changed since the XPath expression was evaluated"_utf16);
    if (m_next_node >= m_node_set.size())
        return nullptr;
    return m_node_set[m_next_node++];
}

// https://www.w3.org/TR/DOM-Level-3-XPath/xpath.html#XPathResult-snapshotItem
WebIDL::ExceptionOr<GC::Ptr<DOM::Node>> XPathResult::snapshot_item(WebIDL::UnsignedLong index) const
{
    if (!is_snapshot())
        return wrong_type();
    if (index >= m_node_set.size())
        return nullptr;
    return m_node_set[index];
}

}
