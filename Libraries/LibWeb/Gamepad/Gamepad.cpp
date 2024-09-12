/*
 * Copyright (c) 2024, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibWeb/Bindings/GamepadPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Gamepad/Gamepad.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::Gamepad {

GC_DEFINE_ALLOCATOR(Gamepad);

WebIDL::ExceptionOr<GC::Ref<Gamepad>> Gamepad::create(JS::Realm& realm, NonnullRefPtr<Core::Gamepad> gamepad)
{
    return realm.create<Gamepad>(realm, gamepad);
}

Gamepad::Gamepad(JS::Realm& realm, NonnullRefPtr<Core::Gamepad> gamepad)
    : Web::Bindings::PlatformObject(realm)
    , m_gamepad(gamepad)
{
    m_poll_timer = Core::Timer::create_repeating(10, [this]() {
        bool needs_update = m_gamepad->poll_all_events().release_value_but_fixme_should_propagate_errors();
        if (needs_update)
            update_gamepad_state();
    });
    m_poll_timer->start();
}

Gamepad::~Gamepad() = default;

void Gamepad::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Gamepad);
}

WebIDL::ExceptionOr<Vector<double>> const Gamepad::axes()
{
    // TODO: This not what the spec does
    return TRY_OR_THROW_OOM(vm(), m_gamepad->get_axes());
}

WebIDL::ExceptionOr<Vector<GC::Ref<GamepadButton>>> const Gamepad::buttons()
{
    // TODO: This not what the spec does
    Vector<GC::Ref<GamepadButton>> buttons;
    for (auto const& button : TRY_OR_THROW_OOM(vm(), m_gamepad->get_buttons()))
        buttons.append(TRY(GamepadButton::create(realm(), button)));
    return buttons;
}

// https://w3c.github.io/gamepad/#dfn-update-gamepad-state
void Gamepad::update_gamepad_state()
{
    // 1. Let now be the current high resolution time.
    HighResolutionTime::DOMHighResTimeStamp now = HighResolutionTime::current_high_resolution_time(*this);

    // 2. Set gamepad.[[timestamp]] to now.
    m_timestamp = now;

    // FIXME: 3. Run the steps to map and normalize axes for gamepad.
    // FIXME: 4. Run the steps to map and normalize buttons for gamepad.
    // FIXME: 5. Let navigator be gamepad's relevant global object's Navigator object.
    // FIXME: 6. If navigator.[[hasGamepadGesture]] is false and gamepad contains a gamepad user gesture:
    // FIXME:     1. Set navigator.[[hasGamepadGesture]] to true.
    // FIXME:     2. For each connectedGamepad of navigator.[[gamepads]]:
    // FIXME:         1. If connectedGamepad is not equal to null:
    // FIXME:             1. Set connectedGamepad.[[exposed]] to true.
    // FIXME:             2. Set connectedGamepad.[[timestamp]] to now.
    // FIXME:             3. Let document be gamepad's relevant global object's associated Document; otherwise null.
    // FIXME:             4. If document is not null and is fully active, then queue a task on the gamepad task source to fire an event named gamepadconnected at gamepad's relevant global object using GamepadEvent with its gamepad attribute initialized to connectedGamepad.
}

}
