/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/InternalGamepadPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Internals/InternalGamepad.h>
#include <LibWeb/Internals/Internals.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>

namespace Web::Internals {

GC_DEFINE_ALLOCATOR(InternalGamepad);

static constexpr Array<i32, 15> BUTTONS = {
    SDL_GAMEPAD_BUTTON_SOUTH,
    SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
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

static constexpr Array<i32, 4> AXES {
    SDL_GAMEPAD_AXIS_LEFTX,
    SDL_GAMEPAD_AXIS_LEFTY,
    SDL_GAMEPAD_AXIS_RIGHTX,
    SDL_GAMEPAD_AXIS_RIGHTY,
};

static constexpr Array<i32, 2> TRIGGERS {
    SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
    SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
};

static constexpr char const* VIRTUAL_GAMEPAD_NAME = "Ladybird Virtual Gamepad";

static SDLCALL bool rumble(void* user_data, u16 low_frequency_rumble, u16 high_frequency_rumble)
{
    auto* internal_gamepad = static_cast<InternalGamepad*>(user_data);
    internal_gamepad->received_rumble(low_frequency_rumble, high_frequency_rumble);
    return true;
}

static SDLCALL bool rumble_triggers(void* user_data, u16 left_rumble, u16 right_rumble)
{
    auto* internal_gamepad = static_cast<InternalGamepad*>(user_data);
    internal_gamepad->received_rumble_triggers(left_rumble, right_rumble);
    return true;
}

InternalGamepad::InternalGamepad(JS::Realm& realm, GC::Ref<Internals> internals)
    : Bindings::PlatformObject(realm)
    , m_internals(internals)
{
    SDL_VirtualJoystickDesc virtual_joystick_desc {};
    SDL_INIT_INTERFACE(&virtual_joystick_desc);

    virtual_joystick_desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    virtual_joystick_desc.naxes = AXES.size() + TRIGGERS.size();
    virtual_joystick_desc.nbuttons = BUTTONS.size();

    u32 button_mask = 0;
    for (auto const button : BUTTONS)
        button_mask |= 1 << button;

    virtual_joystick_desc.button_mask = button_mask;

    u32 axis_mask = 0;
    for (auto const axis : AXES)
        axis_mask |= 1 << axis;

    for (auto const trigger : TRIGGERS)
        axis_mask |= 1 << trigger;

    virtual_joystick_desc.axis_mask = axis_mask;

    virtual_joystick_desc.name = VIRTUAL_GAMEPAD_NAME;
    virtual_joystick_desc.userdata = this;
    virtual_joystick_desc.Rumble = rumble;
    virtual_joystick_desc.RumbleTriggers = rumble_triggers;

    m_sdl_joystick_id = SDL_AttachVirtualJoystick(&virtual_joystick_desc);
    m_sdl_joystick = SDL_OpenJoystick(m_sdl_joystick_id);
}

InternalGamepad::~InternalGamepad() = default;

void InternalGamepad::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(InternalGamepad);
    Base::initialize(realm);
}

void InternalGamepad::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_received_rumble_effects);
    visitor.visit(m_received_rumble_trigger_effects);
    visitor.visit(m_internals);
}

void InternalGamepad::finalize()
{
    Base::finalize();
    disconnect();
}

Array<i32, 15> const& InternalGamepad::buttons()
{
    return BUTTONS;
}

Array<i32, 4> const& InternalGamepad::axes()
{
    return AXES;
}

Array<i32, 2> const& InternalGamepad::triggers()
{
    return TRIGGERS;
}

void InternalGamepad::set_button(int button, bool down)
{
    SDL_SetJoystickVirtualButton(m_sdl_joystick, button, down);
}

void InternalGamepad::set_axis(int axis, short value)
{
    SDL_SetJoystickVirtualAxis(m_sdl_joystick, axis, value);
}

GC::RootVector<JS::Object*> InternalGamepad::get_received_rumble_effects() const
{
    GC::RootVector<JS::Object*> received_rumble_effects { realm().heap() };
    for (auto const received_rumble_effect : m_received_rumble_effects)
        received_rumble_effects.append(received_rumble_effect);
    return received_rumble_effects;
}

GC::RootVector<JS::Object*> InternalGamepad::get_received_rumble_trigger_effects() const
{
    GC::RootVector<JS::Object*> received_rumble_trigger_effects { realm().heap() };
    for (auto const received_rumble_trigger_effect : m_received_rumble_trigger_effects)
        received_rumble_trigger_effects.append(received_rumble_trigger_effect);
    return received_rumble_trigger_effects;
}

void InternalGamepad::received_rumble(u16 low_frequency_rumble, u16 high_frequency_rumble)
{
    auto object = JS::Object::create(realm(), nullptr);
    object->define_direct_property("lowFrequencyRumble"_utf16, JS::Value(low_frequency_rumble), JS::default_attributes);
    object->define_direct_property("highFrequencyRumble"_utf16, JS::Value(high_frequency_rumble), JS::default_attributes);
    m_received_rumble_effects.append(object);
}

void InternalGamepad::received_rumble_triggers(u16 left_rumble, u16 right_rumble)
{
    auto object = JS::Object::create(realm(), nullptr);
    object->define_direct_property("leftRumble"_utf16, JS::Value(left_rumble), JS::default_attributes);
    object->define_direct_property("rightRumble"_utf16, JS::Value(right_rumble), JS::default_attributes);
    m_received_rumble_trigger_effects.append(object);
}

void InternalGamepad::disconnect()
{
    m_internals->disconnect_virtual_gamepad(*this);
    SDL_CloseJoystick(m_sdl_joystick);
    SDL_DetachVirtualJoystick(m_sdl_joystick_id);
}

}
