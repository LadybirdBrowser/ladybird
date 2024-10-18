/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::UIEvents {

struct PointerEventInit : public MouseEventInit {
    WebIDL::Long pointer_id { 0 };
    double width { 1 };
    double height { 1 };
    float pressure { 0 };
    float tangential_pressure { 0 };
    Optional<WebIDL::Long> tilt_x;
    Optional<WebIDL::Long> tilt_y;
    WebIDL::Long twist { 0 };
    Optional<double> altitude_angle;
    Optional<double> azimuth_angle;
    String pointer_type;
    bool is_primary { false };
    WebIDL::Long persistent_device_id { 0 };
    AK::Vector<JS::Handle<PointerEvent>> coalesced_events;
    AK::Vector<JS::Handle<PointerEvent>> predicted_events;
};

// https://w3c.github.io/pointerevents/#pointerevent-interface
class PointerEvent : public MouseEvent {
    WEB_PLATFORM_OBJECT(PointerEvent, MouseEvent);
    JS_DECLARE_ALLOCATOR(PointerEvent);

public:
    [[nodiscard]] static JS::NonnullGCPtr<PointerEvent> create(JS::Realm&, FlyString const& type, PointerEventInit const& = {}, double page_x = 0, double page_y = 0, double offset_x = 0, double offset_y = 0);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<PointerEvent>> construct_impl(JS::Realm&, FlyString const& type, PointerEventInit const&);

    virtual ~PointerEvent() override;

    WebIDL::Long pointer_id() const { return m_pointer_id; }
    double width() const { return m_width; }
    double height() const { return m_height; }
    float pressure() const { return m_pressure; }
    float tangential_pressure() const { return m_tangential_pressure; }
    WebIDL::Long tilt_x() const { return m_tilt_x; }
    WebIDL::Long tilt_y() const { return m_tilt_y; }
    WebIDL::Long twist() const { return m_twist; }
    double altitude_angle() const { return m_altitude_angle; }
    double azimuth_angle() const { return m_azimuth_angle; }
    String const& pointer_type() const { return m_pointer_type; }
    bool is_primary() const { return m_is_primary; }
    WebIDL::Long persistent_device_id() const { return m_persistent_device_id; }
    AK::ReadonlySpan<JS::NonnullGCPtr<PointerEvent>> get_coalesced_events() const { return m_coalesced_events; }
    AK::ReadonlySpan<JS::NonnullGCPtr<PointerEvent>> get_predicted_events() const { return m_predicted_events; }

    // https://w3c.github.io/pointerevents/#dom-pointerevent-pressure
    // For hardware and platforms that do not support pressure, the value MUST be 0.5 when in the active buttons state and 0 otherwise.
    static constexpr float ACTIVE_PRESSURE_DEFAULT_IN_ACTIVE_BUTTON_STATE { 0.5 };

protected:
    PointerEvent(JS::Realm&, FlyString const& type, PointerEventInit const&, double page_x, double page_y, double offset_x, double offset_y);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    virtual bool is_pointer_event() const final { return true; }

    // A unique identifier for the pointer causing the event.
    // https://w3c.github.io/pointerevents/#dom-pointerevent-pointerid
    WebIDL::Long m_pointer_id { 0 };

    // The width (magnitude on the X axis), in CSS pixels (see [CSS21]), of the contact geometry of the pointer
    // https://w3c.github.io/pointerevents/#dom-pointerevent-width
    double m_width { 1 };

    // The height (magnitude on the Y axis), in CSS pixels (see [CSS21]), of the contact geometry of the pointer.
    // https://w3c.github.io/pointerevents/#dom-pointerevent-width
    double m_height { 1 };

    // The normalized pressure of the pointer input in the range of [0,1], where 0 and 1 represent the minimum and
    // maximum pressure the hardware is capable of detecting, respectively
    // https://w3c.github.io/pointerevents/#dom-pointerevent-pressure
    float m_pressure { 0 };

    // The normalized tangential pressure (also known as barrel pressure), typically set by an additional control
    // (e.g. a finger wheel on an airbrush stylus), of the pointer input in the range of [-1,1], where 0 is the
    // neutral position of the control
    // https://w3c.github.io/pointerevents/#dom-pointerevent-tangentialpressure
    float m_tangential_pressure { 0 };

    // The plane angle (in degrees, in the range of [-90,90]) between the Y-Z plane and the plane containing both the
    // transducer (e.g. pen/stylus) axis and the Y axis
    // https://w3c.github.io/pointerevents/#dom-pointerevent-tiltx
    WebIDL::Long m_tilt_x { 0 };

    // The plane angle (in degrees, in the range of [-90,90]) between the X-Z plane and the plane containing both the
    // transducer (e.g. pen/stylus) axis and the X axis
    // https://w3c.github.io/pointerevents/#dom-pointerevent-tilty
    WebIDL::Long m_tilt_y { 0 };

    // The clockwise rotation (in degrees, in the range of [0,359]) of a transducer (e.g. pen/stylus) around its own major axis
    // https://w3c.github.io/pointerevents/#dom-pointerevent-twist
    WebIDL::Long m_twist { 0 };

    // The altitude (in radians) of the transducer (e.g. pen/stylus), in the range [0,π/2] — where 0 is parallel to the surface
    // (X-Y plane), and π/2 is perpendicular to the surface
    // For hardware and platforms that do not report tilt or angle, the value MUST be π/2.
    // https://w3c.github.io/pointerevents/#dom-pointerevent-altitudeangle
    static constexpr double DEFAULT_ALTITUDE_ANGLE { AK::Pi<double> / 2 };
    double m_altitude_angle { DEFAULT_ALTITUDE_ANGLE };

    // The azimuth angle (in radians) of the transducer (e.g. pen/stylus), in the range [0, 2π] — where 0 represents a transducer
    // whose cap is pointing in the direction of increasing X values (point to "3 o'clock" if looking straight down) on the X-Y
    // plane, and the values progressively increase when going clockwise (π/2 at "6 o'clock", π at "9 o'clock", 3π/2 at "12 o'clock").
    // https://w3c.github.io/pointerevents/#dom-pointerevent-azimuthangle
    double m_azimuth_angle { 0 };

    // Indicates the device type that caused the event (mouse, pen, touch, etc.)
    // https://w3c.github.io/pointerevents/#dom-pointerevent-pointertype
    String m_pointer_type;

    // Indicates if the pointer represents the primary pointer of this pointer type
    // https://w3c.github.io/pointerevents/#dom-pointerevent-isprimary
    bool m_is_primary { false };

    // A unique identifier for the pointing device.
    // https://w3c.github.io/pointerevents/#dom-pointerevent-persistentdeviceid
    WebIDL::Long m_persistent_device_id { 0 };

    // https://w3c.github.io/pointerevents/#dom-pointerevent-getcoalescedevents
    AK::Vector<JS::NonnullGCPtr<PointerEvent>> m_coalesced_events;

    // https://w3c.github.io/pointerevents/#dom-pointerevent-getpredictedevents
    AK::Vector<JS::NonnullGCPtr<PointerEvent>> m_predicted_events;
};

}

namespace Web::DOM {

template<>
inline bool Event::fast_is<UIEvents::PointerEvent>() const { return is_pointer_event(); }

}
