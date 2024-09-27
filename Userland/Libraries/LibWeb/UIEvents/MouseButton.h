/*
 * Copyright (c) 2024, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/EnumBits.h>
#include <AK/Types.h>

namespace Web::UIEvents {

enum MouseButton : u8 {
    None = 0,
    Primary = 1,
    Secondary = 2,
    Middle = 4,
    Backward = 8,
    Forward = 16,
};

AK_ENUM_BITWISE_OPERATORS(MouseButton);

// https://www.w3.org/TR/uievents/#dom-mouseevent-button
constexpr i16 mouse_button_to_button_code(MouseButton button)
{
    switch (button) {
    case MouseButton::Primary:
        return 0;
    case MouseButton::Middle:
        return 1;
    case MouseButton::Secondary:
        return 2;
    case MouseButton::Backward:
        return 3;
    case MouseButton::Forward:
        return 4;
    default:
        VERIFY_NOT_REACHED();
    }
}

// https://www.w3.org/TR/uievents/#dom-mouseevent-button
constexpr MouseButton button_code_to_mouse_button(i16 button)
{
    if (button == 0)
        return MouseButton::Primary;
    if (button == 1)
        return MouseButton::Middle;
    if (button == 2)
        return MouseButton::Secondary;
    if (button == 3)
        return MouseButton::Backward;
    if (button == 4)
        return MouseButton::Forward;
    return MouseButton::None;
}

}
