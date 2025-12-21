/*
 * Copyright (c) 2025, Jonathan Gamble <gamblej@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/PixelUnits.h>

namespace Web {

struct ChromeMetrics {
    static constexpr CSSPixels ScrollThumbMinLength { 24 };
    static constexpr CSSPixels ScrollThumbMarginThin { 2 };
    static constexpr CSSPixels ScrollThumbThicknessThin { 6 };
    static constexpr CSSPixels ScrollThumbThickness { 8 };
    static constexpr CSSPixels ScrollGutterThickness { 12 };

    static constexpr CSSPixels ResizeGripperSize { 12 };
};

}
