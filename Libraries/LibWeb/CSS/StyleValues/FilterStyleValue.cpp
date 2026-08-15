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

// The C++ Type is Filter for every filter kind, so filter operations dispatch on the kind.
ValueComparingNonnullRefPtr<StyleValue const> FilterStyleValue::absolutized(ComputationContext const& context) const
{
    switch (kind()) {
    case Kind::Blur:
        return static_cast<BlurFilterStyleValue const&>(*this).absolutized(context);
    case Kind::DropShadow:
        return static_cast<DropShadowFilterStyleValue const&>(*this).absolutized(context);
    case Kind::HueRotate:
        return static_cast<HueRotateFilterStyleValue const&>(*this).absolutized(context);
    case Kind::Color:
        return static_cast<ColorFilterStyleValue const&>(*this).absolutized(context);
    }
    VERIFY_NOT_REACHED();
}

float BlurFilterStyleValue::resolved_radius() const
{
    return Length::from_style_value(radius(), {}).absolute_length_to_px_without_rounding();
}

ValueComparingNonnullRefPtr<StyleValue const> BlurFilterStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto radius = this->radius();
    auto absolutized_radius = radius->absolutized(computation_context);
    if (absolutized_radius->equals(radius))
        return *this;
    return BlurFilterStyleValue::create(move(absolutized_radius));
}

ValueComparingNonnullRefPtr<StyleValue const> DropShadowFilterStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto shadow = this->shadow();
    auto absolutized_offset_x = shadow->offset_x()->absolutized(computation_context);
    auto absolutized_offset_y = shadow->offset_y()->absolutized(computation_context);
    auto absolutized_radius = shadow->blur_radius()->absolutized(computation_context);
    auto absolutized_color = shadow->color()->absolutized(computation_context);

    if (absolutized_offset_x->equals(shadow->offset_x())
        && absolutized_offset_y->equals(shadow->offset_y())
        && absolutized_radius == shadow->blur_radius_or_null()
        && absolutized_color == shadow->color_or_null())
        return *this;

    return DropShadowFilterStyleValue::create(
        move(absolutized_offset_x),
        move(absolutized_offset_y),
        move(absolutized_radius),
        move(absolutized_color));
}

float HueRotateFilterStyleValue::angle_degrees() const
{
    return Angle::from_style_value(angle(), {}).to_degrees();
}

ValueComparingNonnullRefPtr<StyleValue const> HueRotateFilterStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto angle = this->angle();
    auto absolutized_angle = angle->absolutized(computation_context);
    if (absolutized_angle->equals(angle))
        return *this;
    return HueRotateFilterStyleValue::create(move(absolutized_angle));
}

float ColorFilterStyleValue::resolved_amount() const
{
    return number_from_style_value(amount(), 1);
}

ValueComparingNonnullRefPtr<StyleValue const> ColorFilterStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto amount = this->amount();
    auto absolutized_amount = number_from_style_value(amount->absolutized(computation_context), 1);

    if (first_is_one_of(operation(), Gfx::ColorFilterType::Grayscale, Gfx::ColorFilterType::Invert, Gfx::ColorFilterType::Opacity, Gfx::ColorFilterType::Sepia))
        absolutized_amount = clamp(absolutized_amount, 0.0f, 1.0f);

    if (amount->is_number() && amount->as_number().number() == absolutized_amount)
        return *this;

    return ColorFilterStyleValue::create(operation(), NumberStyleValue::create(absolutized_amount));
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
