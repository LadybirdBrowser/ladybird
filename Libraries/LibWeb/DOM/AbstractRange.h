/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/RustFFI.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#concept-range-bp
struct BoundaryPoint {
    GC::Ref<Node> node;
    WebIDL::UnsignedLong offset;
};

// https://dom.spec.whatwg.org/#abstractrange
class WEB_API AbstractRange : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(AbstractRange, Bindings::GCAllocatedWrappable);

public:
    static constexpr GC::MemberOffset start_container_offset() { return GC_FIELD_OFFSET(AbstractRange, m_range_core.start_container); }
    static constexpr GC::MemberOffset end_container_offset() { return GC_FIELD_OFFSET(AbstractRange, m_range_core.end_container); }
    virtual ~AbstractRange() override;

    BoundaryPoint start() const { return { start_container(), start_offset() }; }
    GC::Ref<Node> start_container() const { return *m_range_core.start_container; }
    WebIDL::UnsignedLong start_offset() const { return m_range_core.start_offset; }

    BoundaryPoint end() const { return { end_container(), end_offset() }; }
    GC::Ref<Node> end_container() const { return *m_range_core.end_container; }
    WebIDL::UnsignedLong end_offset() const { return m_range_core.end_offset; }

    // https://dom.spec.whatwg.org/#range-collapsed
    bool collapsed() const
    {
        // A range is collapsed if its start node is its end node and its start offset is its end offset.
        return m_range_core.start_container == m_range_core.end_container && start_offset() == end_offset();
    }

    bool operator==(AbstractRange const& other) const
    {
        return start_container() == other.start_container()
            && start_offset() == other.start_offset()
            && end_container() == other.end_container()
            && end_offset() == other.end_offset();
    }

protected:
    AbstractRange(GC::Ref<Node> start_container, WebIDL::UnsignedLong start_offset, GC::Ref<Node> end_container, WebIDL::UnsignedLong end_offset);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    void set_start_container(GC::Ref<Node> node) { m_range_core.start_container = node; }
    void set_start_offset(WebIDL::UnsignedLong offset) { m_range_core.start_offset = offset; }
    void set_end_container(GC::Ref<Node> node) { m_range_core.end_container = node; }
    void set_end_offset(WebIDL::UnsignedLong offset) { m_range_core.end_offset = offset; }

    // NB: libweb_rust reads and writes the boundary points in place, see Rust/src/dom/range.rs.
    RustFFI::RangeCore m_range_core;
};

}
