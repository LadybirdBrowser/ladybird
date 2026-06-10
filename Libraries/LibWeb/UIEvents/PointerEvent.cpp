/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/PointerEvent.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWeb/UIEvents/PointerEvent.h>
#include <LibWeb/UIEvents/PointerTypes.h>

namespace Web::UIEvents {

GC_DEFINE_ALLOCATOR(PointerEvent);

// https://w3c.github.io/pointerevents/#dom-pointerevent-screenx
// For untrusted PointerEvents, coordinates are floored for click, auxclick, and
// contextmenu events (via the MouseEvent base class). For all other pointer
// event types, fractional coordinates are preserved.
bool PointerEvent::should_have_fractional_coordinates() const
{
    if (is_trusted())
        return true;
    return type() != HTML::EventNames::click
        && type() != UIEvents::EventNames::auxclick
        && type() != HTML::EventNames::contextmenu;
}

double PointerEvent::screen_x() const { return should_have_fractional_coordinates() ? m_screen_x : MouseEvent::screen_x(); }
double PointerEvent::screen_y() const { return should_have_fractional_coordinates() ? m_screen_y : MouseEvent::screen_y(); }
double PointerEvent::page_x() const { return should_have_fractional_coordinates() ? m_page_x : MouseEvent::page_x(); }
double PointerEvent::page_y() const { return should_have_fractional_coordinates() ? m_page_y : MouseEvent::page_y(); }
double PointerEvent::client_x() const { return should_have_fractional_coordinates() ? m_client_x : MouseEvent::client_x(); }
double PointerEvent::client_y() const { return should_have_fractional_coordinates() ? m_client_y : MouseEvent::client_y(); }
double PointerEvent::offset_x() const { return should_have_fractional_coordinates() ? m_offset_x : MouseEvent::offset_x(); }
double PointerEvent::offset_y() const { return should_have_fractional_coordinates() ? m_offset_y : MouseEvent::offset_y(); }

WebIDL::ExceptionOr<GC::Ref<PointerEvent>> PointerEvent::create_from_platform_event(JS::Object const& relevant_global_object, GC::Ptr<HTML::WindowProxy> window_proxy, FlyString const& event_name, CSSPixelPoint screen, CSSPixelPoint page, CSSPixelPoint client, CSSPixelPoint offset, Optional<CSSPixelPoint> movement, unsigned button, unsigned buttons, unsigned modifiers)
{
    PointerEventOptions event_options;
    event_options.ctrl_key = modifiers & Mod_Ctrl;
    event_options.shift_key = modifiers & Mod_Shift;
    event_options.alt_key = modifiers & Mod_Alt;
    event_options.meta_key = modifiers & Mod_Super;
    event_options.screen_x = screen.x().to_double();
    event_options.screen_y = screen.y().to_double();
    event_options.client_x = client.x().to_double();
    event_options.client_y = client.y().to_double();
    event_options.view = window_proxy;
    event_options.is_primary = true;
    event_options.pointer_type = PointerTypes::Mouse;
    if (movement.has_value()) {
        event_options.movement_x = movement.value().x().to_double();
        event_options.movement_y = movement.value().y().to_double();
    }
    event_options.button = mouse_button_to_button_code(static_cast<MouseButton>(button));
    event_options.buttons = buttons;
    auto event = PointerEvent::create(event_name, move(event_options), page.x().to_double(), page.y().to_double(), offset.x().to_double(), offset.y().to_double(), HighResolutionTime::current_high_resolution_time(relevant_global_object));
    event->set_is_trusted(true);
    event->set_bubbles(true);
    event->set_cancelable(true);
    event->set_composed(true);
    return event;
}

PointerEvent::PointerEvent(FlyString const& type, PointerEventOptions&& options, double page_x, double page_y, double offset_x, double offset_y, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : MouseEvent(type, options, page_x, page_y, offset_x, offset_y, time_stamp)
    , m_pointer_id(options.pointer_id)
    , m_width(options.width)
    , m_height(options.height)
    , m_pressure(options.pressure)
    , m_tangential_pressure(options.tangential_pressure)
    , m_tilt_x(options.tilt_x)
    , m_tilt_y(options.tilt_y)
    , m_twist(options.twist)
    , m_altitude_angle(options.altitude_angle)
    , m_azimuth_angle(options.azimuth_angle)
    , m_pointer_type(move(options.pointer_type))
    , m_is_primary(options.is_primary)
    , m_persistent_device_id(options.persistent_device_id)
    , m_coalesced_events(move(options.coalesced_events))
    , m_predicted_events(move(options.predicted_events))
{
}

PointerEvent::~PointerEvent() = default;

void PointerEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_coalesced_events);
    visitor.visit(m_predicted_events);
}

GC::Ref<PointerEvent> PointerEvent::create(
    FlyString const& type, PointerEventOptions&& options,
    double page_x, double page_y, double offset_x, double offset_y,
    HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<PointerEvent>(type, move(options), page_x, page_y, offset_x, offset_y, time_stamp);
}

WebIDL::ExceptionOr<GC::Ref<PointerEvent>> PointerEvent::create_for_constructor(FlyString const& event_name, Bindings::PointerEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    PointerEventOptions options;
    static_cast<MouseEventOptions&>(options) = mouse_event_options_from_bindings(event_init);
    options.pointer_id = event_init.pointer_id;
    options.width = event_init.width;
    options.height = event_init.height;
    options.pressure = event_init.pressure;
    options.tangential_pressure = event_init.tangential_pressure;
    options.tilt_x = event_init.tilt_x.value_or(0);
    options.tilt_y = event_init.tilt_y.value_or(0);
    options.twist = event_init.twist;
    options.altitude_angle = event_init.altitude_angle.value_or(DEFAULT_ALTITUDE_ANGLE);
    options.azimuth_angle = event_init.azimuth_angle.value_or(0);
    options.pointer_type = event_init.pointer_type;
    options.is_primary = event_init.is_primary;
    options.persistent_device_id = event_init.persistent_device_id;

    options.coalesced_events.ensure_capacity(event_init.coalesced_events.size());
    for (auto const& coalesced_event : event_init.coalesced_events)
        options.coalesced_events.unchecked_append(*coalesced_event);

    options.predicted_events.ensure_capacity(event_init.predicted_events.size());
    for (auto const& predicted_event : event_init.predicted_events)
        options.predicted_events.unchecked_append(*predicted_event);

    return create(event_name, move(options), event_init.client_x, event_init.client_y, event_init.client_x, event_init.client_y, time_stamp);
}

}
