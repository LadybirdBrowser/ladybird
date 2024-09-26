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

}
