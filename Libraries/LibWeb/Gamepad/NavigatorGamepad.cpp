/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/HostClassBuilder.h>
#include <LibWeb/Gamepad/Gamepad.h>
#include <LibWeb/Gamepad/GamepadRegistry.h>
#include <LibWeb/Gamepad/NavigatorGamepad.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWebCommon/Gamepad/GamepadInput.h>

namespace Web::Gamepad {

struct GamepadInputDescriptorABI {
    bool has_canonical_index;
    u32 canonical_index;
    i16 logical_minimum;
    i16 logical_maximum;
};

struct GamepadDescriptionABI {
    GamepadHandle handle;
    ReadonlyBytes id;
    bool has_standard_mapping;
    bool supports_dual_rumble;
    bool supports_trigger_rumble;
    ReadonlySpan<GamepadInputDescriptorABI> axes;
    ReadonlySpan<GamepadInputDescriptorABI> buttons;
};

struct GamepadStateABI {
    GamepadHandle handle;
    ReadonlySpan<i16> axis_values;
    ReadonlySpan<i16> button_values;
};

extern "C" {
// Defined in Bindings/Rust/src/gamepad/exports.rs.
bool web_rust_gamepad_get_gamepads(JSVM*, HTML::Window* navigator_window, HTML::Window* current_window, void* ctx, void (*on_gamepad)(void*, Gamepad*));
void web_rust_gamepad_handle_gamepad_connected(JSVM*, HTML::Window*, GamepadDescriptionABI const*);
void web_rust_gamepad_handle_gamepad_updated(JSVM*, HTML::Window*, GamepadStateABI const*);
void web_rust_gamepad_handle_gamepad_disconnected(JSVM*, HTML::Window*, GamepadHandle);

// Called by Bindings/Rust/src/gamepad/.
void web_gamepad_did_start_using_gamepads(HTML::Window*);
void web_gamepad_play_effect(HTML::Window*, GamepadHandle, u8 effect_type, u16 first_magnitude, u16 second_magnitude, u32 duration);
void web_gamepad_stop_effects(HTML::Window*, GamepadHandle);
double web_gamepad_normalize_axis_value(i16 logical_value, i16 logical_minimum, i16 logical_maximum);
double web_gamepad_normalize_button_value(i16 logical_value, i16 logical_minimum, i16 logical_maximum);
bool web_gamepad_is_button_pressed(i16 logical_value, i16 logical_minimum, i16 logical_maximum);
bool web_gamepad_contains_gamepad_user_gesture(GamepadDescriptionABI const*, GamepadStateABI const*);
}

static JSVM* the_vm()
{
    return JS::HostABI::vm_to_abi(JS::VM::the());
}

static HTML::Window& navigator_window(NavigatorGamepadPartial& navigator)
{
    return as<HTML::Navigator>(navigator).window();
}

static Vector<GamepadInputDescriptorABI> input_descriptors_to_abi(Vector<GamepadInputDescriptor> const& inputs)
{
    Vector<GamepadInputDescriptorABI> descriptors;
    descriptors.ensure_capacity(inputs.size());
    for (auto const& input : inputs)
        descriptors.unchecked_append({ input.canonical_index.has_value(), input.canonical_index.value_or(0), input.logical_minimum, input.logical_maximum });
    return descriptors;
}

static Vector<GamepadInputDescriptor> input_descriptors_from_abi(ReadonlySpan<GamepadInputDescriptorABI> inputs)
{
    Vector<GamepadInputDescriptor> descriptors;
    descriptors.ensure_capacity(inputs.size());
    for (auto const& input : inputs) {
        Optional<u32> canonical_index;
        if (input.has_canonical_index)
            canonical_index = input.canonical_index;
        descriptors.unchecked_append({ canonical_index, input.logical_minimum, input.logical_maximum });
    }
    return descriptors;
}

WebIDL::ExceptionOr<GC::RootVector<GC::Ptr<Gamepad>>> NavigatorGamepadPartial::get_gamepads()
{
    GC::RootVector<GC::Ptr<Gamepad>> gamepads;
    auto allowed = web_rust_gamepad_get_gamepads(the_vm(), &navigator_window(*this), &HTML::current_window(), &gamepads, [](void* ctx, Gamepad* gamepad) {
        static_cast<GC::RootVector<GC::Ptr<Gamepad>>*>(ctx)->append(gamepad);
    });
    if (!allowed)
        return WebIDL::SecurityError::create("Not allowed to use gamepads"_utf16);
    return gamepads;
}

void NavigatorGamepadPartial::handle_gamepad_connected(GamepadDescription const& description)
{
    auto axes = input_descriptors_to_abi(description.axes);
    auto buttons = input_descriptors_to_abi(description.buttons);
    GamepadDescriptionABI description_abi {
        description.handle,
        description.id.bytes(),
        description.has_standard_mapping,
        description.supports_dual_rumble,
        description.supports_trigger_rumble,
        axes,
        buttons,
    };
    web_rust_gamepad_handle_gamepad_connected(the_vm(), &navigator_window(*this), &description_abi);
}

void NavigatorGamepadPartial::handle_gamepad_updated(Badge<EventHandler>, GamepadState const& state)
{
    GamepadStateABI state_abi { state.handle, state.axis_values, state.button_values };
    web_rust_gamepad_handle_gamepad_updated(the_vm(), &navigator_window(*this), &state_abi);
}

void NavigatorGamepadPartial::handle_gamepad_disconnected(Badge<EventHandler>, GamepadHandle handle)
{
    web_rust_gamepad_handle_gamepad_disconnected(the_vm(), &navigator_window(*this), handle);
}

void NavigatorGamepadPartial::check_for_connected_gamepads()
{
    GamepadRegistry::the().for_each_connected_gamepad([this](GamepadDescription const& description) {
        handle_gamepad_connected(description);
    });
}

void web_gamepad_did_start_using_gamepads(HTML::Window* window)
{
    window->page().client().page_did_start_using_gamepads();
}

void web_gamepad_play_effect(HTML::Window* window, GamepadHandle handle, u8 effect_type, u16 first_magnitude, u16 second_magnitude, u32 duration)
{
    // NB: The effect type is the index of the variant of GamepadHapticEffectType.
    GamepadEffect effect = effect_type == 0
        ? GamepadEffect { GamepadDualRumbleEffect { first_magnitude, second_magnitude, duration } }
        : GamepadEffect { GamepadTriggerRumbleEffect { first_magnitude, second_magnitude, duration } };
    window->page().client().page_did_play_gamepad_effect(handle, effect);
}

void web_gamepad_stop_effects(HTML::Window* window, GamepadHandle handle)
{
    window->page().client().page_did_request_stop_gamepad_effects(handle);
}

double web_gamepad_normalize_axis_value(i16 logical_value, i16 logical_minimum, i16 logical_maximum)
{
    return normalize_axis_value(logical_value, logical_minimum, logical_maximum);
}

double web_gamepad_normalize_button_value(i16 logical_value, i16 logical_minimum, i16 logical_maximum)
{
    return normalize_button_value(logical_value, logical_minimum, logical_maximum);
}

bool web_gamepad_is_button_pressed(i16 logical_value, i16 logical_minimum, i16 logical_maximum)
{
    return is_button_pressed(logical_value, logical_minimum, logical_maximum);
}

bool web_gamepad_contains_gamepad_user_gesture(GamepadDescriptionABI const* description_abi, GamepadStateABI const* state_abi)
{
    GamepadDescription description;
    description.handle = description_abi->handle;
    description.axes = input_descriptors_from_abi(description_abi->axes);
    description.buttons = input_descriptors_from_abi(description_abi->buttons);

    GamepadState state;
    state.handle = state_abi->handle;
    state.axis_values = Vector<i16> { state_abi->axis_values };
    state.button_values = Vector<i16> { state_abi->button_values };

    return contains_gamepad_user_gesture(description, state);
}

}
