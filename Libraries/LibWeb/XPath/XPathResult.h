/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::XPath {

class XPathResult : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(XPathResult, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(XPathResult);

public:
    static WebIDL::UnsignedShort const ANY_TYPE = 0;
    static WebIDL::UnsignedShort const NUMBER_TYPE = 1;
    static WebIDL::UnsignedShort const STRING_TYPE = 2;
    static WebIDL::UnsignedShort const BOOLEAN_TYPE = 3;
    static WebIDL::UnsignedShort const UNORDERED_NODE_ITERATOR_TYPE = 4;
    static WebIDL::UnsignedShort const ORDERED_NODE_ITERATOR_TYPE = 5;
    static WebIDL::UnsignedShort const UNORDERED_NODE_SNAPSHOT_TYPE = 6;
    static WebIDL::UnsignedShort const ORDERED_NODE_SNAPSHOT_TYPE = 7;
    static WebIDL::UnsignedShort const ANY_UNORDERED_NODE_TYPE = 8;
    static WebIDL::UnsignedShort const FIRST_ORDERED_NODE_TYPE = 9;

    XPathResult(JS::Realm&);
    virtual ~XPathResult() override;
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    WebIDL::UnsignedShort result_type() const { return m_result_type; }
    WebIDL::Double number_value() const { return m_number_value; }
    String string_value() { return m_string_value; }
    WebIDL::Boolean boolean_value() const { return m_boolean_value; }
    GC::Ptr<DOM::Node> single_node_value() { return m_node_set.is_empty() ? nullptr : m_node_set.first(); }
    WebIDL::Boolean invalid_iterator_state() const { return m_invalid_iterator_state; }
    WebIDL::UnsignedLong snapshot_length() const { return m_node_set.size(); }

    GC::Ptr<DOM::Node> iterate_next();
    GC::Ptr<DOM::Node> snapshot_item(int index);

    void set_number(WebIDL::Double number_value);
    void set_string(String string_value);
    void set_boolean(bool boolean_value);
    void set_node_set(Vector<GC::Ptr<DOM::Node>> node_set, unsigned short type);

private:
    WebIDL::UnsignedShort m_result_type;
    WebIDL::Double m_number_value;
    String m_string_value;
    WebIDL::Boolean m_boolean_value;
    WebIDL::Boolean m_invalid_iterator_state;
    WebIDL::UnsignedLong m_snapshot_length;

    Vector<GC::Ptr<DOM::Node>> m_node_set;
    Vector<GC::Ptr<DOM::Node>>::Iterator m_node_set_iter;
};

}
