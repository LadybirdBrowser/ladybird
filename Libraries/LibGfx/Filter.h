/*
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibGfx/Color.h>

namespace Gfx {

struct BlurFilter {
    float radius;
};

struct DropShadowFilter {
    float offset_x;
    float offset_y;
    float radius;
    Gfx::Color color;
};

struct HueRotateFilter {
    float angle_degrees;
};

struct ColorFilter {
    enum class Type {
        Brightness,
        Contrast,
        Grayscale,
        Invert,
        Opacity,
        Saturate,
        Sepia
    } type;
    float amount;
};

using Filter = Variant<BlurFilter, DropShadowFilter, HueRotateFilter, ColorFilter>;

}
