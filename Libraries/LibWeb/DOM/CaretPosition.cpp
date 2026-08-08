/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/CaretPosition.h>
#include <LibWeb/Geometry/DOMRect.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(CaretPosition);

GC::Ref<CaretPosition> CaretPosition::create(GC::Ref<Node> offset_node, WebIDL::UnsignedLong offset, Optional<Gfx::FloatRect> client_rect)
{
    return GC::Heap::the().allocate<CaretPosition>(offset_node, offset, move(client_rect));
}

CaretPosition::CaretPosition(GC::Ref<Node> offset_node, WebIDL::UnsignedLong offset, Optional<Gfx::FloatRect> client_rect)
    : m_offset_node(offset_node)
    , m_offset(offset)
    , m_client_rect(move(client_rect))
{
}

CaretPosition::~CaretPosition() = default;

void CaretPosition::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_offset_node);
}

GC::Ptr<Geometry::DOMRect> CaretPosition::get_client_rect() const
{
    if (!m_client_rect.has_value())
        return nullptr;
    return Geometry::DOMRect::create(*m_client_rect);
}

}
