/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::DOM {

class Node;

class NodeFilter final : public GC::Cell {
    GC_CELL(NodeFilter, GC::Cell);
    GC_DECLARE_ALLOCATOR(NodeFilter);

public:
    [[nodiscard]] static GC::Ref<NodeFilter> create(GC::Ref<WebIDL::CallbackType>);

    virtual ~NodeFilter() = default;

    WebIDL::CallbackType& callback() { return m_callback; }

    // FIXME: Generate both of these enums from IDL.
    enum class Result : u8 {
        FILTER_ACCEPT = 1,
        FILTER_REJECT = 2,
        FILTER_SKIP = 3,
    };

    enum class WhatToShow : u32 {
        SHOW_ALL = 0xFFFFFFFF,
        SHOW_ELEMENT = 0x1,
        SHOW_ATTRIBUTE = 0x2,
        SHOW_TEXT = 0x4,
        SHOW_CDATA_SECTION = 0x8,
        SHOW_PROCESSING_INSTRUCTION = 0x40,
        SHOW_COMMENT = 0x80,
        SHOW_DOCUMENT = 0x100,
        SHOW_DOCUMENT_TYPE = 0x200,
        SHOW_DOCUMENT_FRAGMENT = 0x400,
    };

private:
    NodeFilter(GC::Ref<WebIDL::CallbackType>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<WebIDL::CallbackType> m_callback;
};

AK_ENUM_BITWISE_OPERATORS(NodeFilter::WhatToShow);

using TraversalFilter = Function<Optional<NodeFilter::Result>(Node&)>;

struct TraversalFilterResult {
    enum class Type : u8 {
        Result,
        AlreadyActive,
        CallbackException,
    };

    static TraversalFilterResult from_result(NodeFilter::Result result) { return { Type::Result, result }; }
    static TraversalFilterResult accept() { return from_result(NodeFilter::Result::FILTER_ACCEPT); }
    static TraversalFilterResult reject() { return from_result(NodeFilter::Result::FILTER_REJECT); }
    static TraversalFilterResult skip() { return from_result(NodeFilter::Result::FILTER_SKIP); }
    static TraversalFilterResult already_active() { return { Type::AlreadyActive, NodeFilter::Result::FILTER_SKIP }; }
    static TraversalFilterResult callback_exception() { return { Type::CallbackException, NodeFilter::Result::FILTER_SKIP }; }

    bool is(NodeFilter::Result result) const { return type == Type::Result && this->result == result; }

    Type type;
    NodeFilter::Result result;
};

struct TraversalResult {
    enum class Type : u8 {
        Node,
        Null,
        AlreadyActive,
        CallbackException,
    };

    static TraversalResult from_node(GC::Ptr<Node> node) { return { Type::Node, node }; }
    static TraversalResult null() { return { Type::Null, nullptr }; }
    static TraversalResult already_active() { return { Type::AlreadyActive, nullptr }; }
    static TraversalResult callback_exception() { return { Type::CallbackException, nullptr }; }

    Type type;
    GC::Ptr<Node> node;
};

}
