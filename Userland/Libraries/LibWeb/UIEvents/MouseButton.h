/*
 * Copyright (c) 2024, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>

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

}
