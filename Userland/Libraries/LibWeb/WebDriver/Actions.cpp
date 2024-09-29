/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <AK/GenericShorthands.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Math.h>
#include <AK/Utf8View.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebDriver/Actions.h>
#include <LibWeb/WebDriver/ElementReference.h>
#include <LibWeb/WebDriver/InputState.h>
#include <LibWeb/WebDriver/Properties.h>

namespace Web::WebDriver {

static Optional<ActionObject::Subtype> action_object_subtype_from_string(StringView action_subtype)
{
    if (action_subtype == "pause"sv)
        return ActionObject::Subtype::Pause;
    if (action_subtype == "keyUp"sv)
        return ActionObject::Subtype::KeyUp;
    if (action_subtype == "keyDown"sv)
        return ActionObject::Subtype::KeyDown;
    if (action_subtype == "pointerUp"sv)
        return ActionObject::Subtype::PointerUp;
    if (action_subtype == "pointerDown"sv)
        return ActionObject::Subtype::PointerDown;
    if (action_subtype == "pointerMove"sv)
        return ActionObject::Subtype::PointerMove;
    if (action_subtype == "pointerCancel"sv)
        return ActionObject::Subtype::PointerCancel;
    if (action_subtype == "scroll"sv)
        return ActionObject::Subtype::Scroll;
    return {};
}

static ActionObject::Fields fields_from_subtype(ActionObject::Subtype subtype)
{
    switch (subtype) {
    case ActionObject::Subtype::Pause:
        return ActionObject::PauseFields {};

    case ActionObject::Subtype::KeyUp:
    case ActionObject::Subtype::KeyDown:
        return ActionObject::KeyFields {};

    case ActionObject::Subtype::PointerUp:
    case ActionObject::Subtype::PointerDown:
        return ActionObject::PointerUpDownFields {};

    case ActionObject::Subtype::PointerMove:
        return ActionObject::PointerMoveFields {};

    case ActionObject::Subtype::PointerCancel:
        return ActionObject::PointerCancelFields {};

    case ActionObject::Subtype::Scroll:
        return ActionObject::ScrollFields {};
    }

    VERIFY_NOT_REACHED();
}

ActionObject::ActionObject(String id, InputSourceType type, Subtype subtype)
    : id(move(id))
    , type(type)
    , subtype(subtype)
    , fields(fields_from_subtype(subtype))
{
}

void ActionObject::set_pointer_type(PointerInputSource::Subtype pointer_type)
{
    fields.visit(
        [&](OneOf<PointerUpDownFields, PointerMoveFields, PointerCancelFields> auto& fields) {
            fields.pointer_type = pointer_type;
        },
        [](auto const&) { VERIFY_NOT_REACHED(); });
}

static Optional<ActionObject::Origin> determine_origin(ActionsOptions const& actions_options, Optional<JsonValue const&> const& origin)
{
    if (!origin.has_value())
        return ActionObject::OriginType::Viewport;

    if (origin->is_string()) {
        if (origin->as_string() == "viewport"sv)
            return ActionObject::OriginType::Viewport;
        if (origin->as_string() == "pointer"sv)
            return ActionObject::OriginType::Pointer;
    }

    if (origin->is_object()) {
        if (actions_options.is_element_origin(origin->as_object()))
            return MUST(String::from_byte_string(extract_web_element_reference(origin->as_object())));
    }

    return {};
}

// https://w3c.github.io/webdriver/#dfn-get-coordinates-relative-to-an-origin
static ErrorOr<CSSPixelPoint, WebDriver::Error> get_coordinates_relative_to_origin(PointerInputSource const& source, CSSPixelPoint offset, CSSPixelRect viewport, ActionObject::Origin const& origin, ActionsOptions const& actions_options)
{
    // 1. Run the substeps of the first matching value of origin
    auto coordinates = TRY(origin.visit(
        [&](ActionObject::OriginType origin) -> ErrorOr<CSSPixelPoint, WebDriver::Error> {
            switch (origin) {
            // "viewport"
            case ActionObject::OriginType::Viewport:
                // 1. Let x equal x offset and y equal y offset.
                return offset;

            // "pointer"
            case ActionObject::OriginType::Pointer:
                // 1. Let start x be equal to the x property of source.
                // 2. Let start y be equal to the y property of source.
                // 3. Let x equal start x + x offset and y equal start y + y offset.
                return source.position.translated(offset);
            }

            VERIFY_NOT_REACHED();
        },
        [&](String const& origin) -> ErrorOr<CSSPixelPoint, WebDriver::Error> {
            // Otherwise
            // 1. Let element be the result of trying to run actions options' get element origin steps with origin and
            //    browsing context.
            // 2. If element is null, return error with error code no such element.
            auto element = TRY(actions_options.get_element_origin(origin));

            // 3. Let x element and y element be the result of calculating the in-view center point of element.
            auto position = in_view_center_point(element, viewport);

            // 4. Let x equal x element + x offset, and y equal y element + y offset.
            return position.translated(offset);
        }));

    // 2. Return (x, y)
    return coordinates;
}

// https://w3c.github.io/webdriver/#dfn-process-pointer-parameters
struct PointerParameters {
    PointerInputSource::Subtype pointer_type { PointerInputSource::Subtype::Mouse };
};
static ErrorOr<PointerParameters, WebDriver::Error> process_pointer_parameters(Optional<JsonValue const&> const& parameters_data)
{
    // 1. Let parameters be the default pointer parameters.
    PointerParameters parameters;

    // 2. If parameters data is undefined, return success with data parameters.
    if (!parameters_data.has_value())
        return parameters;

    // 3. If parameters data is not an Object, return error with error code invalid argument.
    if (!parameters_data->is_object())
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Property 'parameters' is not an Object");

    // 4. Let pointer type be the result of getting a property named "pointerType" from parameters data.
    auto pointer_type = TRY(get_optional_property(parameters_data->as_object(), "pointerType"sv));

    // 5. If pointer type is not undefined:
    if (pointer_type.has_value()) {
        // 1. If pointer type does not have one of the values "mouse", "pen", or "touch", return error with error code
        //    invalid argument.
        auto parsed_pointer_type = pointer_input_source_subtype_from_string(*pointer_type);

        if (!parsed_pointer_type.has_value())
            return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Property 'pointerType' must be one of 'mouse', 'pen', or 'touch'");

        // 2. Set the pointerType property of parameters to pointer type.
        parameters.pointer_type = *parsed_pointer_type;
    }

    // 6. Return success with data parameters.
    return parameters;
}

// https://w3c.github.io/webdriver/#dfn-process-a-pause-action
static ErrorOr<void, WebDriver::Error> process_pause_action(JsonObject const& action_item, ActionObject& action)
{
    // 1. Let duration be the result of getting the property "duration" from action item.
    // 2. If duration is not undefined and duration is not an Integer greater than or equal to 0, return error with error code invalid argument.
    // 3. Set the duration property of action to duration.
    if (auto duration = TRY(get_optional_property_with_limits<i64>(action_item, "duration"sv, 0, {})); duration.has_value())
        action.pause_fields().duration = AK::Duration::from_milliseconds(*duration);

    // 4. Return success with data action.
    return {};
}

// https://w3c.github.io/webdriver/#dfn-process-a-null-action
static ErrorOr<ActionObject, WebDriver::Error> process_null_action(String id, JsonObject const& action_item)
{
    // 1. Let subtype be the result of getting a property named "type" from action item.
    auto subtype = action_object_subtype_from_string(TRY(get_property(action_item, "type"sv)));

    // 2. If subtype is not "pause", return error with error code invalid argument.
    if (subtype != ActionObject::Subtype::Pause)
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Property 'type' must be 'pause'");

    // 3. Let action be an action object constructed with arguments id, "none", and subtype.
    ActionObject action { move(id), InputSourceType::None, *subtype };

    // 4. Let result be the result of trying to process a pause action with arguments action item and action.
    TRY(process_pause_action(action_item, action));

    // 5. Return result.
    return action;
}

// https://w3c.github.io/webdriver/#dfn-process-a-key-action
static ErrorOr<ActionObject, WebDriver::Error> process_key_action(String id, JsonObject const& action_item)
{
    using enum ActionObject::Subtype;

    // 1. Let subtype be the result of getting a property named "type" from action item.
    auto subtype = action_object_subtype_from_string(TRY(get_property(action_item, "type"sv)));

    // 2. If subtype is not one of the values "keyUp", "keyDown", or "pause", return an error with error code invalid argument.
    if (!first_is_one_of(subtype, KeyUp, KeyDown, Pause))
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Property 'type' must be one of 'keyUp', 'keyDown', or 'pause'");

    // 3. Let action be an action object constructed with arguments id, "key", and subtype.
    ActionObject action { move(id), InputSourceType::Key, *subtype };

    // 4. If subtype is "pause", let result be the result of trying to process a pause action with arguments action
    //    item and action, and return result.
    if (subtype == Pause) {
        TRY(process_pause_action(action_item, action));
        return action;
    }

    // 5. Let key be the result of getting a property named "value" from action item.
    auto key = TRY(get_property(action_item, "value"sv));

    // 6. If key is not a String containing a single unicode code point [or grapheme cluster?] return error with error
    //    code invalid argument.
    if (Utf8View { key }.length() != 1) {
        // FIXME: The spec seems undecided on whether grapheme clusters should be supported. Update this step to check
        //        for graphemes if we end up needing to support them.
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Property 'value' must be a single code point");
    }

    // 7. Set the value property on action to key.
    action.key_fields().value = MUST(String::from_byte_string(key));

    // 8. Return success with data action.
    return action;
}

// Common steps between:
// https://w3c.github.io/webdriver/#dfn-process-a-pointer-up-or-pointer-down-action
// https://w3c.github.io/webdriver/#dfn-process-a-pointer-move-action
static ErrorOr<void, WebDriver::Error> process_pointer_action_common(JsonObject const& action_item, ActionObject::PointerFields& fields)
{
    // 4. Let width be the result of getting the property width from action item.
    // 5. If width is not undefined and width is not a Number greater than or equal to 0 return error with error code invalid argument.
    // 6. Set the width property of action to width.
    fields.width = TRY(get_optional_property_with_limits<double>(action_item, "width"sv, 0.0, {}));

    // 7. Let height be the result of getting the property height from action item.
    // 8. If height is not undefined and height is not a Number greater than or equal to 0 return error with error code invalid argument.
    // 9. Set the height property of action to height.
    fields.height = TRY(get_optional_property_with_limits<double>(action_item, "height"sv, 0.0, {}));

    // 10. Let pressure be the result of getting the property pressure from action item.
    // 11. If pressure is not undefined and pressure is not a Number greater than or equal to 0 and less than or equal to 1 return error with error code invalid argument.
    // 12. Set the pressure property of action to pressure.
    fields.pressure = TRY(get_optional_property_with_limits<double>(action_item, "pressure"sv, 0.0, 1.0));

    // 13. Let tangentialPressure be the result of getting the property tangentialPressure from action item.
    // 14. If tangentialPressure is not undefined and tangentialPressure is not a Number greater than or equal to -1 and less than or equal to 1 return error with error code invalid argument.
    // 15. Set the tangentialPressure property of action to tangentialPressure.
    fields.tangential_pressure = TRY(get_optional_property_with_limits<double>(action_item, "tangentialPressure"sv, -1.0, 1.0));

    // 16. Let tiltX be the result of getting the property tiltX from action item.
    // 17. If tiltX is not undefined and tiltX is not an Integer greater than or equal to -90 and less than or equal to 90 return error with error code invalid argument.
    // 18. Set the tiltX property of action to tiltX.
    fields.tilt_x = TRY(get_optional_property_with_limits<i32>(action_item, "tiltX"sv, -90, 90));

    // 19. Let tiltY be the result of getting the property tiltY from action item.
    // 20. If tiltY is not undefined and tiltY is not an Integer greater than or equal to -90 and less than or equal to 90 return error with error code invalid argument.
    // 21. Set the tiltY property of action to tiltY.
    fields.tilt_y = TRY(get_optional_property_with_limits<i32>(action_item, "tiltY"sv, -90, 90));

    // 22. Let twist be the result of getting the property twist from action item.
    // 23. If twist is not undefined and twist is not an Integer greater than or equal to 0 and less than or equal to 359 return error with error code invalid argument.
    // 24. Set the twist property of action to twist.
    fields.twist = TRY(get_optional_property_with_limits<u32>(action_item, "twist"sv, 0, 359));

    // 25. Let altitudeAngle be the result of getting the property altitudeAngle from action item.
    // 26. If altitudeAngle is not undefined and altitudeAngle is not a Number greater than or equal to 0 and less than or equal to π/2 return error with error code invalid argument.
    // 27. Set the altitudeAngle property of action to altitudeAngle.
    fields.altitude_angle = TRY(get_optional_property_with_limits<double>(action_item, "altitudeAngle"sv, 0.0, AK::Pi<double> / 2.0));

    // 28. Let azimuthAngle be the result of getting the property azimuthAngle from action item.
    // 29. If azimuthAngle is not undefined and azimuthAngle is not a Number greater than or equal to 0 and less than or equal to 2π return error with error code invalid argument.
    // 30. Set the azimuthAngle property of action to azimuthAngle.
    fields.azimuth_angle = TRY(get_optional_property_with_limits<double>(action_item, "azimuthAngle"sv, 0.0, AK::Pi<double> * 2.0));

    // 31. Return success with data null.
    return {};
}

// https://w3c.github.io/webdriver/#dfn-process-a-pointer-up-or-pointer-down-action
static ErrorOr<void, WebDriver::Error> process_pointer_up_or_down_action(JsonObject const& action_item, ActionObject& action)
{
    auto& fields = action.pointer_up_down_fields();

    // 1. Let button be the result of getting the property button from action item.
    // 2. If button is not an Integer greater than or equal to 0 return error with error code invalid argument.
    // 3. Set the button property of action to button.
    fields.button = UIEvents::button_code_to_mouse_button(TRY(get_property_with_limits<i16>(action_item, "button"sv, 0, {})));

    return process_pointer_action_common(action_item, fields);
}

// https://w3c.github.io/webdriver/#dfn-process-a-pointer-move-action
static ErrorOr<void, WebDriver::Error> process_pointer_move_action(JsonObject const& action_item, ActionObject& action, ActionsOptions const& actions_options)
{
    auto& fields = action.pointer_move_fields();

    // 1. Let duration be the result of getting the property duration from action item.
    // 2. If duration is not undefined and duration is not an Integer greater than or equal to 0, return error with error code invalid argument.
    // 3. Set the duration property of action to duration.
    if (auto duration = TRY(get_optional_property_with_limits<i64>(action_item, "duration"sv, 0, {})); duration.has_value())
        fields.duration = AK::Duration::from_milliseconds(*duration);

    // 4. Let origin be the result of getting the property origin from action item.
    // 5. If origin is undefined let origin equal "viewport".
    auto origin = determine_origin(actions_options, action_item.get("origin"sv));

    // 6. If origin is not equal to "viewport" or "pointer", and actions options is element origin steps given origin
    //    return false, return error with error code invalid argument.
    if (!origin.has_value())
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Property 'origin' must be 'viewport', 'pointer', or an element origin");

    // 7. Set the origin property of action to origin.
    fields.origin = origin.release_value();

    // 8. Let x be the result of getting the property x from action item.
    // 9. If x is not an Integer, return error with error code invalid argument.
    // 10. Set the x property of action to x.
    fields.position.set_x(TRY(get_property<i32>(action_item, "x"sv)));

    // 11. Let y be the result of getting the property y from action item.
    // 12. If y is not an Integer, return error with error code invalid argument.
    // 13. Set the y property of action to y.
    fields.position.set_y(TRY(get_property<i32>(action_item, "y"sv)));

    return process_pointer_action_common(action_item, fields);
}

// https://w3c.github.io/webdriver/#dfn-process-a-pointer-action
static ErrorOr<ActionObject, WebDriver::Error> process_pointer_action(String id, PointerParameters const& parameters, JsonObject const& action_item, ActionsOptions const& actions_options)
{
    using enum ActionObject::Subtype;

    // 1. Let subtype be the result of getting a property named "type" from action item.
    auto subtype = action_object_subtype_from_string(TRY(get_property(action_item, "type"sv)));

    // 2. If subtype is not one of the values "pause", "pointerUp", "pointerDown", "pointerMove", or "pointerCancel", return an error with error code invalid argument.
    if (!first_is_one_of(subtype, Pause, PointerUp, PointerDown, PointerMove, PointerCancel))
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Property 'type' must be one of 'pause', 'pointerUp', 'pointerDown', 'pointerMove', or 'pointerCancel'");

    // 3. Let action be an action object constructed with arguments id, "pointer", and subtype.
    ActionObject action { move(id), InputSourceType::Pointer, *subtype };

    // 4. If subtype is "pause", let result be the result of trying to process a pause action with arguments action
    //    item, action, and actions options, and return result.
    if (subtype == Pause) {
        TRY(process_pause_action(action_item, action));
        return action;
    }

    // 5. Set the pointerType property of action equal to the pointerType property of parameters.
    action.set_pointer_type(parameters.pointer_type);

    // 6. If subtype is "pointerUp" or "pointerDown", process a pointer up or pointer down action with arguments action
    //    item and action. If doing so results in an error, return that error.
    if (subtype == PointerUp || subtype == PointerDown) {
        TRY(process_pointer_up_or_down_action(action_item, action));
    }

    // 7. If subtype is "pointerMove" process a pointer move action with arguments action item, action, and actions
    //    options. If doing so results in an error, return that error.
    else if (subtype == PointerMove) {
        TRY(process_pointer_move_action(action_item, action, actions_options));
    }

    // 8. If subtype is "pointerCancel" process a pointer cancel action. If doing so results in an error, return that error.
    else if (subtype == PointerCancel) {
        // FIXME: There are no spec steps to "process a pointer cancel action" yet.
        return WebDriver::Error::from_code(WebDriver::ErrorCode::UnsupportedOperation, "pointerCancel events not implemented"sv);
    }

    // 9. Return success with data action.
    return action;
}

// https://w3c.github.io/webdriver/#dfn-process-a-wheel-action
static ErrorOr<ActionObject, WebDriver::Error> process_wheel_action(String id, JsonObject const& action_item, ActionsOptions const& actions_options)
{
    using enum ActionObject::Subtype;

    // 1. Let subtype be the result of getting a property named "type" from action item.
    auto subtype = action_object_subtype_from_string(TRY(get_property(action_item, "type"sv)));

    // 2. If subtype is not the value "pause", or "scroll", return an error with error code invalid argument.
    if (!first_is_one_of(subtype, Pause, Scroll))
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Property 'type' must be one of 'pause' or 'scroll'");

    // 3. Let action be an action object constructed with arguments id, "wheel", and subtype.
    ActionObject action { move(id), InputSourceType::Wheel, *subtype };

    // 4. If subtype is "pause", let result be the result of trying to process a pause action with arguments action
    //    item and action, and return result.
    if (subtype == Pause) {
        TRY(process_pause_action(action_item, action));
        return action;
    }

    auto& fields = action.scroll_fields();

    // 5. Let duration be the result of getting a property named "duration" from action item.
    // 6. If duration is not undefined and duration is not an Integer greater than or equal to 0, return error with error code invalid argument.
    // 7. Set the duration property of action to duration.
    if (auto duration = TRY(get_optional_property_with_limits<i64>(action_item, "duration"sv, 0, {})); duration.has_value())
        fields.duration = AK::Duration::from_milliseconds(*duration);

    // 8. Let origin be the result of getting the property origin from action item.
    // 9. If origin is undefined let origin equal "viewport".
    auto origin = determine_origin(actions_options, action_item.get("origin"sv));

    // 10. If origin is not equal to "viewport", or actions options' is element origin steps given origin return false,
    //     return error with error code invalid argument.
    if (!origin.has_value() || origin == ActionObject::OriginType::Pointer)
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Property 'origin' must be 'viewport' or an element origin");

    // 11. Set the origin property of action to origin.
    fields.origin = origin.release_value();

    // 12. Let x be the result of getting the property x from action item.
    // 13. If x is not an Integer, return error with error code invalid argument.
    // 14. Set the x property of action to x.
    fields.x = TRY(get_property<i64>(action_item, "x"sv));

    // 15. Let y be the result of getting the property y from action item.
    // 16. If y is not an Integer, return error with error code invalid argument.
    // 17. Set the y property of action to y.
    fields.y = TRY(get_property<i64>(action_item, "y"sv));

    // 18. Let deltaX be the result of getting the property deltaX from action item.
    // 19. If deltaX is not an Integer, return error with error code invalid argument.
    // 20. Set the deltaX property of action to deltaX.
    fields.delta_x = TRY(get_property<i64>(action_item, "deltaX"sv));

    // 21. Let deltaY be the result of getting the property deltaY from action item.
    // 22. If deltaY is not an Integer, return error with error code invalid argument.
    // 23. Set the deltaY property of action to deltaY.
    fields.delta_y = TRY(get_property<i64>(action_item, "deltaY"sv));

    // 24. Return success with data action.
    return action;
}

// https://w3c.github.io/webdriver/#dfn-process-an-input-source-action-sequence
static ErrorOr<Vector<ActionObject>, WebDriver::Error> process_input_source_action_sequence(InputState& input_state, JsonValue const& action_sequence, ActionsOptions const& actions_options)
{
    // 1. Let type be the result of getting a property named "type" from action sequence.
    auto type = input_source_type_from_string(TRY(get_property(action_sequence, "type"sv)));

    // 2. If type is not "key", "pointer", "wheel", or "none", return an error with error code invalid argument.
    if (!type.has_value())
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Property 'type' must be one of 'key', 'pointer', 'wheel', or 'none'");

    // 3. Let id be the result of getting the property "id" from action sequence.
    // 4. If id is undefined or is not a String, return error with error code invalid argument.
    auto const id = MUST(String::from_byte_string(TRY(get_property(action_sequence, "id"sv))));

    // 5. If type is equal to "pointer", let parameters data be the result of getting the property "parameters" from
    //    action sequence. Then let parameters be the result of trying to process pointer parameters with argument
    //    parameters data.
    Optional<PointerParameters> parameters;
    Optional<PointerInputSource::Subtype> subtype;

    if (type == InputSourceType::Pointer) {
        parameters = TRY(process_pointer_parameters(action_sequence.as_object().get("parameters"sv)));
        subtype = parameters->pointer_type;
    }

    // 6. Let source be the result of trying to get or create an input source given input state, type and id.
    auto const& source = *TRY(get_or_create_input_source(input_state, *type, id, subtype));

    // 7. If parameters is not undefined, then if its pointerType property is not equal to source's subtype property,
    //    return an error with error code invalid argument.
    if (auto const* pointer_input_source = source.get_pointer<PointerInputSource>(); pointer_input_source && parameters.has_value()) {
        if (parameters->pointer_type != pointer_input_source->subtype)
            return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Invalid 'pointerType' property");
    }

    // 8. Let action items be the result of getting a property named "actions" from action sequence.
    // 9. If action items is not an Array, return error with error code invalid argument.
    auto const& action_items = *TRY(get_property<JsonArray const*>(action_sequence, "actions"sv));

    // 10. Let actions be a new list.
    Vector<ActionObject> actions;

    // 11. For each action item in action items:
    TRY(action_items.try_for_each([&](auto const& action_item) -> ErrorOr<void, WebDriver::Error> {
        // 1. If action item is not an Object return error with error code invalid argument.
        if (!action_item.is_object())
            return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Property 'actions' item is not an Object");

        auto action = TRY([&]() {
            switch (*type) {
            // 2. If type is "none" let action be the result of trying to process a null action with parameters id, and
            //    action item.
            case InputSourceType::None:
                return process_null_action(id, action_item.as_object());

            // 3. Otherwise, if type is "key" let action be the result of trying to process a key action with parameters
            //    id, and action item.
            case InputSourceType::Key:
                return process_key_action(id, action_item.as_object());

            // 4. Otherwise, if type is "pointer" let action be the result of trying to process a pointer action with
            //    parameters id, parameters, action item, and actions options.
            case InputSourceType::Pointer:
                return process_pointer_action(id, *parameters, action_item.as_object(), actions_options);

            // 5. Otherwise, if type is "wheel" let action be the result of trying to process a wheel action with
            //    parameters id, and action item, and actions options.
            case InputSourceType::Wheel:
                return process_wheel_action(id, action_item.as_object(), actions_options);
            }

            VERIFY_NOT_REACHED();
        }());

        // 6. Append action to actions.
        actions.append(move(action));
        return {};
    }));

    // 12. Return success with data actions.
    return actions;
}

// https://w3c.github.io/webdriver/#dfn-extract-an-action-sequence
ErrorOr<Vector<Vector<ActionObject>>, WebDriver::Error> extract_an_action_sequence(InputState& input_state, JsonValue const& parameters, ActionsOptions const& actions_options)
{
    // 1. Let actions be the result of getting a property named "actions" from parameters.
    // 2. If actions is undefined or is not an Array, return error with error code invalid argument.
    auto const& actions = *TRY(get_property<JsonArray const*>(parameters, "actions"sv));

    // 3. Let actions by tick be an empty List.
    Vector<Vector<ActionObject>> actions_by_tick;

    // 4. For each value action sequence corresponding to an indexed property in actions:
    TRY(actions.try_for_each([&](auto const& action_sequence) -> ErrorOr<void, WebDriver::Error> {
        // 1. Let source actions be the result of trying to process an input source action sequence given input state,
        //    action sequence, and actions options.
        auto source_actions = TRY(process_input_source_action_sequence(input_state, action_sequence, actions_options));

        // 2. For each action in source actions:
        for (auto [i, action] : enumerate(source_actions)) {
            // 1. Let i be the zero-based index of action in source actions.
            // 2. If the length of actions by tick is less than i + 1, append a new List to actions by tick.
            if (actions_by_tick.size() < (i + 1))
                actions_by_tick.resize(i + 1);

            // 3. Append action to the List at index i in actions by tick.
            actions_by_tick[i].append(move(action));
        }

        return {};
    }));

    // 5. Return success with data actions by tick.
    return actions_by_tick;
}

// https://w3c.github.io/webdriver/#dfn-computing-the-tick-duration
static AK::Duration compute_tick_duration(ReadonlySpan<ActionObject> tick_actions)
{
    // 1. Let max duration be 0.
    auto max_duration = AK::Duration::zero();

    // 2. For each action object in tick actions:
    for (auto const& action_object : tick_actions) {
        // 1. let duration be undefined.
        Optional<AK::Duration> duration;

        // 2. If action object has subtype property set to "pause" or action object has type property set to "pointer"
        //    and subtype property set to "pointerMove", or action object has type property set to "wheel" and subtype
        //    property set to "scroll", let duration be equal to the duration property of action object.
        action_object.fields.visit(
            [&](OneOf<ActionObject::PauseFields, ActionObject::PointerMoveFields, ActionObject::ScrollFields> auto const& fields) {
                duration = fields.duration;
            },
            [](auto const&) {});

        // 3. If duration is not undefined, and duration is greater than max duration, let max duration be equal to duration.
        if (duration.has_value())
            max_duration = max(max_duration, *duration);
    }

    // 3. Return max duration.
    return max_duration;
}

// https://w3c.github.io/webdriver/#dfn-dispatch-a-pause-action
static void dispatch_pause_action()
{
    // 1. Return success with data null.
}

// https://w3c.github.io/webdriver/#dfn-dispatch-a-pointerdown-action
static ErrorOr<void, WebDriver::Error> dispatch_pointer_down_action(ActionObject::PointerUpDownFields const& action_object, PointerInputSource& source, GlobalKeyState const& global_key_state, HTML::BrowsingContext& browsing_context)
{
    // 1. Let pointerType be equal to action object's pointerType property.
    auto pointer_type = action_object.pointer_type;

    // 2. Let button be equal to action object's button property.
    auto button = action_object.button;

    // 3. If the source's pressed property contains button return success with data null.
    if (has_flag(source.pressed, button))
        return {};

    // 4. Let x be equal to source's x property.
    // 5. Let y be equal to source's y property.
    auto position = browsing_context.page().css_to_device_point(source.position);

    // 6. Add button to the set corresponding to source's pressed property, and let buttons be the resulting value of
    //    that property.
    auto buttons = (source.pressed |= button);

    // 7. Let width be equal to action object's width property.
    // 8. Let height be equal to action object's height property.
    // 9. Let pressure be equal to action object's pressure property.
    // 10. Let tangentialPressure be equal to action object's tangentialPressure property.
    // 11. Let tiltX be equal to action object's tiltX property.
    // 12. Let tiltY be equal to action object's tiltY property.
    // 13. Let twist be equal to action object's twist property.
    // 14. Let altitudeAngle be equal to action object's altitudeAngle property.
    // 15. Let azimuthAngle be equal to action object's azimuthAngle property.

    // 16. Perform implementation-specific action dispatch steps on browsing context equivalent to pressing the button
    //     numbered button on the pointer with pointerId equal to source's pointerId, having type pointerType at viewport
    //     x coordinate x, viewport y coordinate y, width, height, pressure, tangentialPressure, tiltX, tiltY, twist,
    //     altitudeAngle, azimuthAngle, with buttons buttons depressed in accordance with the requirements of [UI-EVENTS]
    //     and [POINTER-EVENTS]. set ctrlKey, shiftKey, altKey, and metaKey equal to the corresponding items in global
    //     key state. Type specific properties for the pointer that are not exposed through the webdriver API must be
    //     set to the default value specified for hardware that doesn't support that property.
    switch (pointer_type) {
    case PointerInputSource::Subtype::Mouse:
        browsing_context.page().handle_mousedown(position, position, button, buttons, global_key_state.modifiers());
        break;
    case PointerInputSource::Subtype::Pen:
        return WebDriver::Error::from_code(WebDriver::ErrorCode::UnsupportedOperation, "Pen events not implemented"sv);
    case PointerInputSource::Subtype::Touch:
        return WebDriver::Error::from_code(WebDriver::ErrorCode::UnsupportedOperation, "Touch events not implemented"sv);
    }

    // 17. Return success with data null.
    return {};
}

// https://w3c.github.io/webdriver/#dfn-dispatch-a-pointerup-action
static ErrorOr<void, WebDriver::Error> dispatch_pointer_up_action(ActionObject::PointerUpDownFields const& action_object, PointerInputSource& source, GlobalKeyState const& global_key_state, HTML::BrowsingContext& browsing_context)
{
    // 1. Let pointerType be equal to action object's pointerType property.
    auto pointer_type = action_object.pointer_type;

    // 2. Let button be equal to action object's button property.
    auto button = action_object.button;

    // 3. If the source's pressed property does not contain button, return success with data null.
    if (!has_flag(source.pressed, button))
        return {};

    // 4. Let x be equal to source's x property.
    // 5. Let y be equal to source's y property.
    auto position = browsing_context.page().css_to_device_point(source.position);

    // 6. Remove button from the set corresponding to source's pressed property, and let buttons be the resulting value
    //    of that property.
    auto buttons = (source.pressed &= ~button);

    // 7. Perform implementation-specific action dispatch steps on browsing context equivalent to releasing the button
    //    numbered button on the pointer with pointerId equal to input source's pointerId, having type pointerType at
    //    viewport x coordinate x, viewport y coordinate y, with buttons buttons depressed, in accordance with the
    //    requirements of [UI-EVENTS] and [POINTER-EVENTS]. The generated events must set ctrlKey, shiftKey, altKey,
    //    and metaKey equal to the corresponding items in global key state. Type specific properties for the pointer
    //    that are not exposed through the webdriver API must be set to the default value specified for hardware that
    //    doesn't support that property.
    switch (pointer_type) {
    case PointerInputSource::Subtype::Mouse:
        browsing_context.page().handle_mouseup(position, position, button, buttons, global_key_state.modifiers());
        break;
    case PointerInputSource::Subtype::Pen:
        return WebDriver::Error::from_code(WebDriver::ErrorCode::UnsupportedOperation, "Pen events not implemented"sv);
    case PointerInputSource::Subtype::Touch:
        return WebDriver::Error::from_code(WebDriver::ErrorCode::UnsupportedOperation, "Touch events not implemented"sv);
    }

    // 8. Return success with data null.
    return {};
}

// https://w3c.github.io/webdriver/#dfn-perform-a-pointer-move
static ErrorOr<void, WebDriver::Error> perform_pointer_move(ActionObject::PointerMoveFields const& action_object, PointerInputSource& source, GlobalKeyState const& global_key_state, HTML::BrowsingContext& browsing_context, AK::Duration, CSSPixelPoint coordinates)
{
    // FIXME: 1. Let time delta be the time since the beginning of the current tick, measured in milliseconds on a monotonic clock.
    // FIXME: 2. Let duration ratio be the ratio of time delta and duration, if duration is greater than 0, or 1 otherwise.
    // FIXME: 3. If duration ratio is 1, or close enough to 1 that the implementation will not further subdivide the move action,
    //           let last be true. Otherwise let last be false.
    // FIXME: 4. If last is true, let x equal target x and y equal target y.
    // FIXME: 5. Otherwise let x equal an approximation to duration ratio × (target x - start x) + start x, and y equal an
    //           approximation to duration ratio × (target y - start y) + start y.

    // 6. Let current x equal the x property of input state.
    // 7. Let current y equal the y property of input state.
    auto current = source.position;

    // 8. If x is not equal to current x or y is not equal to current y, run the following steps:
    if (current != coordinates) {
        // 1. Let buttons be equal to input state's buttons property.
        auto buttons = source.pressed;

        // 2. Perform implementation-specific action dispatch steps on browsing context equivalent to moving the pointer
        //    with pointerId equal to input source's pointerId, having type pointerType from viewport x coordinate current
        //    x, viewport y coordinate current y to viewport x coordinate x and viewport y coordinate y, width, height,
        //    pressure, tangentialPressure, tiltX, tiltY, twist, altitudeAngle, azimuthAngle, with buttons buttons
        //    depressed, in accordance with the requirements of [UI-EVENTS] and [POINTER-EVENTS]. The generated events
        //    must set ctrlKey, shiftKey, altKey, and metaKey equal to the corresponding items in global key state. Type
        //    specific properties for the pointer that are not exposed through the WebDriver API must be set to the
        //    default value specified for hardware that doesn't support that property. In the case where the pointerType
        //    is "pen" or "touch", and buttons is empty, this may be a no-op. For a pointer of type "mouse" this will
        //    always produce events including at least a pointerMove event.
        auto position = browsing_context.page().css_to_device_point(coordinates);

        switch (action_object.pointer_type) {
        case PointerInputSource::Subtype::Mouse:
            browsing_context.page().handle_mousemove(position, position, buttons, global_key_state.modifiers());
            break;
        case PointerInputSource::Subtype::Pen:
            return WebDriver::Error::from_code(WebDriver::ErrorCode::UnsupportedOperation, "Pen events not implemented"sv);
        case PointerInputSource::Subtype::Touch:
            return WebDriver::Error::from_code(WebDriver::ErrorCode::UnsupportedOperation, "Touch events not implemented"sv);
        }

        // 3. Let input state's x property equal x and y property equal y.
        source.position = coordinates;
    }

    // FIXME: 9. If last is true, return.
    // FIXME: 10. Run the following substeps in parallel:
    {
        // FIXME: 1. Asynchronously wait for an implementation defined amount of time to pass.
        // FIXME: 2. Perform a pointer move with arguments input state, duration, start x, start y, target x, target y.
    }

    return {};
}

// https://w3c.github.io/webdriver/#dfn-dispatch-a-pointermove-action
static ErrorOr<void, WebDriver::Error> dispatch_pointer_move_action(ActionObject::PointerMoveFields const& action_object, PointerInputSource& source, GlobalKeyState const& global_key_state, AK::Duration tick_duration, HTML::BrowsingContext& browsing_context, ActionsOptions const& actions_options)
{
    auto viewport = browsing_context.page().top_level_traversable()->viewport_rect();

    // 1. Let x offset be equal to the x property of action object.
    // 2. Let y offset be equal to the y property of action object.
    // 3. Let origin be equal to the origin property of action object.
    // 4. Let (x, y) be the result of trying to get coordinates relative to an origin with source, x offset, y offset,
    //    origin, browsing context, and actions options.
    auto coordinates = TRY(get_coordinates_relative_to_origin(source, action_object.position, viewport, action_object.origin, actions_options));

    // 5. If x is less than 0 or greater than the width of the viewport in CSS pixels, then return error with error code move target out of bounds.
    if (coordinates.x() < 0 || coordinates.x() > viewport.width())
        return WebDriver::Error::from_code(WebDriver::ErrorCode::MoveTargetOutOfBounds, ByteString::formatted("Coordinates {} are out of bounds", coordinates));

    // 6. If y is less than 0 or greater than the height of the viewport in CSS pixels, then return error with error code move target out of bounds.
    if (coordinates.y() < 0 || coordinates.y() > viewport.height())
        return WebDriver::Error::from_code(WebDriver::ErrorCode::MoveTargetOutOfBounds, ByteString::formatted("Coordinates {} are out of bounds", coordinates));

    // 7. Let duration be equal to action object's duration property if it is not undefined, or tick duration otherwise.
    [[maybe_unused]] auto duration = action_object.duration.value_or(tick_duration);

    // FIXME: 8. If duration is greater than 0 and inside any implementation-defined bounds, asynchronously wait for an
    //           implementation defined amount of time to pass.

    // 9. Let width be equal to action object's width property.
    // 10. Let height be equal to action object's height property.
    // 11. Let pressure be equal to action object's pressure property.
    // 12. Let tangentialPressure be equal to action object's tangentialPressure property.
    // 13. Let tiltX be equal to action object's tiltX property.
    // 14. Let tiltY be equal to action object's tiltY property.
    // 15. Let twist be equal to action object's twist property.
    // 16. Let altitudeAngle be equal to action object's altitudeAngle property.
    // 17. Let azimuthAngle be equal to action object's azimuthAngle property.

    // 18. Perform a pointer move with arguments source, global key state, duration, start x, start y, x, y, width,
    //     height, pressure, tangentialPressure, tiltX, tiltY, twist, altitudeAngle, azimuthAngle.
    TRY(perform_pointer_move(action_object, source, global_key_state, browsing_context, duration, coordinates));

    // 19. Return success with data null.
    return {};
}

// https://w3c.github.io/webdriver/#dfn-dispatch-actions-inner
class ActionExecutor final : public JS::Cell {
    JS_CELL(ActionExecutor, JS::Cell);
    JS_DECLARE_ALLOCATOR(ActionExecutor);

public:
    ActionExecutor(InputState& input_state, Vector<Vector<ActionObject>> actions_by_tick, HTML::BrowsingContext& browsing_context, ActionsOptions actions_options, OnActionsComplete on_complete)
        : m_browsing_context(browsing_context)
        , m_input_state(input_state)
        , m_actions_options(move(actions_options))
        , m_actions_by_tick(move(actions_by_tick))
        , m_on_complete(on_complete)
    {
    }

    // 1. For each item tick actions in actions by tick:
    void process_next_tick()
    {
        if (m_current_tick >= m_actions_by_tick.size()) {
            m_on_complete->function()(JsonValue {});
            return;
        }

        auto const& tick_actions = m_actions_by_tick[m_current_tick++];

        // 1. Let tick duration be the result of computing the tick duration with argument tick actions.
        auto tick_duration = compute_tick_duration(tick_actions);

        // 2. Try to dispatch tick actions with input state, tick actions, tick duration, browsing context, and actions options.
        if (auto result = dispatch_tick_actions(m_input_state, tick_actions, tick_duration, m_browsing_context, m_actions_options); result.is_error()) {
            m_on_complete->function()(result.release_error());
            return;
        }

        // 3. Wait until the following conditions are all met:
        //     * There are no pending asynchronous waits arising from the last invocation of the dispatch tick actions
        //       steps.
        //     * The user agent event loop has spun enough times to process the DOM events generated by the last
        //       invocation of the dispatch tick actions steps.
        //     * At least tick duration milliseconds have passed.

        // FIXME: We currently do not implement any asynchronous waits. And we assume that Page will generally fire the
        //        events of interest synchronously. So we simply wait for the tick duration to pass, and then let the
        //        event loop spin a single time.
        m_timer = Core::Timer::create_single_shot(static_cast<int>(tick_duration.to_milliseconds()), [this]() {
            m_timer = nullptr;

            HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, JS::create_heap_function(heap(), [this]() {
                process_next_tick();
            }));
        });
        m_timer->start();
    }

private:
    virtual void visit_edges(Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_browsing_context);
        visitor.visit(m_on_complete);
    }

    JS::NonnullGCPtr<HTML::BrowsingContext> m_browsing_context;

    InputState& m_input_state;
    ActionsOptions m_actions_options;

    Vector<Vector<ActionObject>> m_actions_by_tick;
    size_t m_current_tick { 0 };

    OnActionsComplete m_on_complete;

    RefPtr<Core::Timer> m_timer;
};

JS_DEFINE_ALLOCATOR(ActionExecutor);

// https://w3c.github.io/webdriver/#dfn-dispatch-actions
JS::NonnullGCPtr<JS::Cell> dispatch_actions(InputState& input_state, Vector<Vector<ActionObject>> actions_by_tick, HTML::BrowsingContext& browsing_context, ActionsOptions actions_options, OnActionsComplete on_complete)
{
    // 1. Let token be a new unique identifier.
    auto token = MUST(Crypto::generate_random_uuid());

    // 2. Enqueue token in input state's actions queue.
    input_state.actions_queue.append(token);

    // 3. Wait for token to be the first item in input state's actions queue.
    // FIXME: We should probably do this, but our WebDriver currently blocks until a given action is complete anyways,
    //        so we should never arrive here with an ongoing action (which we verify for now).
    VERIFY(input_state.actions_queue.size() == 1);

    // 4. Let actions result be the result of dispatch actions inner with input state, actions by tick, browsing
    //    context, and actions options.
    auto action_executor = browsing_context.heap().allocate_without_realm<ActionExecutor>(input_state, move(actions_by_tick), browsing_context, move(actions_options), on_complete);
    action_executor->process_next_tick();

    // 5. Dequeue input state's actions queue.
    auto executed_token = input_state.actions_queue.take_first();

    // 6. Assert: this returns token
    VERIFY(executed_token == token);

    // 7. Return actions result.
    return action_executor;
}

// https://w3c.github.io/webdriver/#dfn-dispatch-tick-actions
ErrorOr<void, WebDriver::Error> dispatch_tick_actions(InputState& input_state, ReadonlySpan<ActionObject> tick_actions, AK::Duration tick_duration, HTML::BrowsingContext& browsing_context, ActionsOptions const& actions_options)
{
    // 1. For each action object in tick actions:
    for (auto const& action_object : tick_actions) {
        // 1. Let input id be equal to the value of action object's id property.
        auto const& input_id = action_object.id;

        // 2. Let source type be equal to the value of action object's type property.
        // NOTE: We don't actually need this, we can determine the event to fire based on the subtype.

        // 3. Let source be the result of get an input source given input state and input id.
        auto source = get_input_source(input_state, input_id);

        // 4. Assert: source is not undefined.
        VERIFY(source.has_value());

        // 5. Let global key state be the result of get the global key state with input state.
        auto global_key_state = get_global_key_state(input_state);

        // 6. Let subtype be action object's subtype.
        auto subtype = action_object.subtype;

        // 7. Let algorithm be the value of the column dispatch action algorithm from the following table where the
        //    source type column is source type and the subtype column is equal to subtype.
        //
        // source type | subtype         | Dispatch action algorithm
        // ---------------------------------------------------------------
        // "none"      | "pause"         | Dispatch a pause action
        // "key"       | "pause"         | Dispatch a pause action
        // "key"       | "keyDown"       | Dispatch a keyDown action
        // "key"       | "keyUp"         | Dispatch a keyUp action
        // "pointer"   | "pause"         | Dispatch a pause action
        // "pointer"   | "pointerDown"   | Dispatch a pointerDown action
        // "pointer"   | "pointerUp"     | Dispatch a pointerUp action
        // "pointer"   | "pointerMove"   | Dispatch a pointerMove action
        // "pointer"   | "pointerCancel" | Dispatch a pointerCancel action
        // "wheel"     | "pause"         | Dispatch a pause action
        // "wheel"     | "scroll"        | Dispatch a scroll action

        // 8. Try to run algorithm with arguments action object, source, global key state, tick duration, browsing
        //    context, and actions options.
        switch (subtype) {
        case ActionObject::Subtype::Pause:
            dispatch_pause_action();
            break;
        case ActionObject::Subtype::KeyDown:
            return WebDriver::Error::from_code(WebDriver::ErrorCode::UnsupportedOperation, "Key down events not implemented"sv);
        case ActionObject::Subtype::KeyUp:
            return WebDriver::Error::from_code(WebDriver::ErrorCode::UnsupportedOperation, "Key up events not implemented"sv);
        case ActionObject::Subtype::PointerDown:
            TRY(dispatch_pointer_down_action(action_object.pointer_up_down_fields(), source->get<PointerInputSource>(), global_key_state, browsing_context));
            break;
        case ActionObject::Subtype::PointerUp:
            TRY(dispatch_pointer_up_action(action_object.pointer_up_down_fields(), source->get<PointerInputSource>(), global_key_state, browsing_context));
            break;
        case ActionObject::Subtype::PointerMove:
            TRY(dispatch_pointer_move_action(action_object.pointer_move_fields(), source->get<PointerInputSource>(), global_key_state, tick_duration, browsing_context, actions_options));
            break;
        case ActionObject::Subtype::PointerCancel:
            return WebDriver::Error::from_code(WebDriver::ErrorCode::UnsupportedOperation, "Pointer cancel events not implemented"sv);
        case ActionObject::Subtype::Scroll:
            return WebDriver::Error::from_code(WebDriver::ErrorCode::UnsupportedOperation, "Scroll events not implemented"sv);
        }

        // 9. If subtype is "keyDown", append a copy of action object with the subtype property changed to "keyUp" to
        //    input state's input cancel list.
        if (subtype == ActionObject::Subtype::KeyDown) {
            auto action_copy = action_object;
            action_copy.subtype = ActionObject::Subtype::KeyUp;

            input_state.input_cancel_list.append(move(action_copy));
        }

        // 10. If subtype is "pointerDown", append a copy of action object with the subtype property changed to
        //    "pointerUp" to input state's input cancel list.
        if (subtype == ActionObject::Subtype::PointerDown) {
            auto action_copy = action_object;
            action_copy.subtype = ActionObject::Subtype::PointerUp;

            input_state.input_cancel_list.append(move(action_copy));
        }
    }

    // 2. Return success with data null.
    return {};
}

}
