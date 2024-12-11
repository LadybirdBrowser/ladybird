/*
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>

namespace Gfx {

struct BlurFilter {
    float radius;
};

struct DropShadowFilter {
    double offset_x;
    double offset_y;
    double radius;
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
