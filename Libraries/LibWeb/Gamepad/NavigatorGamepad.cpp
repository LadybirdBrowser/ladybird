/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/GamepadFinder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Gamepad/Gamepad.h>
#include <LibWeb/Gamepad/NavigatorGamepad.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::Gamepad {

// https://w3c.github.io/gamepad/#getgamepads-method
WebIDL::ExceptionOr<Vector<GC::Ptr<Gamepad>>> NavigatorGamepadMixin::get_gamepads()
{
    // FIXME: This is not a part of the spec, but there's no better place to put it
    if (!m_loaded_initial_gamepads) {
        if (auto result = Core::find_all_connected_gamepads(); !result.is_error())
            for (auto const& gamepad : result.release_value())
                register_new_gamepad(gamepad).release_value_but_fixme_should_propagate_errors();
        m_loaded_initial_gamepads = true;
    }

    // 1. Let doc be the current global object's associated Document.
    auto& window = verify_cast<HTML::Window>(HTML::current_principal_global_object());
    auto& doc = window.associated_document();

    // 2. If doc is null or doc is not fully active, then return an empty list.
    if (!doc.is_fully_active())
        return Vector<GC::Ptr<Gamepad>> {};

    // FIXME: 3. If doc is not allowed to use the "gamepad" permission, then throw a "SecurityError" DOMException
    //           and abort these steps.
    // FIXME: 4. If this.[[hasGamepadGesture]] is false, then return an empty list.

    // 5. Let now be the current high resolution time.
    HighResolutionTime::DOMHighResTimeStamp now = HighResolutionTime::current_high_resolution_time(verify_cast<HTML::Navigator>(*this));

    // 6. Let gamepads be an empty list.
    Vector<GC::Ptr<Gamepad>> gamepads;

    // 7. For each gamepad of this.[[gamepads]]:
    for (auto const& gamepad : m_gamepads) {
        // 1. If gamepad is not null and gamepad.[[exposed]] is false:
        if (gamepad != nullptr && !gamepad->exposed()) {
            // 1. Set gamepad.[[exposed]] to true.
            gamepad->set_exposed(true);

            // 2. Set gamepad.[[timestamp]] to now.
            gamepad->set_timestamp(now);
        }

        // 2. Append gamepad to gamepads.
        gamepads.append(gamepad);
    }

    // 8. Return gamepads.
    return gamepads;
}

WebIDL::ExceptionOr<void> NavigatorGamepadMixin::register_new_gamepad(String const& path)
{
    for (auto const& gamepad : m_gamepads) {
        if (gamepad->gamepad()->path() == path)
            return {};
    }

    if (auto result = Core::Gamepad::create(path); !result.is_error()) {
        auto gamepad = TRY(construct_gamepad(result.release_value()));
        m_gamepads[gamepad->index()] = gamepad;
    }

    return {};
}

// https://w3c.github.io/gamepad/#constructing-a-gamepad
WebIDL::ExceptionOr<GC::Ref<Gamepad>> NavigatorGamepadMixin::construct_gamepad(AK::NonnullRefPtr<Core::Gamepad> underlaying_gamepad)
{
    auto& navigator = verify_cast<HTML::Navigator>(*this);

    // 1. Let gamepad be a newly created Gamepad instance:
    auto gamepad = TRY(Gamepad::create(navigator.realm(), underlaying_gamepad));

    // 1. Initialize gamepad's id attribute to an identification string for the gamepad.
    gamepad->set_id(underlaying_gamepad->name());

    // 2. Initialize gamepad's index attribute to the result of selecting an unused gamepad index for gamepad.
    gamepad->set_index(select_unused_gamepad_index());

    // FIXME: 3. Initialize gamepad's mapping attribute to the result of selecting a mapping for the gamepad device.

    // 4. Initialize gamepad.[[connected]] to true.
    gamepad->set_connected(true);

    // 5. Initialize gamepad.[[timestamp]] to the current high resolution time.
    gamepad->set_timestamp(HighResolutionTime::unsafe_shared_current_time());

    // FIXME: 6. Initialize gamepad.[[axes]] to the result of initializing axes for gamepad.
    // FIXME: 7. Initialize gamepad.[[buttons]] to the result of initializing buttons for gamepad.
    // FIXME: 8. Initialize gamepad.[[vibrationActuator]] following the steps of constructing a GamepadHapticActuator for gamepad.

    // 2. Return gamepad.
    return gamepad;
}

int NavigatorGamepadMixin::select_unused_gamepad_index()
{
    // 1. Let navigator be gamepad's relevant global object's Navigator object.

    // 2. Let maxGamepadIndex be the size of navigator.[[gamepads]] − 1.
    int maxGamepadIndex = m_gamepads.size() - 1;

    // 3. For each gamepadIndex of the range from 0 to maxGamepadIndex:
    for (int gamepadIndex = 0; gamepadIndex < maxGamepadIndex; gamepadIndex++) {
        // 1. If navigator.[[gamepads]][gamepadIndex] is null, then return gamepadIndex.
        if (m_gamepads[gamepadIndex] == nullptr)
            return gamepadIndex;
    }

    // 4. Append null to navigator.[[gamepads]].
    m_gamepads.append(nullptr);

    // 5. Return the size of navigator.[[gamepads]] − 1.
    return m_gamepads.size() - 1;
}

}
