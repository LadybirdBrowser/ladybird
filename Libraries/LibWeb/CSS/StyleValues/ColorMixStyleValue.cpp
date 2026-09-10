/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ColorMixStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<ColorMixStyleValue const> ColorMixStyleValue::create(RefPtr<StyleValue const> color_interpolation_method, ColorMixComponent first_component, ColorMixComponent second_component)
{
    return adopt_ref(*new (nothrow) ColorMixStyleValue(move(color_interpolation_method), move(first_component), move(second_component)));
}

ColorMixStyleValue::ColorMixStyleValue(RefPtr<StyleValue const> color_interpolation_method, ColorMixComponent first_component, ColorMixComponent second_component)
    : ColorStyleValue(make_color_mix_data(color_interpolation_method, first_component, second_component))
{
}

ColorMixStyleValue::ColorMixStyleValue(StyleValueFFI::StyleValueData const* data)
    : ColorStyleValue(data)
{
}

}
