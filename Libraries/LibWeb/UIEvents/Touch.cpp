/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/UIEvents/Touch.h>

namespace Web::UIEvents {

GC_DEFINE_ALLOCATOR(Touch);

GC::Ref<Touch> Touch::create(Bindings::TouchInit const& init)
{
    return GC::Heap::the().allocate<Touch>(init);
}

// https://w3c.github.io/touch-events/#touch-interface
// Touch objects are immutable; after one is created, its attributes must not change.
Touch::Touch(Bindings::TouchInit const& init)
    : m_target(init.target)
    , m_identifier(init.identifier)
    , m_screen_x(init.screen_x)
    , m_screen_y(init.screen_y)
    , m_client_x(init.client_x)
    , m_client_y(init.client_y)
    , m_page_x(init.page_x)
    , m_page_y(init.page_y)
    , m_radius_x(init.radius_x)
    , m_radius_y(init.radius_y)
    , m_rotation_angle(init.rotation_angle)
    , m_force(init.force)
{
}

GC::Ref<Touch> Touch::clone_with_target(GC::Ref<DOM::EventTarget> target) const
{
    Bindings::TouchInit init { .identifier = m_identifier, .target = target };
    init.client_x = m_client_x;
    init.client_y = m_client_y;
    init.screen_x = m_screen_x;
    init.screen_y = m_screen_y;
    init.page_x = m_page_x;
    init.page_y = m_page_y;
    init.radius_x = m_radius_x;
    init.radius_y = m_radius_y;
    init.rotation_angle = m_rotation_angle;
    init.force = m_force;
    return create(init);
}

void Touch::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_target);
}

}
