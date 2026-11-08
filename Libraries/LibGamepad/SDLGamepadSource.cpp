/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibGamepad/SDLGamepadSource.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>

#if defined(AK_OS_WINDOWS)
#    include <objbase.h>
#endif

namespace Gamepad {

static constexpr int GAMEPAD_POLL_INTERVAL_MS = 8;

// https://w3c.github.io/gamepad/#dfn-standard-gamepad
// Type     Index   Location
// Button   0       Bottom button in right cluster
//          1       Right button in right cluster
//          2       Left button in right cluster
//          3       Top button in right cluster
//          4       Top left front button
//          5       Top right front button
//          6       Bottom left front button
//          7       Bottom right front button
//          8       Left button in center cluster
//          9       Right button in center cluster
//          10      Left stick pressed button
//          11      Right stick pressed button
//          12      Top button in left cluster
//          13      Bottom button in left cluster
//          14      Left button in left cluster
//          15      Right button in left cluster
//          16      Center button in center cluster
static Array<Variant<SDL_GamepadButton, SDL_GamepadAxis>, 17> const standard_gamepad_button_layout {
    SDL_GAMEPAD_BUTTON_SOUTH,
    SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
    SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
    SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_DPAD_UP,
    SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    SDL_GAMEPAD_BUTTON_GUIDE,
};

static Array<SDL_GamepadButton, 11> const non_standard_gamepad_button_layout {
    SDL_GAMEPAD_BUTTON_MISC1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,
    SDL_GAMEPAD_BUTTON_TOUCHPAD,
    SDL_GAMEPAD_BUTTON_MISC2,
    SDL_GAMEPAD_BUTTON_MISC3,
    SDL_GAMEPAD_BUTTON_MISC4,
    SDL_GAMEPAD_BUTTON_MISC5,
    SDL_GAMEPAD_BUTTON_MISC6,
};

// axes     0       Horizontal axis for left stick (negative left/positive right)
//          1       Vertical axis for left stick (negative up/positive down)
//          2       Horizontal axis for right stick (negative left/positive right)
//          3       Vertical axis for right stick (negative up/positive down)
static Array<SDL_GamepadAxis, 4> const standard_gamepad_axes_layout {
    SDL_GAMEPAD_AXIS_LEFTX,
    SDL_GAMEPAD_AXIS_LEFTY,
    SDL_GAMEPAD_AXIS_RIGHTX,
    SDL_GAMEPAD_AXIS_RIGHTY,
};

static constexpr char const* VIRTUAL_GAMEPAD_NAME = "Ladybird Virtual Gamepad";

static SDLCALL bool record_received_dual_rumble_effect(void* user_data, u16 low_frequency_rumble, u16 high_frequency_rumble)
{
    auto* device = static_cast<SDLGamepadSource::Device*>(user_data);
    device->received_dual_rumble_effects.append({ low_frequency_rumble, high_frequency_rumble });
    return true;
}

static SDLCALL bool record_received_trigger_rumble_effect(void* user_data, u16 left_rumble, u16 right_rumble)
{
    auto* device = static_cast<SDLGamepadSource::Device*>(user_data);
    device->received_trigger_rumble_effects.append({ left_rumble, right_rumble });
    return true;
}

SDLGamepadSource::SDLGamepadSource(IgnoreSystemGamepads ignore_system_gamepads)
    : m_ignore_system_gamepads(ignore_system_gamepads)
{
}

SDLGamepadSource::~SDLGamepadSource()
{
    if (m_poll_timer)
        m_poll_timer->stop();

    // NB: A virtual joystick that outlives its device would call back into freed memory.
    for (auto handle : m_devices.keys())
        remove_device(handle);

    if (m_sdl_initialized)
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

bool SDLGamepadSource::start()
{
    if (m_sdl_initialized)
        return true;

#if defined(AK_OS_WINDOWS)
    // Initialize COM in the multithreaded model before SDL gets a chance to initialize it apartment-threaded, which
    // would add a high overhead to calls across threads. If another component already chose a model, keep it.
    (void)CoInitializeEx(0, COINIT_MULTITHREADED);
#endif

    // Without this, SDL turns SIGINT and SIGTERM into quit events that nothing handles.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        dbgln("Failed to initialize SDL3: {}", SDL_GetError());
        return false;
    }

    m_sdl_initialized = true;
    return true;
}

void SDLGamepadSource::set_polling(bool polling)
{
    // Tests look at their gamepads when they ask.
    if (!polling || !m_sdl_initialized || m_ignore_system_gamepads == IgnoreSystemGamepads::Yes) {
        if (m_poll_timer)
            m_poll_timer->stop();
        return;
    }

    if (!m_poll_timer)
        m_poll_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(GAMEPAD_POLL_INTERVAL_MS), [this] { poll(); });
    m_poll_timer->start();
}

void SDLGamepadSource::poll()
{
    if (!m_sdl_initialized)
        return;

    SDL_Event event;
    while (SDL_PollEvent(&event))
        handle_sdl_event(event);

    Vector<Web::Gamepad::GamepadState> changed_states;
    for (auto& entry : m_devices) {
        auto& device = *entry.value;
        if (!device.connected)
            continue;
        // Compare freshly read state, since SDL suppresses some input events, such as a guide button release shortly
        // after its press.
        read_current_state(device, m_scratch_state);
        if (m_scratch_state == device.last_state)
            continue;
        device.last_state = m_scratch_state;
        changed_states.append(device.last_state);
    }

    if (on_gamepad_states_changed)
        on_gamepad_states_changed(move(changed_states));
}

void SDLGamepadSource::handle_sdl_event(SDL_Event const& event)
{
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        device_connected(event.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        device_disconnected(event.gdevice.which);
        break;
    default:
        break;
    }
}

void SDLGamepadSource::device_connected(u32 sdl_joystick_id)
{
    auto* device = device_for_sdl_joystick_id(sdl_joystick_id);

    if (!device) {
        if (m_ignore_system_gamepads == IgnoreSystemGamepads::Yes)
            return;

        auto* sdl_gamepad = SDL_OpenGamepad(sdl_joystick_id);
        if (!sdl_gamepad) {
            dbgln("Failed to open gamepad: {}", SDL_GetError());
            return;
        }

        auto handle = m_next_handle++;
        auto new_device = make<Device>();
        new_device->sdl_joystick_id = sdl_joystick_id;
        new_device->sdl_gamepad = sdl_gamepad;
        new_device->description = build_description(handle, sdl_joystick_id, sdl_gamepad);
        device = new_device.ptr();
        m_devices.set(handle, move(new_device));
    }

    if (device->connected)
        return;
    read_current_state(*device, device->last_state);
    device->connected = true;
    if (on_gamepad_connected)
        on_gamepad_connected(device->description, device->last_state);
}

void SDLGamepadSource::device_disconnected(u32 sdl_joystick_id)
{
    auto* device = device_for_sdl_joystick_id(sdl_joystick_id);
    if (!device)
        return;
    auto handle = device->description.handle;
    remove_device(handle);
    if (on_gamepad_disconnected)
        on_gamepad_disconnected(handle);
}

void SDLGamepadSource::remove_device(Web::Gamepad::GamepadHandle handle)
{
    auto removed_device = m_devices.take(handle);
    if (!removed_device.has_value())
        return;
    auto& device = **removed_device;

    SDL_CloseGamepad(device.sdl_gamepad);
    if (device.virtual_sdl_joystick) {
        SDL_CloseJoystick(device.virtual_sdl_joystick);
        SDL_DetachVirtualJoystick(device.sdl_joystick_id);
    }
}

void SDLGamepadSource::rumble(Web::Gamepad::GamepadHandle handle, u16 low_frequency_magnitude, u16 high_frequency_magnitude, u32 duration_ms)
{
    // https://wiki.libsdl.org/SDL3/SDL_RumbleGamepad
    // "Each call to this function cancels any previous rumble effect, and calling it with 0 intensity stops any
    // rumbling."
    if (auto device = m_devices.get(handle); device.has_value())
        SDL_RumbleGamepad((*device)->sdl_gamepad, low_frequency_magnitude, high_frequency_magnitude, duration_ms);
}

void SDLGamepadSource::rumble_triggers(Web::Gamepad::GamepadHandle handle, u16 left_magnitude, u16 right_magnitude, u32 duration_ms)
{
    // https://wiki.libsdl.org/SDL3/SDL_RumbleGamepadTriggers
    // "Each call to this function cancels any previous trigger rumble effect, and calling it with 0 intensity stops
    // any rumbling."
    if (auto device = m_devices.get(handle); device.has_value())
        SDL_RumbleGamepadTriggers((*device)->sdl_gamepad, left_magnitude, right_magnitude, duration_ms);
}

Optional<Web::Gamepad::VirtualGamepad> SDLGamepadSource::create_virtual_gamepad()
{
    if (!start())
        return {};

    auto device = make<Device>();

    Vector<i32> buttons;
    Vector<i32> triggers;
    for (auto const& standard_gamepad_button : standard_gamepad_button_layout) {
        standard_gamepad_button.visit(
            [&](SDL_GamepadButton button) { buttons.append(button); },
            [&](SDL_GamepadAxis axis) { triggers.append(axis); });
    }

    Vector<i32> axes;
    for (auto const standard_gamepad_axis : standard_gamepad_axes_layout)
        axes.append(standard_gamepad_axis);

    SDL_VirtualJoystickDesc virtual_joystick_desc {};
    SDL_INIT_INTERFACE(&virtual_joystick_desc);

    virtual_joystick_desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    virtual_joystick_desc.naxes = axes.size() + triggers.size();
    virtual_joystick_desc.nbuttons = buttons.size();

    u32 button_mask = 0;
    for (auto const button : buttons)
        button_mask |= 1 << button;

    virtual_joystick_desc.button_mask = button_mask;

    u32 axis_mask = 0;
    for (auto const axis : axes)
        axis_mask |= 1 << axis;

    for (auto const trigger : triggers)
        axis_mask |= 1 << trigger;

    virtual_joystick_desc.axis_mask = axis_mask;

    virtual_joystick_desc.name = VIRTUAL_GAMEPAD_NAME;
    virtual_joystick_desc.userdata = device.ptr();
    virtual_joystick_desc.Rumble = record_received_dual_rumble_effect;
    virtual_joystick_desc.RumbleTriggers = record_received_trigger_rumble_effect;

    auto sdl_joystick_id = SDL_AttachVirtualJoystick(&virtual_joystick_desc);
    if (sdl_joystick_id == 0) {
        dbgln("Failed to attach virtual joystick: {}", SDL_GetError());
        return {};
    }

    auto* sdl_joystick = SDL_OpenJoystick(sdl_joystick_id);
    auto* sdl_gamepad = SDL_OpenGamepad(sdl_joystick_id);
    if (!sdl_joystick || !sdl_gamepad) {
        dbgln("Failed to open virtual gamepad: {}", SDL_GetError());
        if (sdl_gamepad)
            SDL_CloseGamepad(sdl_gamepad);
        if (sdl_joystick)
            SDL_CloseJoystick(sdl_joystick);
        SDL_DetachVirtualJoystick(sdl_joystick_id);
        return {};
    }

    auto handle = m_next_handle++;
    device->sdl_joystick_id = sdl_joystick_id;
    device->sdl_gamepad = sdl_gamepad;
    device->description = build_description(handle, sdl_joystick_id, sdl_gamepad);
    device->virtual_sdl_joystick = sdl_joystick;
    m_devices.set(handle, move(device));

    return Web::Gamepad::VirtualGamepad { handle, move(buttons), move(axes), move(triggers) };
}

void SDLGamepadSource::set_virtual_gamepad_button(Web::Gamepad::GamepadHandle handle, i32 button, bool down)
{
    auto* device = virtual_device(handle);
    if (!device)
        return;
    SDL_SetJoystickVirtualButton(device->virtual_sdl_joystick, button, down);
    poll();
}

void SDLGamepadSource::set_virtual_gamepad_axis(Web::Gamepad::GamepadHandle handle, i32 axis, i16 value)
{
    auto* device = virtual_device(handle);
    if (!device)
        return;
    SDL_SetJoystickVirtualAxis(device->virtual_sdl_joystick, axis, value);
    poll();
}

void SDLGamepadSource::disconnect_virtual_gamepad(Web::Gamepad::GamepadHandle handle)
{
    if (virtual_device(handle))
        remove_device(handle);
}

Vector<Web::Gamepad::ReceivedDualRumbleEffect> SDLGamepadSource::virtual_gamepad_received_rumble_effects(Web::Gamepad::GamepadHandle handle)
{
    if (auto* device = virtual_device(handle))
        return device->received_dual_rumble_effects;
    return {};
}

Vector<Web::Gamepad::ReceivedTriggerRumbleEffect> SDLGamepadSource::virtual_gamepad_received_trigger_rumble_effects(Web::Gamepad::GamepadHandle handle)
{
    if (auto* device = virtual_device(handle))
        return device->received_trigger_rumble_effects;
    return {};
}

SDLGamepadSource::Device* SDLGamepadSource::device_for_sdl_joystick_id(u32 sdl_joystick_id)
{
    for (auto& entry : m_devices) {
        if (entry.value->sdl_joystick_id == sdl_joystick_id)
            return entry.value.ptr();
    }
    return nullptr;
}

SDLGamepadSource::Device* SDLGamepadSource::virtual_device(Web::Gamepad::GamepadHandle handle)
{
    auto device = m_devices.get(handle);
    if (!device.has_value() || !(*device)->virtual_sdl_joystick)
        return nullptr;
    return *device;
}

Web::Gamepad::GamepadDescription SDLGamepadSource::build_description(Web::Gamepad::GamepadHandle handle, u32 sdl_joystick_id, SDL_Gamepad* sdl_gamepad) const
{
    Web::Gamepad::GamepadDescription description;
    description.handle = handle;

    if (auto const* name = SDL_GetGamepadNameForID(sdl_joystick_id))
        description.id = String::from_utf8_with_replacement_character(StringView { name, strlen(name) });

    SDL_PropertiesID sdl_gamepad_properties = SDL_GetGamepadProperties(sdl_gamepad);
    description.supports_dual_rumble = SDL_GetBooleanProperty(sdl_gamepad_properties, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, /* default_value= */ false);
    description.supports_trigger_rumble = SDL_GetBooleanProperty(sdl_gamepad_properties, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, /* default_value= */ false);

    bool has_standard_mapping = true;

    for (size_t canonical_index = 0; canonical_index < standard_gamepad_button_layout.size(); ++canonical_index) {
        bool has_input = standard_gamepad_button_layout[canonical_index].visit(
            [&](SDL_GamepadButton button) {
                return SDL_GamepadHasButton(sdl_gamepad, button);
            },
            [&](SDL_GamepadAxis axis) {
                return SDL_GamepadHasAxis(sdl_gamepad, axis);
            });

        if (!has_input) {
            has_standard_mapping = false;
            continue;
        }

        bool is_digital = standard_gamepad_button_layout[canonical_index].has<SDL_GamepadButton>();
        description.buttons.append({
            .canonical_index = static_cast<u32>(canonical_index),
            .logical_minimum = 0,
            // Buttons are binary inputs with SDL.
            // "Trigger axis values range from 0 (released) to SDL_JOYSTICK_AXIS_MAX (fully pressed) when reported by
            // SDL_GetGamepadAxis(). Note that this is not the same range that will be reported by the lower-level
            // SDL_GetJoystickAxis()."
            .logical_maximum = is_digital ? i16(1) : i16(SDL_JOYSTICK_AXIS_MAX),
        });
    }

    for (auto const non_standard_gamepad_button : non_standard_gamepad_button_layout) {
        if (!SDL_GamepadHasButton(sdl_gamepad, non_standard_gamepad_button))
            continue;
        description.buttons.append({
            .canonical_index = {},
            .logical_minimum = 0,
            .logical_maximum = 1,
        });
    }

    for (size_t canonical_index = 0; canonical_index < standard_gamepad_axes_layout.size(); ++canonical_index) {
        if (!SDL_GamepadHasAxis(sdl_gamepad, standard_gamepad_axes_layout[canonical_index])) {
            has_standard_mapping = false;
            continue;
        }
        description.axes.append({
            .canonical_index = static_cast<u32>(canonical_index),
            .logical_minimum = SDL_JOYSTICK_AXIS_MIN,
            .logical_maximum = SDL_JOYSTICK_AXIS_MAX,
        });
    }

    description.has_standard_mapping = has_standard_mapping;
    return description;
}

void SDLGamepadSource::read_current_state(Device const& device, Web::Gamepad::GamepadState& state) const
{
    state.handle = device.description.handle;
    state.axis_values.clear_with_capacity();
    state.button_values.clear_with_capacity();

    for (auto const standard_gamepad_axis : standard_gamepad_axes_layout) {
        if (SDL_GamepadHasAxis(device.sdl_gamepad, standard_gamepad_axis))
            state.axis_values.append(SDL_GetGamepadAxis(device.sdl_gamepad, standard_gamepad_axis));
    }

    for (auto const& standard_gamepad_button : standard_gamepad_button_layout) {
        standard_gamepad_button.visit(
            [&](SDL_GamepadButton button) {
                if (SDL_GamepadHasButton(device.sdl_gamepad, button))
                    state.button_values.append(SDL_GetGamepadButton(device.sdl_gamepad, button) ? 1 : 0);
            },
            [&](SDL_GamepadAxis axis) {
                if (SDL_GamepadHasAxis(device.sdl_gamepad, axis))
                    state.button_values.append(SDL_GetGamepadAxis(device.sdl_gamepad, axis));
            });
    }

    for (auto const non_standard_gamepad_button : non_standard_gamepad_button_layout) {
        if (SDL_GamepadHasButton(device.sdl_gamepad, non_standard_gamepad_button))
            state.button_values.append(SDL_GetGamepadButton(device.sdl_gamepad, non_standard_gamepad_button) ? 1 : 0);
    }
}

}
