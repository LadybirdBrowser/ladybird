/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibGC/Heap.h>
#include <LibWeb/Bindings/PointerEvent.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/PointerEvent.h>
#include <LibWeb/UIEvents/PointerTypes.h>
#include <LibWebCommon/UIEvents/KeyCode.h>
#include <LibWebCommon/UIEvents/MouseButton.h>

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
    event_options.pointer_id = mouse_pointer_id;
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

WebIDL::ExceptionOr<GC::Ref<PointerEvent>> PointerEvent::create_from_platform_event(JS::Object const& relevant_global_object, GC::Ptr<HTML::WindowProxy> window_proxy, Utf16FlyString const& event_name, CSSPixelPoint screen, CSSPixelPoint page, CSSPixelPoint client, CSSPixelPoint offset, Optional<CSSPixelPoint> movement, unsigned button, unsigned buttons, unsigned modifiers)
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
    event_options.pointer_id = mouse_pointer_id;
    event_options.is_primary = true;
    event_options.pointer_type = PointerTypes::Mouse;
    if (movement.has_value()) {
        event_options.movement_x = movement->x().to_double();
        event_options.movement_y = movement->y().to_double();
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

PointerEvent::PointerEvent(Utf16FlyString const& type, PointerEventOptions&& options, double page_x, double page_y, double offset_x, double offset_y, HighResolutionTime::DOMHighResTimeStamp time_stamp)
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
    , m_pointer_type(options.pointer_type)
    , m_is_primary(options.is_primary)
    , m_persistent_device_id(options.persistent_device_id)
    , m_coalesced_events(move(options.coalesced_events))
    , m_predicted_events(move(options.predicted_events))
{
}

GC::Ref<PointerEvent> PointerEvent::create(Utf16FlyString const& type, PointerEventOptions&& options, double page_x, double page_y, double offset_x, double offset_y, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<PointerEvent>(type, move(options), page_x, page_y, offset_x, offset_y, time_stamp);
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

struct Tilt {
    i32 tilt_x { 0 };
    i32 tilt_y { 0 };
};

// https://w3c.github.io/pointerevents/#converting-between-tiltx-tilty-and-altitudeangle-azimuthangle
// User agents MUST use the following algorithm for converting these values.
static Tilt spherical2tilt(double altitude_angle, double azimuth_angle)
{
    constexpr auto pi = AK::Pi<double>;

    double tilt_x_radians = 0;
    double tilt_y_radians = 0;

    if (altitude_angle == 0) {
        // the pen is in the X-Y plane
        if (azimuth_angle == 0 || azimuth_angle == 2 * pi) {
            // pen is on positive X axis
            tilt_x_radians = pi / 2;
        }
        if (azimuth_angle == pi / 2) {
            // pen is on positive Y axis
            tilt_y_radians = pi / 2;
        }
        if (azimuth_angle == pi) {
            // pen is on negative X axis
            tilt_x_radians = -pi / 2;
        }
        if (azimuth_angle == 3 * pi / 2) {
            // pen is on negative Y axis
            tilt_y_radians = -pi / 2;
        }
        if (azimuth_angle > 0 && azimuth_angle < pi / 2) {
            tilt_x_radians = pi / 2;
            tilt_y_radians = pi / 2;
        }
        if (azimuth_angle > pi / 2 && azimuth_angle < pi) {
            tilt_x_radians = -pi / 2;
            tilt_y_radians = pi / 2;
        }
        if (azimuth_angle > pi && azimuth_angle < 3 * pi / 2) {
            tilt_x_radians = -pi / 2;
            tilt_y_radians = -pi / 2;
        }
        if (azimuth_angle > 3 * pi / 2 && azimuth_angle < 2 * pi) {
            tilt_x_radians = pi / 2;
            tilt_y_radians = -pi / 2;
        }
    }

    if (altitude_angle != 0) {
        auto tan_altitude = AK::tan(altitude_angle);

        tilt_x_radians = AK::atan(AK::cos(azimuth_angle) / tan_altitude);
        tilt_y_radians = AK::atan(AK::sin(azimuth_angle) / tan_altitude);
    }

    // When the user agent calculates tiltX / tiltY from azimuthAngle / altitudeAngle it SHOULD round the final
    // integer values using Math.round [ECMASCRIPT] rules.
    auto round = [](double value) { return static_cast<i32>(AK::floor(value + 0.5)); };
    return { round(AK::to_degrees(tilt_x_radians)), round(AK::to_degrees(tilt_y_radians)) };
}

struct SphericalAngles {
    double altitude_angle { 0 };
    double azimuth_angle { 0 };
};

// https://w3c.github.io/pointerevents/#converting-between-tiltx-tilty-and-altitudeangle-azimuthangle
static SphericalAngles tilt2spherical(i32 tilt_x, i32 tilt_y)
{
    constexpr auto pi = AK::Pi<double>;

    auto tilt_x_radians = AK::to_radians(static_cast<double>(tilt_x));
    auto tilt_y_radians = AK::to_radians(static_cast<double>(tilt_y));

    // calculate azimuth angle
    double azimuth_angle = 0;

    if (tilt_x == 0) {
        if (tilt_y > 0)
            azimuth_angle = pi / 2;
        else if (tilt_y < 0)
            azimuth_angle = 3 * pi / 2;
    } else if (tilt_y == 0) {
        if (tilt_x < 0)
            azimuth_angle = pi;
    } else if (AK::abs(tilt_x) == 90 || AK::abs(tilt_y) == 90) {
        // not enough information to calculate azimuth
        azimuth_angle = 0;
    } else {
        // Non-boundary case: neither tiltX nor tiltY is equal to 0 or +-90
        auto tan_x = AK::tan(tilt_x_radians);
        auto tan_y = AK::tan(tilt_y_radians);

        azimuth_angle = AK::atan2(tan_y, tan_x);
        if (azimuth_angle < 0)
            azimuth_angle += 2 * pi;
    }

    // calculate altitude angle
    double altitude_angle = 0;

    if (AK::abs(tilt_x) == 90 || AK::abs(tilt_y) == 90) {
        altitude_angle = 0;
    } else if (tilt_x == 0) {
        altitude_angle = pi / 2 - AK::fabs(tilt_y_radians);
    } else if (tilt_y == 0) {
        altitude_angle = pi / 2 - AK::fabs(tilt_x_radians);
    } else {
        // Non-boundary case: neither tiltX nor tiltY is equal to 0 or +-90
        auto tan_x = AK::tan(tilt_x_radians);
        auto tan_y = AK::tan(tilt_y_radians);
        altitude_angle = AK::atan(1.0 / AK::sqrt(tan_x * tan_x + tan_y * tan_y));
    }

    return { altitude_angle, azimuth_angle };
}

static void initialize_orientation(PointerEventOptions& options, Bindings::PointerEventInit const& event_init)
{
    constexpr auto pi = AK::Pi<double>;

    options.tilt_x = event_init.tilt_x.value_or(0);
    options.tilt_y = event_init.tilt_y.value_or(0);
    options.altitude_angle = event_init.altitude_angle.value_or(pi / 2);
    options.azimuth_angle = event_init.azimuth_angle.value_or(0);

    auto has_tilt = event_init.tilt_x.has_value() || event_init.tilt_y.has_value();
    auto has_spherical_angles = event_init.altitude_angle.has_value() || event_init.azimuth_angle.has_value();

    // NB: Only one representation can be derived from the other. When both or neither are supplied, the values are
    //     used as given.
    if (has_tilt == has_spherical_angles)
        return;

    if (has_tilt) {
        // INTEROP: Other browsers reduce tilt values to [-90, 90] before converting them, without changing the
        //          tiltX and tiltY attributes.
        auto normalize_tilt = [](i32 tilt) {
            auto normalized = tilt % 180;
            if (normalized > 90)
                normalized -= 180;
            else if (normalized < -90)
                normalized += 180;
            return normalized;
        };
        auto spherical_angles = tilt2spherical(normalize_tilt(options.tilt_x), normalize_tilt(options.tilt_y));
        options.altitude_angle = spherical_angles.altitude_angle;
        options.azimuth_angle = spherical_angles.azimuth_angle;
        return;
    }

    // INTEROP: Other browsers reduce azimuthAngle to [0, 2π] before converting it, without changing the
    //          azimuthAngle attribute.
    auto azimuth_angle = options.azimuth_angle;
    if (azimuth_angle < 0 || azimuth_angle > 2 * pi) {
        azimuth_angle = AK::fmod(azimuth_angle, 2 * pi);
        if (azimuth_angle < 0)
            azimuth_angle += 2 * pi;
    }
    auto tilt = spherical2tilt(options.altitude_angle, azimuth_angle);
    options.tilt_x = tilt.tilt_x;
    options.tilt_y = tilt.tilt_y;
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
    options.twist = event_init.twist;
    initialize_orientation(options, event_init);
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

WebIDL::ExceptionOr<GC::Ref<PointerEvent>> PointerEvent::create_for_constructor(Utf16String const& event_name, Bindings::PointerEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    PointerEventOptions options;
    static_cast<MouseEventOptions&>(options) = mouse_event_options_from_bindings(event_init);
    options.pointer_id = event_init.pointer_id;
    options.width = event_init.width;
    options.height = event_init.height;
    options.pressure = event_init.pressure;
    options.tangential_pressure = event_init.tangential_pressure;
    options.twist = event_init.twist;
    initialize_orientation(options, event_init);
    options.pointer_type = event_init.pointer_type;
    options.is_primary = event_init.is_primary;
    options.persistent_device_id = event_init.persistent_device_id;
    options.coalesced_events.ensure_capacity(event_init.coalesced_events.size());
    for (auto const& coalesced_event : event_init.coalesced_events)
        options.coalesced_events.unchecked_append(*coalesced_event);
    options.predicted_events.ensure_capacity(event_init.predicted_events.size());
    for (auto const& predicted_event : event_init.predicted_events)
        options.predicted_events.unchecked_append(*predicted_event);
    return create(Utf16FlyString::from_utf16(event_name.utf16_view()), move(options), event_init.client_x, event_init.client_y, event_init.client_x, event_init.client_y, time_stamp);
}

}
