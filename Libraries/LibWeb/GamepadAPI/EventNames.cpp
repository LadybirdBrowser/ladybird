/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/GamepadAPI/EventNames.h>

namespace Web::GamepadAPI::EventNames {

#define __ENUMERATE_GAMEPAD_EVENT(name) \
    FlyString name = #name##_fly_string;
ENUMERATE_GAMEPAD_EVENTS
#undef __ENUMERATE_GAMEPAD_EVENT

}
