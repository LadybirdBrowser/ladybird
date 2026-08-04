/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Rect.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::DOM {

class CaretPosition final : public Bindings::Wrappable {
    WEB_WRAPPABLE(CaretPosition, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(CaretPosition);

public:
    [[nodiscard]] static GC::Ref<CaretPosition> create(GC::Ref<Node> offset_node, WebIDL::UnsignedLong offset, Optional<Gfx::FloatRect> client_rect);

    virtual ~CaretPosition() override;

    GC::Ref<Node> offset_node() const { return m_offset_node; }
    WebIDL::UnsignedLong offset() const { return m_offset; }

    GC::Ptr<Geometry::DOMRect> get_client_rect() const;

private:
    CaretPosition(GC::Ref<Node> offset_node, WebIDL::UnsignedLong offset, Optional<Gfx::FloatRect> client_rect);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<Node> m_offset_node;
    WebIDL::UnsignedLong m_offset { 0 };
    Optional<Gfx::FloatRect> m_client_rect;
};

}
