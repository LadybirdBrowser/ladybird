/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::XPath {

class XPathResult : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(XPathResult, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(XPathResult);

public:
    [[nodiscard]] static GC::Ref<XPathResult> create();

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

    XPathResult();
    virtual ~XPathResult() override;
    virtual void visit_edges(GC::Cell::Visitor&) override;

    WebIDL::UnsignedShort result_type() const { return m_result_type; }
    WebIDL::ExceptionOr<WebIDL::Double> number_value() const;
    WebIDL::ExceptionOr<Utf16String> string_value() const;
    WebIDL::ExceptionOr<WebIDL::Boolean> boolean_value() const;
    WebIDL::ExceptionOr<GC::Ptr<DOM::Node>> single_node_value() const;
    WebIDL::Boolean invalid_iterator_state() const;
    WebIDL::ExceptionOr<WebIDL::UnsignedLong> snapshot_length() const;

    WebIDL::ExceptionOr<GC::Ptr<DOM::Node>> iterate_next();
    WebIDL::ExceptionOr<GC::Ptr<DOM::Node>> snapshot_item(WebIDL::UnsignedLong index) const;

    // The nodes of a result that has nodes, in document order.
    ReadonlySpan<GC::Ptr<DOM::Node>> nodes() const { return m_node_set; }

    void set_number(WebIDL::Double number_value);
    void set_string(Utf16String string_value);
    void set_boolean(bool boolean_value);
    // `root` is the root of the tree the nodes were selected in: changing that tree invalidates an iterator over them.
    void set_node_set(ReadonlySpan<GC::Ref<DOM::Node>> node_set, unsigned short type, DOM::Node& root);

private:
    void forget_node_set();

    bool is_iterator() const { return m_result_type == UNORDERED_NODE_ITERATOR_TYPE || m_result_type == ORDERED_NODE_ITERATOR_TYPE; }
    bool is_snapshot() const { return m_result_type == UNORDERED_NODE_SNAPSHOT_TYPE || m_result_type == ORDERED_NODE_SNAPSHOT_TYPE; }

    WebIDL::UnsignedShort m_result_type { 0 };
    WebIDL::Double m_number_value { 0 };
    Utf16String m_string_value;
    WebIDL::Boolean m_boolean_value { false };

    Vector<GC::Ptr<DOM::Node>> m_node_set;
    size_t m_next_node { 0 };
    GC::Ptr<DOM::Node> m_root;
    u64 m_dom_tree_version { 0 };
    u64 m_character_data_version { 0 };
};

}
