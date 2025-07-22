/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibWeb/Gamepad/Gamepad.h>
#include <LibWeb/Gamepad/NavigatorGamepad.h>
#include <LibWeb/HTML/Navigator.h>

namespace Web::Gamepad {

// https://w3c.github.io/gamepad/#dom-navigator-getgamepads
WebIDL::ExceptionOr<GC::RootVector<GC::Ptr<Gamepad>>> NavigatorGamepadPartial::get_gamepads()
{
    auto& navigator = as<HTML::Navigator>(*this);

    // FIXME: 1. Let doc be the current global object's associated Document.

    // FIXME: 2. If doc is null or doc is not fully active, then return an empty list.

    // FIXME: 3. If doc is not allowed to use the "gamepad" permission, then throw a "SecurityError" DOMException and abort
    //    these steps.

    // FIXME: 4. If this.[[hasGamepadGesture]] is false, then return an empty list.

    // FIXME: 5. Let now be the current high resolution time given the current global object.

    // FIXME: 6. Let gamepads be an empty list.

    // FIXME: 7. For each gamepad of this.[[gamepads]]:
    {
        // FIXME: 1. If gamepad is not null and gamepad.[[exposed]] is false:
        if (false) {
            // FIXME: 1. Set gamepad.[[exposed]] to true.

            // FIXME: 2. Set gamepad.[[timestamp]] to now.
        }

        // FIXME: 2. Append gamepad to gamepads.
    }

    // FIXME: 8. Return gamepads.
    dbgln("FIXME: Unimplemented NavigatorGamepadPartial::get_gamepads()");
    return GC::RootVector<GC::Ptr<Gamepad>> { navigator.heap() };
}

}
