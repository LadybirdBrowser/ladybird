/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibJS/Heap/HeapFunction.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWeb/WebDriver/Error.h>
#include <LibWeb/WebDriver/InputSource.h>
#include <LibWeb/WebDriver/Response.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-action-object
struct ActionObject {
    enum class Subtype {
        Pause,
        KeyUp,
        KeyDown,
        PointerUp,
        PointerDown,
        PointerMove,
        PointerCancel,
        Scroll,
    };

    enum class OriginType {
        Viewport,
        Pointer,
    };
    using Origin = Variant<OriginType, String>;

    struct PauseFields {
        Optional<AK::Duration> duration;
    };

    struct KeyFields {
        u32 value { 0 };
    };

    struct PointerFields {
        PointerInputSource::Subtype pointer_type { PointerInputSource::Subtype::Mouse };
        Optional<double> width;
        Optional<double> height;
        Optional<double> pressure;
        Optional<double> tangential_pressure;
        Optional<i32> tilt_x;
        Optional<i32> tilt_y;
        Optional<u32> twist;
        Optional<double> altitude_angle;
        Optional<double> azimuth_angle;
    };

    struct PointerUpDownFields : public PointerFields {
        UIEvents::MouseButton button { UIEvents::MouseButton::None };
    };

    struct PointerMoveFields : public PointerFields {
        Optional<AK::Duration> duration;
        Origin origin { OriginType::Viewport };
        CSSPixelPoint position;
    };

    struct PointerCancelFields {
        PointerInputSource::Subtype pointer_type { PointerInputSource::Subtype::Mouse };
    };

    struct ScrollFields {
        Origin origin { OriginType::Viewport };
        Optional<AK::Duration> duration;
        i64 x { 0 };
        i64 y { 0 };
        i64 delta_x { 0 };
        i64 delta_y { 0 };
    };

    ActionObject(String id, InputSourceType type, Subtype subtype);

    void set_pointer_type(PointerInputSource::Subtype);

    PauseFields& pause_fields() { return fields.get<PauseFields>(); }
    PauseFields const& pause_fields() const { return fields.get<PauseFields>(); }

    KeyFields& key_fields() { return fields.get<KeyFields>(); }
    KeyFields const& key_fields() const { return fields.get<KeyFields>(); }

    PointerUpDownFields& pointer_up_down_fields() { return fields.get<PointerUpDownFields>(); }
    PointerUpDownFields const& pointer_up_down_fields() const { return fields.get<PointerUpDownFields>(); }

    PointerMoveFields& pointer_move_fields() { return fields.get<PointerMoveFields>(); }
    PointerMoveFields const& pointer_move_fields() const { return fields.get<PointerMoveFields>(); }

    PointerCancelFields& pointer_cancel_fields() { return fields.get<PointerCancelFields>(); }
    PointerCancelFields const& pointer_cancel_fields() const { return fields.get<PointerCancelFields>(); }

    ScrollFields& scroll_fields() { return fields.get<ScrollFields>(); }
    ScrollFields const& scroll_fields() const { return fields.get<ScrollFields>(); }

    String id;
    InputSourceType type;
    Subtype subtype;

    using Fields = Variant<PauseFields, KeyFields, PointerUpDownFields, PointerMoveFields, PointerCancelFields, ScrollFields>;
    Fields fields;
};

// https://w3c.github.io/webdriver/#dfn-actions-options
struct ActionsOptions {
    Function<bool(JsonObject const&)> is_element_origin;
    Function<ErrorOr<JS::NonnullGCPtr<DOM::Element>, WebDriver::Error>(StringView)> get_element_origin;
};

using OnActionsComplete = JS::NonnullGCPtr<JS::HeapFunction<void(Web::WebDriver::Response)>>;

ErrorOr<Vector<Vector<ActionObject>>, WebDriver::Error> extract_an_action_sequence(InputState&, JsonValue const&, ActionsOptions const&);

JS::NonnullGCPtr<JS::Cell> dispatch_actions(InputState&, Vector<Vector<ActionObject>>, HTML::BrowsingContext&, ActionsOptions, OnActionsComplete);
ErrorOr<void, WebDriver::Error> dispatch_tick_actions(InputState&, ReadonlySpan<ActionObject>, AK::Duration, HTML::BrowsingContext&, ActionsOptions const&);
JS::NonnullGCPtr<JS::Cell> dispatch_list_of_actions(InputState&, Vector<ActionObject>, HTML::BrowsingContext&, ActionsOptions, OnActionsComplete);
JS::NonnullGCPtr<JS::Cell> dispatch_actions_for_a_string(Web::WebDriver::InputState&, String const& input_id, Web::WebDriver::InputSource&, StringView text, Web::HTML::BrowsingContext&, Web::WebDriver::OnActionsComplete);

}
