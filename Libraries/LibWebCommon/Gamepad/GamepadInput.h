/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebCommon/Export.h>
#include <LibWebCommon/Gamepad/GamepadSnapshot.h>

namespace Web::Gamepad {

WEBCOMMON_API double normalize_axis_value(i16 logical_value, i16 logical_minimum, i16 logical_maximum);
WEBCOMMON_API double normalize_button_value(i16 logical_value, i16 logical_minimum, i16 logical_maximum);
WEBCOMMON_API bool is_button_pressed(i16 logical_value, i16 logical_minimum, i16 logical_maximum);

WEBCOMMON_API bool contains_gamepad_user_gesture(GamepadDescription const&, GamepadState const&);

}
