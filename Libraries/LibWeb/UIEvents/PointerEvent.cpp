/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/PointerEventPrototype.h>
#include <LibWeb/UIEvents/PointerEvent.h>

namespace Web::UIEvents {

GC_DEFINE_ALLOCATOR(PointerEvent);

WebIDL::ExceptionOr<GC::Ref<PointerEvent>> PointerEvent::create_from_platform_event(JS::Realm& realm, FlyString const& event_name, CSSPixelPoint screen, CSSPixelPoint page, CSSPixelPoint client, CSSPixelPoint offset, Optional<CSSPixelPoint> movement, unsigned button, unsigned buttons, unsigned modifiers)
{
    PointerEventInit event_init {};
    event_init.ctrl_key = modifiers & Mod_Ctrl;
    event_init.shift_key = modifiers & Mod_Shift;
    event_init.alt_key = modifiers & Mod_Alt;
    event_init.meta_key = modifiers & Mod_Super;
    event_init.screen_x = screen.x().to_double();
    event_init.screen_y = screen.y().to_double();
    event_init.client_x = client.x().to_double();
    event_init.client_y = client.y().to_double();
    if (movement.has_value()) {
        event_init.movement_x = movement.value().x().to_double();
        event_init.movement_y = movement.value().y().to_double();
    }
    event_init.button = mouse_button_to_button_code(static_cast<MouseButton>(button));
    event_init.buttons = buttons;
    auto event = PointerEvent::create(realm, event_name, event_init, page.x().to_double(), page.y().to_double(), offset.x().to_double(), offset.y().to_double());
    event->set_is_trusted(true);
    event->set_bubbles(true);
    event->set_cancelable(true);
    event->set_composed(true);
    return event;
}

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

GC::Ref<PointerEvent> PointerEvent::create(JS::Realm& realm, FlyString const& type, PointerEventInit const& event_init, double page_x, double page_y, double offset_x, double offset_y)
{
    return realm.create<PointerEvent>(realm, type, event_init, page_x, page_y, offset_x, offset_y);
}

WebIDL::ExceptionOr<GC::Ref<PointerEvent>> PointerEvent::construct_impl(JS::Realm& realm, FlyString const& type, PointerEventInit const& event_init)
{
    return create(realm, type, event_init);
}

}
