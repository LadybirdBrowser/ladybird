/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FilterStyleValue.h"
#include <LibWeb/CSS/CalculationResolutionContext.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>

namespace Web::CSS {

// The kind and color-operation discriminants cross the style value FFI as raw codes; the Rust
// serializer's tables depend on them.
static_assert(to_underlying(FilterStyleValue::Kind::Blur) == 0);
static_assert(to_underlying(FilterStyleValue::Kind::DropShadow) == 1);
static_assert(to_underlying(FilterStyleValue::Kind::HueRotate) == 2);
static_assert(to_underlying(FilterStyleValue::Kind::Color) == 3);
static_assert(to_underlying(Gfx::ColorFilterType::Brightness) == 0);
static_assert(to_underlying(Gfx::ColorFilterType::Contrast) == 1);
static_assert(to_underlying(Gfx::ColorFilterType::Grayscale) == 2);
static_assert(to_underlying(Gfx::ColorFilterType::Invert) == 3);
static_assert(to_underlying(Gfx::ColorFilterType::Opacity) == 4);
static_assert(to_underlying(Gfx::ColorFilterType::Saturate) == 5);
static_assert(to_underlying(Gfx::ColorFilterType::Sepia) == 6);

float BlurFilterStyleValue::resolved_radius() const
{
    return Length::from_style_value(radius(), {}).absolute_length_to_px_without_rounding();
}

float HueRotateFilterStyleValue::angle_degrees() const
{
    return Angle::from_style_value(angle(), {}).to_degrees();
}

float ColorFilterStyleValue::resolved_amount() const
{
    return number_from_style_value(amount(), 1);
}

bool is_filter_style_value_list(StyleValue const& value)
{
    if (!value.is_value_list())
        return false;
    auto const& list = value.as_value_list();
    if (list.size() == 0)
        return false;
    if (list.separator() != StyleValueList::Separator::Space)
        return false;
    return all_of(list.values(), [](auto const& value) { return value->is_filter() || value->is_url(); });
}

}
