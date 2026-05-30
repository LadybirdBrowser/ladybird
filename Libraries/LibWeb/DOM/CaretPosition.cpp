/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CaretPosition.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/CaretPosition.h>
#include <LibWeb/Geometry/DOMRect.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(CaretPosition);

GC::Ref<CaretPosition> CaretPosition::create(JS::Realm& realm, GC::Ref<Node> offset_node, WebIDL::UnsignedLong offset, Optional<Gfx::FloatRect> client_rect)
{
    return realm.create<CaretPosition>(realm, offset_node, offset, move(client_rect));
}

CaretPosition::CaretPosition(JS::Realm& realm, GC::Ref<Node> offset_node, WebIDL::UnsignedLong offset, Optional<Gfx::FloatRect> client_rect)
    : Bindings::PlatformObject(realm)
    , m_offset_node(offset_node)
    , m_offset(offset)
    , m_client_rect(move(client_rect))
{
}

CaretPosition::~CaretPosition() = default;

void CaretPosition::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CaretPosition);
    Base::initialize(realm);
}

void CaretPosition::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_offset_node);
}

GC::Ptr<Geometry::DOMRect> CaretPosition::get_client_rect() const
{
    if (!m_client_rect.has_value())
        return nullptr;
    return Geometry::DOMRect::create(realm(), *m_client_rect);
}

}
