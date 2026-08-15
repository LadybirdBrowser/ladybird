/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ColorInterpolationMethodStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<ColorInterpolationMethodStyleValue const> ColorInterpolationMethodStyleValue::create(ColorInterpolationMethod color_space)
{
    return adopt_ref(*new (nothrow) ColorInterpolationMethodStyleValue(move(color_space)));
}

ColorInterpolationMethodStyleValue::ColorInterpolationMethod ColorInterpolationMethodStyleValue::default_color_interpolation_method(ColorSyntax color_syntax)
{
    return color_syntax == ColorSyntax::Legacy ? RectangularColorSpace::Srgb : RectangularColorSpace::Oklab;
}

}
