/*
 * Copyright (c) 2024, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/GamepadButtonPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Gamepad/GamepadButton.h>

namespace Web::Gamepad {

GC_DEFINE_ALLOCATOR(GamepadButton);

WebIDL::ExceptionOr<GC::Ref<GamepadButton>> GamepadButton::create(JS::Realm& realm, bool pressed)
{
    return realm.create<GamepadButton>(realm, pressed);
}

GamepadButton::GamepadButton(JS::Realm& realm, bool pressed)
    : Web::Bindings::PlatformObject(realm)
    , m_pressed(pressed)
{
}

GamepadButton::~GamepadButton() = default;

void GamepadButton::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GamepadButton);
}

}
