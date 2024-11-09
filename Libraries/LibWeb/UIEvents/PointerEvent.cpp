/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/PointerEventPrototype.h>
#include <LibWeb/UIEvents/PointerEvent.h>

namespace Web::UIEvents {

JS_DEFINE_ALLOCATOR(PointerEvent);

PointerEvent::PointerEvent(JS::Realm& realm, FlyString const& type, PointerEventInit const& event_init, double page_x, double page_y, double offset_x, double offset_y)
    : MouseEvent(realm, type, event_init, page_x, page_y, offset_x, offset_y)
    , m_pointer_id(event_init.pointer_id)
    , m_width(event_init.width)
    , m_height(event_init.height)
    , m_pressure(event_init.pressure)
    , m_tangential_pressure(event_init.tangential_pressure)
    , m_tilt_x(event_init.tilt_x.value_or(0))
    , m_tilt_y(event_init.tilt_y.value_or(0))
    , m_twist(event_init.twist)
    , m_altitude_angle(event_init.altitude_angle.value_or(DEFAULT_ALTITUDE_ANGLE))
    , m_azimuth_angle(event_init.azimuth_angle.value_or(0))
    , m_pointer_type(event_init.pointer_type)
    , m_is_primary(event_init.is_primary)
    , m_persistent_device_id(event_init.persistent_device_id)
{
    m_coalesced_events.ensure_capacity(event_init.coalesced_events.size());
    for (auto const& coalesced_event : event_init.coalesced_events)
        m_coalesced_events.unchecked_append(*coalesced_event);

    m_predicted_events.ensure_capacity(event_init.predicted_events.size());
    for (auto const& predicted_event : event_init.predicted_events)
        m_predicted_events.unchecked_append(*predicted_event);
}

PointerEvent::~PointerEvent() = default;

void PointerEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PointerEvent);
}

void PointerEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_coalesced_events);
    visitor.visit(m_predicted_events);
}

JS::NonnullGCPtr<PointerEvent> PointerEvent::create(JS::Realm& realm, FlyString const& type, PointerEventInit const& event_init, double page_x, double page_y, double offset_x, double offset_y)
{
    return realm.heap().allocate<PointerEvent>(realm, realm, type, event_init, page_x, page_y, offset_x, offset_y);
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<PointerEvent>> PointerEvent::construct_impl(JS::Realm& realm, FlyString const& type, PointerEventInit const& event_init)
{
    return create(realm, type, event_init);
}

}
