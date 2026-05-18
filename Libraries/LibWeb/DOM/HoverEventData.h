/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>
#include <LibWeb/PixelUnits.h>

namespace Web::DOM {

struct HoverEventData {
    CSSPixelPoint screen_position;
    CSSPixelPoint page_offset;
    CSSPixelPoint viewport_position;
    CSSPixelPoint offset;
    Optional<CSSPixelPoint> movement;
    u32 button { 0 };
    u32 buttons { 0 };
    u32 modifiers { 0 };
};

}
