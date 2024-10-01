/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebDriver/Actions.h>
#include <LibWeb/WebDriver/InputSource.h>
#include <LibWeb/WebDriver/InputState.h>

namespace Web::WebDriver {

static InputSourceType input_source_type(InputSource const& input_source)
{
    return input_source.visit(
        [](NullInputSource const&) { return InputSourceType::None; },
        [](KeyInputSource const&) { return InputSourceType::Key; },
        [](PointerInputSource const&) { return InputSourceType::Pointer; },
        [](WheelInputSource const&) { return InputSourceType::Wheel; });
}

// https://w3c.github.io/webdriver/#dfn-get-a-pointer-id
static u32 get_pointer_id(InputState const& input_state, PointerInputSource::Subtype subtype)
{
    // 1. Let minimum id be 0 if subtype is "mouse", or 2 otherwise.
    auto minimum_id = subtype == PointerInputSource::Subtype::Mouse ? 0u : 2u;

    // 2. Let pointer ids be an empty set.
    HashTable<u32> pointer_ids;

    // 3. Let sources be the result of getting the values with input state's input state map.
    // 4. For each source in sources:
    for (auto const& source : input_state.input_state_map) {
        // 1. If source is a pointer input source, append source's pointerId to pointer ids.
        if (auto const* pointer_input_source = source.value.get_pointer<PointerInputSource>())
            pointer_ids.set(pointer_input_source->pointer_id);
    }

    // 5. Return the smallest integer that is greater than or equal to minimum id and that is not contained in pointer ids.
    for (u32 integer = minimum_id; integer < NumericLimits<u32>::max(); ++integer) {
        if (!pointer_ids.contains(integer))
            return integer;
    }

    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/webdriver/#dfn-create-a-pointer-input-source
PointerInputSource::PointerInputSource(InputState const& input_state, PointerInputSource::Subtype subtype)
    : subtype(subtype)
    , pointer_id(get_pointer_id(input_state, subtype))
{
    // To create a pointer input source object given input state, and subtype, return a new pointer input source with
    // subtype set to subtype, pointerId set to get a pointer id with input state and subtype, and the other items set
    // to their default values.
}

UIEvents::KeyModifier GlobalKeyState::modifiers() const
{
    auto modifiers = UIEvents::KeyModifier::Mod_None;

    if (ctrl_key)
        modifiers |= UIEvents::KeyModifier::Mod_Ctrl;
    if (shift_key)
        modifiers |= UIEvents::KeyModifier::Mod_Shift;
    if (alt_key)
        modifiers |= UIEvents::KeyModifier::Mod_Alt;
    if (meta_key)
        modifiers |= UIEvents::KeyModifier::Mod_Super;

    return modifiers;
}

Optional<InputSourceType> input_source_type_from_string(StringView input_source_type)
{
    if (input_source_type == "none"sv)
        return InputSourceType::None;
    if (input_source_type == "key"sv)
        return InputSourceType::Key;
    if (input_source_type == "pointer"sv)
        return InputSourceType::Pointer;
    if (input_source_type == "wheel"sv)
        return InputSourceType::Wheel;
    return {};
}

Optional<PointerInputSource::Subtype> pointer_input_source_subtype_from_string(StringView pointer_type)
{
    if (pointer_type == "mouse"sv)
        return PointerInputSource::Subtype::Mouse;
    if (pointer_type == "pen"sv)
        return PointerInputSource::Subtype::Pen;
    if (pointer_type == "touch"sv)
        return PointerInputSource::Subtype::Touch;
    return {};
}

// https://w3c.github.io/webdriver/#dfn-create-an-input-source
InputSource create_input_source(InputState const& input_state, InputSourceType type, Optional<PointerInputSource::Subtype> subtype)
{
    // Run the substeps matching the first matching value of type:
    switch (type) {
    // "none"
    case InputSourceType::None:
        // Let source be the result of create a null input source.
        return NullInputSource {};

    // "key"
    case InputSourceType::Key:
        // Let source be the result of create a key input source.
        return KeyInputSource {};

    // "pointer"
    case InputSourceType::Pointer:
        // Let source be the result of create a pointer input source with input state and subtype.
        return PointerInputSource { input_state, *subtype };

    // "wheel"
    case InputSourceType::Wheel:
        // Let source be the result of create a wheel input source.
        return WheelInputSource {};
    }

    // Otherwise:
    //     Return error with error code invalid argument.

    // NOTE: We know this cannot be reached because the only caller will have already thrown an invalid argument error
    //       if the `type` parameter was not valid.
    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/webdriver/#dfn-remove-an-input-source
void add_input_source(InputState& input_state, String id, InputSource source)
{
    // 1. Let input state map be input state's input state map.
    // 2. Set input state map[input id] to source.
    input_state.input_state_map.set(move(id), move(source));
}

// https://w3c.github.io/webdriver/#dfn-remove-an-input-source
void remove_input_source(InputState& input_state, StringView id)
{
    // 1. Assert: None of the items in input state's input cancel list has id equal to input id.
    // FIXME: Spec issue: This assertion cannot be correct. For example, when Element Click is executed, the initial
    //        pointer down action will append a pointer up action to the input cancel list, and the input cancel list
    //        is never subsequently cleared. So instead of performing this assertion, we remove any action from the
    //        input cancel list with the provided input ID.
    //        https://github.com/w3c/webdriver/issues/1809
    input_state.input_cancel_list.remove_all_matching([&](ActionObject const& action) {
        return action.id == id;
    });

    // 2. Let input state map be input state's input state map.
    // 3. Remove input state map[input id].
    input_state.input_state_map.remove(id);
}

// https://w3c.github.io/webdriver/#dfn-get-an-input-source
Optional<InputSource&> get_input_source(InputState& input_state, StringView id)
{
    // 1. Let input state map be input state's input state map.
    // 2. If input state map[input id] exists, return input state map[input id].
    // 3. Return undefined.
    return input_state.input_state_map.get(id);
}

// https://w3c.github.io/webdriver/#dfn-get-or-create-an-input-source
ErrorOr<InputSource*, WebDriver::Error> get_or_create_input_source(InputState& input_state, InputSourceType type, StringView id, Optional<PointerInputSource::Subtype> subtype)
{
    // 1. Let source be get an input source with input state and input id.
    auto source = get_input_source(input_state, id);

    // 2. If source is not undefined and source's type is not equal to type, or source is a pointer input source,
    //    return error with error code invalid argument.
    if (source.has_value() && input_source_type(*source) != type) {
        // FIXME: Spec issue: It does not make sense to check if "source is a pointer input source". This would errantly
        //        prevent the ability to perform two pointer actions in a row.
        //        https://github.com/w3c/webdriver/issues/1810
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Property 'type' does not match existing input source type");
    }

    // 3. If source is undefined, set source to the result of trying to create an input source with input state and type.
    if (!source.has_value()) {
        // FIXME: Spec issue: The spec doesn't say to add the source to the input state map, but it is explicitly
        //        expected when we reach the `dispatch tick actions` AO.
        //        https://github.com/w3c/webdriver/issues/1810
        input_state.input_state_map.set(MUST(String::from_utf8(id)), create_input_source(input_state, type, subtype));
        source = get_input_source(input_state, id);
    }

    // 4. Return success with data source.
    return &source.value();
}

// https://w3c.github.io/webdriver/#dfn-get-the-global-key-state
GlobalKeyState get_global_key_state(InputState const& input_state)
{
    // 1. Let input state map be input state's input state map.
    auto const& input_state_map = input_state.input_state_map;

    // 2. Let sources be the result of getting the values with input state map.

    // 3. Let key state be a new global key state with pressed set to an empty set, altKey, ctrlKey, metaKey, and
    //    shiftKey set to false.
    GlobalKeyState key_state {};

    // 4. For each source in sources:
    for (auto const& source : input_state_map) {
        // 1. If source is not a key input source, continue to the first step of this loop.
        auto const* key_input_source = source.value.get_pointer<KeyInputSource>();
        if (!key_input_source)
            continue;

        // 2. Set key state's pressed item to the union of its current value and source's pressed item.
        for (auto const& pressed : key_input_source->pressed)
            key_state.pressed.set(pressed);

        // 3. If source's alt item is true, set key state's altKey item to true.
        key_state.alt_key |= key_input_source->alt;

        // 4. If source's ctrl item is true, set key state's ctrlKey item to true.
        key_state.ctrl_key |= key_input_source->ctrl;

        // 5. If source's meta item is true, set key state's metaKey item to true.
        key_state.meta_key |= key_input_source->meta;

        // 6. If source's shift item is true, set key state's shiftKey item to true.
        key_state.shift_key |= key_input_source->shift;
    }

    // 5. Return key state.
    return key_state;
}

}
