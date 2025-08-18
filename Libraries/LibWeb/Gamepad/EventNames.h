/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::Gamepad::EventNames {

#define ENUMERATE_GAMEPAD_EVENTS                \
    __ENUMERATE_GAMEPAD_EVENT(gamepadconnected) \
    __ENUMERATE_GAMEPAD_EVENT(gamepaddisconnected)

#define __ENUMERATE_GAMEPAD_EVENT(name) extern FlyString name;
ENUMERATE_GAMEPAD_EVENTS
#undef __ENUMERATE_GAMEPAD_EVENT

}
