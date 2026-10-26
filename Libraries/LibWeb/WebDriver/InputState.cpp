/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/Math.h>
#include <AK/NeverDestroyed.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/WebDriver/Actions.h>
#include <LibWeb/WebDriver/InputState.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-browsing-context-input-state-map
// NB: The map is keyed by the navigable of the top-level browsing context, which a process hosting a frame of the tab
//     holds as well.
static HashMap<GC::RawPtr<HTML::Navigable>, InputState>& browsing_context_input_state_map()
{
    static NeverDestroyed<HashMap<GC::RawPtr<HTML::Navigable>, InputState>> map;
    return *map;
}

InputState::InputState() = default;
InputState::~InputState() = default;

// https://w3c.github.io/webdriver/#dfn-get-the-input-state
InputState& get_input_state(HTML::Navigable& top_level_traversable)
{
    // 1. Assert: browsing context is a top-level browsing context.
    VERIFY(top_level_traversable.is_top_level_traversable());

    // 2. Let input state map be session's browsing context input state map.
    // 3. If input state map does not contain browsing context, set input state map[browsing context] to create an input state.
    auto& input_state = browsing_context_input_state_map().ensure(top_level_traversable);

    // 4. Return input state map[browsing context].
    return input_state;
}

// https://w3c.github.io/webdriver/#dfn-reset-the-input-state
void reset_input_state(HTML::Navigable& top_level_traversable)
{
    // 1. Assert: browsing context is a top-level browsing context.
    VERIFY(top_level_traversable.is_top_level_traversable());

    // 2. Let input state map be session's browsing context input state map.
    // 3. If input state map[browsing context] exists, then remove input state map[browsing context].
    browsing_context_input_state_map().remove(top_level_traversable);
}

// NB: The browser treats this private snapshot as opaque. Sources carry their type and state; cancellation records
//     carry an id, a type (key or pointer), and the fields of the corresponding keyUp or pointerUp action. Pointer
//     positions use raw CSS fixed-point values so that moving between processes does not round their coordinates.
static StringView pointer_type_name(PointerInputSource::Subtype type)
{
    switch (type) {
    case PointerInputSource::Subtype::Mouse:
        return "mouse"sv;
    case PointerInputSource::Subtype::Pen:
        return "pen"sv;
    case PointerInputSource::Subtype::Touch:
        return "touch"sv;
    }
    VERIFY_NOT_REACHED();
}

static Error invalid_input_state()
{
    return Error::from_code(ErrorCode::UnknownError, "Invalid internal input state snapshot"sv);
}

template<typename T>
static void serialize_optional_field(JsonObject& object, StringView name, Optional<T> const& value)
{
    if (value.has_value())
        object.set(name, *value);
}

template<typename T>
static ErrorOr<void, Error> restore_optional_field(JsonObject const& object, StringView name, Optional<T>& output)
{
    auto value = object.get(name);
    if (!value.has_value())
        return {};
    Optional<T> number;
    if constexpr (IsFloatingPoint<T>) {
        number = value->get_number_with_precision_loss<T>();
        if (number.has_value() && !isfinite(*number))
            return invalid_input_state();
    } else {
        number = value->get_integer<T>();
    }
    if (!number.has_value())
        return invalid_input_state();
    output = *number;
    return {};
}

JsonObject serialize_input_state(InputState const& input_state)
{
    JsonArray sources;
    for (auto const& [id, source] : input_state.input_state_map) {
        JsonObject object;
        object.set("id"sv, id);
        source.visit(
            [&](NullInputSource const&) { object.set("type"sv, "none"sv); },
            [&](KeyInputSource const& key) {
                object.set("type"sv, "key"sv);
                JsonArray pressed;
                for (auto const& value : key.pressed)
                    pressed.must_append(value);
                object.set("pressed"sv, move(pressed));
                object.set("alt"sv, key.alt);
                object.set("ctrl"sv, key.ctrl);
                object.set("meta"sv, key.meta);
                object.set("shift"sv, key.shift);
            },
            [&](PointerInputSource const& pointer) {
                object.set("type"sv, "pointer"sv);
                object.set("pointerType"sv, pointer_type_name(pointer.subtype));
                object.set("pointerId"sv, pointer.pointer_id);
                object.set("pressed"sv, to_underlying(pointer.pressed));
                object.set("x"sv, pointer.position.x().raw_value());
                object.set("y"sv, pointer.position.y().raw_value());
            },
            [&](WheelInputSource const&) { object.set("type"sv, "wheel"sv); });
        sources.must_append(move(object));
    }

    JsonArray cancel;
    for (auto const& action : input_state.input_cancel_list) {
        JsonObject object;
        object.set("id"sv, action.id);
        if (action.subtype == ActionObject::Subtype::KeyUp) {
            VERIFY(action.type == InputSourceType::Key);
            object.set("type"sv, "key"sv);
            object.set("value"sv, action.key_fields().value);
        } else {
            VERIFY(action.type == InputSourceType::Pointer && action.subtype == ActionObject::Subtype::PointerUp);
            auto const& pointer = action.pointer_up_down_fields();
            object.set("type"sv, "pointer"sv);
            object.set("pointerType"sv, pointer_type_name(pointer.pointer_type));
            object.set("button"sv, to_underlying(pointer.button));
            serialize_optional_field(object, "width"sv, pointer.width);
            serialize_optional_field(object, "height"sv, pointer.height);
            serialize_optional_field(object, "pressure"sv, pointer.pressure);
            serialize_optional_field(object, "tangentialPressure"sv, pointer.tangential_pressure);
            serialize_optional_field(object, "tiltX"sv, pointer.tilt_x);
            serialize_optional_field(object, "tiltY"sv, pointer.tilt_y);
            serialize_optional_field(object, "twist"sv, pointer.twist);
            serialize_optional_field(object, "altitudeAngle"sv, pointer.altitude_angle);
            serialize_optional_field(object, "azimuthAngle"sv, pointer.azimuth_angle);
        }
        cancel.must_append(move(object));
    }

    JsonObject snapshot;
    snapshot.set("sources"sv, move(sources));
    snapshot.set("cancel"sv, move(cancel));
    return snapshot;
}

ErrorOr<void, Error> restore_input_state(HTML::Navigable& top_level_traversable, JsonValue const& snapshot)
{
    auto& input_state = get_input_state(top_level_traversable);
    if (!input_state.actions_queue.is_empty())
        return Error::from_code(ErrorCode::UnknownError, "Cannot restore input state during action dispatch"sv);

    InputState restored;
    if (!snapshot.is_null()) {
        if (!snapshot.is_object())
            return invalid_input_state();
        auto sources = snapshot.as_object().get_array("sources"sv);
        auto cancel = snapshot.as_object().get_array("cancel"sv);
        if (!sources.has_value() || !cancel.has_value())
            return invalid_input_state();

        for (auto const& value : sources->values()) {
            if (!value.is_object())
                return invalid_input_state();
            auto const& object = value.as_object();
            auto id = object.get_string("id"sv);
            auto type = object.get_string("type"sv);
            if (!id.has_value() || !type.has_value() || restored.input_state_map.contains(*id))
                return invalid_input_state();
            if (*type == "none"sv) {
                restored.input_state_map.set(*id, NullInputSource {});
            } else if (*type == "wheel"sv) {
                restored.input_state_map.set(*id, WheelInputSource {});
            } else if (*type == "key"sv) {
                auto pressed = object.get_array("pressed"sv);
                auto alt = object.get_bool("alt"sv);
                auto ctrl = object.get_bool("ctrl"sv);
                auto meta = object.get_bool("meta"sv);
                auto shift = object.get_bool("shift"sv);
                if (!pressed.has_value() || !alt.has_value() || !ctrl.has_value() || !meta.has_value() || !shift.has_value())
                    return invalid_input_state();
                KeyInputSource source;
                for (auto const& key : pressed->values()) {
                    if (!key.is_string())
                        return invalid_input_state();
                    source.pressed.set(key.as_string());
                }
                source.alt = *alt;
                source.ctrl = *ctrl;
                source.meta = *meta;
                source.shift = *shift;
                restored.input_state_map.set(*id, move(source));
            } else if (*type == "pointer"sv) {
                auto pointer_type = object.get_string("pointerType"sv);
                auto subtype = pointer_type.has_value() ? pointer_input_source_subtype_from_string(*pointer_type) : Optional<PointerInputSource::Subtype> {};
                auto pointer_id = object.get_u32("pointerId"sv);
                auto pressed = object.get_u32("pressed"sv);
                auto x = object.get_i32("x"sv);
                auto y = object.get_i32("y"sv);
                if (!subtype.has_value() || !pointer_id.has_value() || !pressed.has_value() || *pressed > 31 || !x.has_value() || !y.has_value())
                    return invalid_input_state();
                PointerInputSource source { restored, *subtype };
                source.pointer_id = *pointer_id;
                source.pressed = static_cast<UIEvents::MouseButton>(*pressed);
                source.position = { CSSPixels::from_raw(*x), CSSPixels::from_raw(*y) };
                restored.input_state_map.set(*id, move(source));
            } else {
                return invalid_input_state();
            }
        }

        for (auto const& value : cancel->values()) {
            if (!value.is_object())
                return invalid_input_state();
            auto const& object = value.as_object();
            auto id = object.get_string("id"sv);
            auto type = object.get_string("type"sv);
            if (!id.has_value() || !type.has_value())
                return invalid_input_state();
            // NB: Classic commands can remove a temporary source while its cancellation record remains in the list.
            if (*type == "key"sv) {
                auto key = object.get_u32("value"sv);
                if (!key.has_value())
                    return invalid_input_state();
                ActionObject action { *id, InputSourceType::Key, ActionObject::Subtype::KeyUp };
                action.key_fields().value = *key;
                restored.input_cancel_list.append(move(action));
            } else if (*type == "pointer"sv) {
                auto pointer_type = object.get_string("pointerType"sv);
                auto subtype = pointer_type.has_value() ? pointer_input_source_subtype_from_string(*pointer_type) : Optional<PointerInputSource::Subtype> {};
                auto button = object.get_u32("button"sv);
                if (!subtype.has_value() || !button.has_value() || !(*button == 0 || *button == 1 || *button == 2 || *button == 4 || *button == 8 || *button == 16))
                    return invalid_input_state();
                ActionObject action { *id, InputSourceType::Pointer, ActionObject::Subtype::PointerUp };
                action.set_pointer_type(*subtype);
                auto& pointer = action.pointer_up_down_fields();
                pointer.button = static_cast<UIEvents::MouseButton>(*button);
                TRY(restore_optional_field(object, "width"sv, pointer.width));
                TRY(restore_optional_field(object, "height"sv, pointer.height));
                TRY(restore_optional_field(object, "pressure"sv, pointer.pressure));
                TRY(restore_optional_field(object, "tangentialPressure"sv, pointer.tangential_pressure));
                TRY(restore_optional_field(object, "tiltX"sv, pointer.tilt_x));
                TRY(restore_optional_field(object, "tiltY"sv, pointer.tilt_y));
                TRY(restore_optional_field(object, "twist"sv, pointer.twist));
                TRY(restore_optional_field(object, "altitudeAngle"sv, pointer.altitude_angle));
                TRY(restore_optional_field(object, "azimuthAngle"sv, pointer.azimuth_angle));
                restored.input_cancel_list.append(move(action));
            } else {
                return invalid_input_state();
            }
        }
    }

    input_state.input_state_map = move(restored.input_state_map);
    input_state.input_cancel_list = move(restored.input_cancel_list);
    return {};
}

}
