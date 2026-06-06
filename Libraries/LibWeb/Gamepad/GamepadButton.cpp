/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Gamepad/GamepadButton.h>

#include <SDL3/SDL_gamepad.h>

namespace Web::Gamepad {

GC_DEFINE_ALLOCATOR(GamepadButton);

GC::Ref<GamepadButton> GamepadButton::create()
{
    return GC::Heap::the().allocate<GamepadButton>();
}

GamepadButton::GamepadButton()
    : Bindings::Wrappable()
{
}

GamepadButton::~GamepadButton() = default;

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
