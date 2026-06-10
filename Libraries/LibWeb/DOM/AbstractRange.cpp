/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/AbstractRange.h>
#include <LibWeb/DOM/Document.h>

namespace Web::DOM {

AbstractRange::AbstractRange(GC::Ref<Node> start_container, WebIDL::UnsignedLong start_offset, GC::Ref<Node> end_container, WebIDL::UnsignedLong end_offset)
    : m_start_container(start_container)
    , m_start_offset(start_offset)
    , m_end_container(end_container)
    , m_end_offset(end_offset)
{
}

AbstractRange::~AbstractRange() = default;

void AbstractRange::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_start_container);
    visitor.visit(m_end_container);
}

}
