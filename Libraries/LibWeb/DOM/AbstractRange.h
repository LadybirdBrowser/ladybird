/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#concept-range-bp
struct BoundaryPoint {
    GC::Ref<Node> node;
    WebIDL::UnsignedLong offset;
};

// https://dom.spec.whatwg.org/#abstractrange
class AbstractRange : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(AbstractRange, Bindings::PlatformObject);

public:
    virtual ~AbstractRange() override;

    BoundaryPoint start() const { return { m_start_container, m_start_offset }; }
    GC::Ref<Node> start_container() const { return m_start_container; }
    WebIDL::UnsignedLong start_offset() const { return m_start_offset; }

    BoundaryPoint end() const { return { m_end_container, m_end_offset }; }
    GC::Ref<Node> end_container() const { return m_end_container; }
    WebIDL::UnsignedLong end_offset() const { return m_end_offset; }

    // https://dom.spec.whatwg.org/#range-collapsed
    bool collapsed() const
    {
        // A range is collapsed if its start node is its end node and its start offset is its end offset.
        return start_container() == end_container() && start_offset() == end_offset();
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

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<Node> m_start_container;
    WebIDL::UnsignedLong m_start_offset;

    GC::Ref<Node> m_end_container;
    WebIDL::UnsignedLong m_end_offset;
};

}
