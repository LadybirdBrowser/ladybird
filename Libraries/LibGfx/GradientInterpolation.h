/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Gfx {

enum class RectangularColorSpace : u8 {
    Srgb,
    SrgbLinear,
    DisplayP3,
    DisplayP3Linear,
    A98Rgb,
    ProphotoRgb,
    Rec2020,
    Lab,
    Oklab,
    Xyz,
    XyzD50,
    XyzD65,
};

enum class PolarColorSpace : u8 {
    Hsl,
    Hwb,
    Lch,
    Oklch,
};

enum class HueInterpolationMethod : u8 {
    Shorter,
    Longer,
    Increasing,
    Decreasing,
};

struct GradientInterpolationMethod {
    enum class Type : u8 {
        Rectangular,
        Polar,
    };

    Type type { Type::Rectangular };
    RectangularColorSpace rectangular_color_space { RectangularColorSpace::Oklab };
    PolarColorSpace polar_color_space { PolarColorSpace::Oklch };
    HueInterpolationMethod hue_interpolation_method { HueInterpolationMethod::Shorter };
};

}
