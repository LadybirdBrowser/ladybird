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

bool FilterStyleValue::equals(StyleValue const& other) const
{
    switch (kind()) {
    case Kind::Blur:
        return static_cast<BlurFilterStyleValue const&>(*this).equals(other);
    case Kind::DropShadow:
        return static_cast<DropShadowFilterStyleValue const&>(*this).equals(other);
    case Kind::HueRotate:
        return static_cast<HueRotateFilterStyleValue const&>(*this).equals(other);
    case Kind::Color:
        return static_cast<ColorFilterStyleValue const&>(*this).equals(other);
    }
    VERIFY_NOT_REACHED();
}

void FilterStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    switch (kind()) {
    case Kind::Blur:
        return static_cast<BlurFilterStyleValue const&>(*this).serialize(builder, mode);
    case Kind::DropShadow:
        return static_cast<DropShadowFilterStyleValue const&>(*this).serialize(builder, mode);
    case Kind::HueRotate:
        return static_cast<HueRotateFilterStyleValue const&>(*this).serialize(builder, mode);
    case Kind::Color:
        return static_cast<ColorFilterStyleValue const&>(*this).serialize(builder, mode);
    }
    VERIFY_NOT_REACHED();
}

float BlurFilterStyleValue::resolved_radius() const
{
    return Length::from_style_value(radius(), {}).absolute_length_to_px_without_rounding();
}

void BlurFilterStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append("blur("sv);
    radius()->serialize(builder, mode);
    builder.append(')');
}

ValueComparingNonnullRefPtr<StyleValue const> BlurFilterStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto radius = this->radius();
    auto absolutized_radius = radius->absolutized(computation_context);
    if (absolutized_radius->equals(radius))
        return *this;
    return BlurFilterStyleValue::create(move(absolutized_radius));
}

bool BlurFilterStyleValue::equals(StyleValue const& other) const
{
    if (!other.is_filter() || other.as_filter().kind() != Kind::Blur)
        return false;
    auto const& other_blur = static_cast<BlurFilterStyleValue const&>(other);
    return radius() == other_blur.radius();
}

void DropShadowFilterStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append("drop-shadow("sv);
    shadow().serialize(builder, mode);
    builder.append(')');
}

ValueComparingNonnullRefPtr<StyleValue const> DropShadowFilterStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto const& shadow = this->shadow();
    auto absolutized_offset_x = shadow.offset_x()->absolutized(computation_context);
    auto absolutized_offset_y = shadow.offset_y()->absolutized(computation_context);
    auto absolutized_radius = shadow.blur_radius_or_null() ? ValueComparingRefPtr<StyleValue const> { shadow.blur_radius_or_null()->absolutized(computation_context) } : nullptr;
    auto absolutized_color = shadow.color_or_null() ? ValueComparingRefPtr<StyleValue const> { shadow.color_or_null()->absolutized(computation_context) } : nullptr;

    if (absolutized_offset_x->equals(shadow.offset_x())
        && absolutized_offset_y->equals(shadow.offset_y())
        && absolutized_radius == shadow.blur_radius_or_null()
        && absolutized_color == shadow.color_or_null())
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
    return shadow_style_value() == other_drop_shadow.shadow_style_value();
}

float HueRotateFilterStyleValue::angle_degrees() const
{
    return Angle::from_style_value(angle(), {}).to_degrees();
}

void HueRotateFilterStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append("hue-rotate("sv);
    angle()->serialize(builder, mode);
    builder.append(')');
}

ValueComparingNonnullRefPtr<StyleValue const> HueRotateFilterStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto angle = this->angle();
    auto absolutized_angle = angle->absolutized(computation_context);
    if (absolutized_angle->equals(angle))
        return *this;
    return HueRotateFilterStyleValue::create(move(absolutized_angle));
}

bool HueRotateFilterStyleValue::equals(StyleValue const& other) const
{
    if (!other.is_filter() || other.as_filter().kind() != Kind::HueRotate)
        return false;
    auto const& other_hue_rotate = static_cast<HueRotateFilterStyleValue const&>(other);
    return angle() == other_hue_rotate.angle();
}

float ColorFilterStyleValue::resolved_amount() const
{
    return number_from_style_value(amount(), 1);
}

void ColorFilterStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.appendff("{}(",
        [&] {
            switch (operation()) {
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

    amount()->serialize(builder, mode);
    builder.append(')');
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

bool ColorFilterStyleValue::equals(StyleValue const& other) const
{
    if (!other.is_filter() || other.as_filter().kind() != Kind::Color)
        return false;
    auto const& other_color = static_cast<ColorFilterStyleValue const&>(other);
    return operation() == other_color.operation()
        && amount() == other_color.amount();
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
