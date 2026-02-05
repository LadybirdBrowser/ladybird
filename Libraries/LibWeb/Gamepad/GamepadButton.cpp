/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/GamepadButtonPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Gamepad/GamepadButton.h>

#include <SDL3/SDL_gamepad.h>

namespace Web::Gamepad {

GC_DEFINE_ALLOCATOR(GamepadButton);

GamepadButton::GamepadButton(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

GamepadButton::~GamepadButton() = default;

void GamepadButton::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GamepadButton);
    Base::initialize(realm);
}

void GamepadButton::set_pressed(Badge<Gamepad>, bool value)
{
    m_pressed = value;
}

void GamepadButton::set_touched(Badge<Gamepad>, bool value)
{
    m_touched = value;
}

void GamepadButton::set_value(Badge<Gamepad>, double value)
{
    m_value = value;
}

}
