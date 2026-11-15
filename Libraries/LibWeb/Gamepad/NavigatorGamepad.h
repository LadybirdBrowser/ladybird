/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibGC/RootVector.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWebCommon/Gamepad/GamepadSnapshot.h>

namespace Web::Gamepad {

class NavigatorGamepadPartial {
public:
    WebIDL::ExceptionOr<GC::RootVector<GC::Ptr<Gamepad>>> get_gamepads();

    void handle_gamepad_connected(GamepadDescription const&);
    void handle_gamepad_updated(Badge<EventHandler>, GamepadState const&);
    void handle_gamepad_disconnected(Badge<EventHandler>, GamepadHandle);

protected:
    void check_for_connected_gamepads();

private:
    virtual ~NavigatorGamepadPartial() = default;

    friend class HTML::Navigator;
};

}
