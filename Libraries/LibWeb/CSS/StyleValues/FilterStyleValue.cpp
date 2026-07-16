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

ValueComparingNonnullRefPtr<FilterStyleValue const> FilterStyleValue::initial_value_for(FilterStyleValue const& value, bool use_transparent_drop_shadow_color)
{
    switch (value.kind()) {
    case FilterStyleValue::Kind::Blur:
        return BlurFilterStyleValue::create(LengthStyleValue::create(Length::make_px(0)));
    case FilterStyleValue::Kind::DropShadow:
        return DropShadowFilterStyleValue::create(
            LengthStyleValue::create(Length::make_px(0)),
            LengthStyleValue::create(Length::make_px(0)),
            LengthStyleValue::create(Length::make_px(0)),
            use_transparent_drop_shadow_color ? static_cast<ValueComparingRefPtr<ColorStyleValue const>>(ColorStyleValue::create_from_color(Color::Transparent, ColorSyntax::Legacy)) : nullptr);
    case FilterStyleValue::Kind::HueRotate:
        return HueRotateFilterStyleValue::create(AngleStyleValue::create(Angle::make_degrees(0)));
    case FilterStyleValue::Kind::Color: {
        auto const& color = static_cast<ColorFilterStyleValue const&>(value);
        auto default_value = [&]() {
            switch (color.operation()) {
            case Gfx::ColorFilterType::Grayscale:
            case Gfx::ColorFilterType::Invert:
            case Gfx::ColorFilterType::Sepia:
                return 0.0;
            case Gfx::ColorFilterType::Brightness:
            case Gfx::ColorFilterType::Contrast:
            case Gfx::ColorFilterType::Opacity:
            case Gfx::ColorFilterType::Saturate:
                return 1.0;
            }
            VERIFY_NOT_REACHED();
        }();
        return ColorFilterStyleValue::create(color.operation(), NumberStyleValue::create(default_value));
    }
    }
    VERIFY_NOT_REACHED();
}

float BlurFilterStyleValue::resolved_radius() const
{
    return Length::from_style_value(m_radius, {}).absolute_length_to_px_without_rounding();
}

void BlurFilterStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append("blur("sv);
    m_radius->serialize(builder, mode);
    builder.append(')');
}

ValueComparingNonnullRefPtr<StyleValue const> BlurFilterStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto absolutized_radius = m_radius->absolutized(computation_context);
    if (absolutized_radius->equals(m_radius))
        return *this;
    return BlurFilterStyleValue::create(move(absolutized_radius));
}

bool BlurFilterStyleValue::equals(StyleValue const& other) const
{
    if (!other.is_filter() || other.as_filter().kind() != Kind::Blur)
        return false;
    auto const& other_blur = static_cast<BlurFilterStyleValue const&>(other);
    return m_radius == other_blur.m_radius;
}

void DropShadowFilterStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append("drop-shadow("sv);
    m_shadow->serialize(builder, mode);
    builder.append(')');
}

ValueComparingNonnullRefPtr<StyleValue const> DropShadowFilterStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto absolutized_offset_x = m_shadow->offset_x()->absolutized(computation_context);
    auto absolutized_offset_y = m_shadow->offset_y()->absolutized(computation_context);
    auto absolutized_radius = m_shadow->blur_radius_or_null() ? ValueComparingRefPtr<StyleValue const> { m_shadow->blur_radius_or_null()->absolutized(computation_context) } : nullptr;
    auto absolutized_color = m_shadow->color_or_null() ? ValueComparingRefPtr<StyleValue const> { m_shadow->color_or_null()->absolutized(computation_context) } : nullptr;

    if (absolutized_offset_x->equals(m_shadow->offset_x())
        && absolutized_offset_y->equals(m_shadow->offset_y())
        && absolutized_radius == m_shadow->blur_radius_or_null()
        && absolutized_color == m_shadow->color_or_null())
        return *this;

    return DropShadowFilterStyleValue::create(
        move(absolutized_offset_x),
        move(absolutized_offset_y),
        move(absolutized_radius),
        move(absolutized_color));
}

bool DropShadowFilterStyleValue::equals(StyleValue const& other) const
{
    if (!other.is_filter() || other.as_filter().kind() != Kind::DropShadow)
        return false;
    auto const& other_drop_shadow = static_cast<DropShadowFilterStyleValue const&>(other);
    return m_shadow == other_drop_shadow.m_shadow;
}

float HueRotateFilterStyleValue::angle_degrees() const
{
    return Angle::from_style_value(m_angle, {}).to_degrees();
}

void HueRotateFilterStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append("hue-rotate("sv);
    m_angle->serialize(builder, mode);
    builder.append(')');
}

ValueComparingNonnullRefPtr<StyleValue const> HueRotateFilterStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto absolutized_angle = m_angle->absolutized(computation_context);
    if (absolutized_angle->equals(m_angle))
        return *this;
    return HueRotateFilterStyleValue::create(move(absolutized_angle));
}

bool HueRotateFilterStyleValue::equals(StyleValue const& other) const
{
    if (!other.is_filter() || other.as_filter().kind() != Kind::HueRotate)
        return false;
    auto const& other_hue_rotate = static_cast<HueRotateFilterStyleValue const&>(other);
    return m_angle == other_hue_rotate.m_angle;
}

float ColorFilterStyleValue::resolved_amount() const
{
    return number_from_style_value(m_amount, 1);
}

void ColorFilterStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.appendff("{}(",
        [&] {
            switch (m_operation) {
            case Gfx::ColorFilterType::Brightness:
                return "brightness"sv;
            case Gfx::ColorFilterType::Contrast:
                return "contrast"sv;
            case Gfx::ColorFilterType::Grayscale:
                return "grayscale"sv;
            case Gfx::ColorFilterType::Invert:
                return "invert"sv;
            case Gfx::ColorFilterType::Opacity:
                return "opacity"sv;
            case Gfx::ColorFilterType::Saturate:
                return "saturate"sv;
            case Gfx::ColorFilterType::Sepia:
                return "sepia"sv;
            default:
                VERIFY_NOT_REACHED();
            }
        }());

    m_amount->serialize(builder, mode);
    builder.append(')');
}

ValueComparingNonnullRefPtr<StyleValue const> ColorFilterStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto absolutized_amount = number_from_style_value(m_amount->absolutized(computation_context), 1);

    if (first_is_one_of(m_operation, Gfx::ColorFilterType::Grayscale, Gfx::ColorFilterType::Invert, Gfx::ColorFilterType::Opacity, Gfx::ColorFilterType::Sepia))
        absolutized_amount = clamp(absolutized_amount, 0.0f, 1.0f);

    if (m_amount->is_number() && m_amount->as_number().number() == absolutized_amount)
        return *this;

    return ColorFilterStyleValue::create(m_operation, NumberStyleValue::create(absolutized_amount));
}

bool ColorFilterStyleValue::equals(StyleValue const& other) const
{
    if (!other.is_filter() || other.as_filter().kind() != Kind::Color)
        return false;
    auto const& other_color = static_cast<ColorFilterStyleValue const&>(other);
    return m_operation == other_color.m_operation
        && m_amount == other_color.m_amount;
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
