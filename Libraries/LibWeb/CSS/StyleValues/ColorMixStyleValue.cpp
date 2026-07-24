/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ColorMixStyleValue.h"
#include <AK/IntegralMath.h>
#include <AK/TypeCasts.h>
#include <LibGfx/ColorConversion.h>
#include <LibWeb/CSS/StyleValues/ColorFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorInterpolationMethodStyleValue.h>
#include <LibWeb/Layout/Node.h>

namespace Web::CSS {

// The Rust color conversion and interpolation code mirrors these enum values across FFI.
static_assert(to_underlying(ColorStyleValue::ColorType::RGB) == 0);
static_assert(to_underlying(ColorStyleValue::ColorType::A98RGB) == 1);
static_assert(to_underlying(ColorStyleValue::ColorType::DisplayP3) == 2);
static_assert(to_underlying(ColorStyleValue::ColorType::DisplayP3Linear) == 3);
static_assert(to_underlying(ColorStyleValue::ColorType::HSL) == 4);
static_assert(to_underlying(ColorStyleValue::ColorType::HWB) == 5);
static_assert(to_underlying(ColorStyleValue::ColorType::Lab) == 6);
static_assert(to_underlying(ColorStyleValue::ColorType::LCH) == 7);
static_assert(to_underlying(ColorStyleValue::ColorType::OKLab) == 8);
static_assert(to_underlying(ColorStyleValue::ColorType::OKLCH) == 9);
static_assert(to_underlying(ColorStyleValue::ColorType::sRGB) == 10);
static_assert(to_underlying(ColorStyleValue::ColorType::sRGBLinear) == 11);
static_assert(to_underlying(ColorStyleValue::ColorType::ProPhotoRGB) == 12);
static_assert(to_underlying(ColorStyleValue::ColorType::Rec2020) == 13);
static_assert(to_underlying(ColorStyleValue::ColorType::XYZD50) == 14);
static_assert(to_underlying(ColorStyleValue::ColorType::XYZD65) == 15);
static_assert(to_underlying(Gfx::RectangularColorSpace::Srgb) == 0);
static_assert(to_underlying(Gfx::RectangularColorSpace::SrgbLinear) == 1);
static_assert(to_underlying(Gfx::RectangularColorSpace::DisplayP3) == 2);
static_assert(to_underlying(Gfx::RectangularColorSpace::DisplayP3Linear) == 3);
static_assert(to_underlying(Gfx::RectangularColorSpace::A98Rgb) == 4);
static_assert(to_underlying(Gfx::RectangularColorSpace::ProphotoRgb) == 5);
static_assert(to_underlying(Gfx::RectangularColorSpace::Rec2020) == 6);
static_assert(to_underlying(Gfx::RectangularColorSpace::Lab) == 7);
static_assert(to_underlying(Gfx::RectangularColorSpace::Oklab) == 8);
static_assert(to_underlying(Gfx::RectangularColorSpace::Xyz) == 9);
static_assert(to_underlying(Gfx::RectangularColorSpace::XyzD50) == 10);
static_assert(to_underlying(Gfx::RectangularColorSpace::XyzD65) == 11);
static_assert(to_underlying(Gfx::PolarColorSpace::Hsl) == 0);
static_assert(to_underlying(Gfx::PolarColorSpace::Hwb) == 1);
static_assert(to_underlying(Gfx::PolarColorSpace::Lch) == 2);
static_assert(to_underlying(Gfx::PolarColorSpace::Oklch) == 3);
static_assert(to_underlying(Gfx::HueInterpolationMethod::Shorter) == 0);
static_assert(to_underlying(Gfx::HueInterpolationMethod::Longer) == 1);
static_assert(to_underlying(Gfx::HueInterpolationMethod::Increasing) == 2);
static_assert(to_underlying(Gfx::HueInterpolationMethod::Decreasing) == 3);

static bool is_missing_color_component(StyleValue const& component)
{
    return component.to_keyword() == Keyword::None;
}

static Optional<Gfx::ColorComponents> resolve_native_color_components(StyleValue const& style_value, CalculationResolutionContext const& context)
{
    if (!style_value.is_color_function())
        return {};

    auto const& color = as<ColorFunctionStyleValue>(style_value);
    auto color_type = color.color_type();
    if (!color_type.has_value() || color.origin_color())
        return {};

    auto resolve_alpha = [&](ValueComparingRefPtr<StyleValue const> const& alpha_style_value) -> Optional<float> {
        // An omitted alpha on a ColorFunctionStyleValue is treated as 1 for interpolation.
        if (!alpha_style_value)
            return 1.0f;
        auto result = ColorStyleValue::resolve_alpha(*alpha_style_value, context);
        if (!result.has_value())
            return {};
        return static_cast<float>(result.value());
    };

    switch (*color_type) {
    case ColorStyleValue::ColorType::HSL: {
        auto h = ColorStyleValue::resolve_hue(color.channel(0), context);
        auto s = ColorStyleValue::resolve_with_reference_value(color.channel(1), 100.0f, context);
        auto l = ColorStyleValue::resolve_with_reference_value(color.channel(2), 100.0f, context);
        auto a = resolve_alpha(color.alpha());
        if (!h.has_value() || !s.has_value() || !l.has_value() || !a.has_value())
            return {};
        return Gfx::ColorComponents { static_cast<float>(h.value()), static_cast<float>(s.value() / 100.0), static_cast<float>(l.value() / 100.0), a.value() };
    }
    case ColorStyleValue::ColorType::HWB: {
        auto h = ColorStyleValue::resolve_hue(color.channel(0), context);
        auto w = ColorStyleValue::resolve_with_reference_value(color.channel(1), 100.0f, context);
        auto b = ColorStyleValue::resolve_with_reference_value(color.channel(2), 100.0f, context);
        auto a = resolve_alpha(color.alpha());
        if (!h.has_value() || !w.has_value() || !b.has_value() || !a.has_value())
            return {};
        return Gfx::ColorComponents { static_cast<float>(h.value()), static_cast<float>(w.value() / 100.0), static_cast<float>(b.value() / 100.0), a.value() };
    }
    case ColorStyleValue::ColorType::Lab: {
        auto l = ColorStyleValue::resolve_with_reference_value(color.channel(0), 100.0f, context);
        auto a_component = ColorStyleValue::resolve_with_reference_value(color.channel(1), 125.0f, context);
        auto b_component = ColorStyleValue::resolve_with_reference_value(color.channel(2), 125.0f, context);
        auto alpha = resolve_alpha(color.alpha());
        if (!l.has_value() || !a_component.has_value() || !b_component.has_value() || !alpha.has_value())
            return {};
        return Gfx::ColorComponents { static_cast<float>(l.value()), static_cast<float>(a_component.value()), static_cast<float>(b_component.value()), alpha.value() };
    }
    case ColorStyleValue::ColorType::OKLab: {
        auto l = ColorStyleValue::resolve_with_reference_value(color.channel(0), 1.0f, context);
        auto a_component = ColorStyleValue::resolve_with_reference_value(color.channel(1), 0.4f, context);
        auto b_component = ColorStyleValue::resolve_with_reference_value(color.channel(2), 0.4f, context);
        auto alpha = resolve_alpha(color.alpha());
        if (!l.has_value() || !a_component.has_value() || !b_component.has_value() || !alpha.has_value())
            return {};
        return Gfx::ColorComponents { static_cast<float>(l.value()), static_cast<float>(a_component.value()), static_cast<float>(b_component.value()), alpha.value() };
    }
    case ColorStyleValue::ColorType::LCH: {
        auto l = ColorStyleValue::resolve_with_reference_value(color.channel(0), 100.0f, context);
        auto c = ColorStyleValue::resolve_with_reference_value(color.channel(1), 150.0f, context);
        auto h = ColorStyleValue::resolve_hue(color.channel(2), context);
        auto a = resolve_alpha(color.alpha());
        if (!l.has_value() || !c.has_value() || !h.has_value() || !a.has_value())
            return {};
        return Gfx::ColorComponents { static_cast<float>(l.value()), static_cast<float>(c.value()), static_cast<float>(h.value()), a.value() };
    }
    case ColorStyleValue::ColorType::OKLCH: {
        auto l = ColorStyleValue::resolve_with_reference_value(color.channel(0), 1.0f, context);
        auto c = ColorStyleValue::resolve_with_reference_value(color.channel(1), 0.4f, context);
        auto h = ColorStyleValue::resolve_hue(color.channel(2), context);
        auto a = resolve_alpha(color.alpha());
        if (!l.has_value() || !c.has_value() || !h.has_value() || !a.has_value())
            return {};
        return Gfx::ColorComponents { static_cast<float>(l.value()), static_cast<float>(c.value()), static_cast<float>(h.value()), a.value() };
    }
    case ColorStyleValue::ColorType::RGB: {
        auto r = ColorStyleValue::resolve_with_reference_value(color.channel(0), 255.0f, context);
        auto g = ColorStyleValue::resolve_with_reference_value(color.channel(1), 255.0f, context);
        auto b = ColorStyleValue::resolve_with_reference_value(color.channel(2), 255.0f, context);
        auto a = resolve_alpha(color.alpha());
        if (!r.has_value() || !g.has_value() || !b.has_value() || !a.has_value())
            return {};
        return Gfx::ColorComponents {
            static_cast<float>(clamp(r.value(), 0.0, 255.0) / 255.0),
            static_cast<float>(clamp(g.value(), 0.0, 255.0) / 255.0),
            static_cast<float>(clamp(b.value(), 0.0, 255.0) / 255.0),
            a.value(),
        };
    }
    default: {
        auto first = ColorStyleValue::resolve_with_reference_value(color.channel(0), 1.0f, context);
        auto second = ColorStyleValue::resolve_with_reference_value(color.channel(1), 1.0f, context);
        auto third = ColorStyleValue::resolve_with_reference_value(color.channel(2), 1.0f, context);
        auto alpha = resolve_alpha(color.alpha());
        if (!first.has_value() || !second.has_value() || !third.has_value() || !alpha.has_value())
            return {};
        return Gfx::ColorComponents { static_cast<float>(first.value()), static_cast<float>(second.value()), static_cast<float>(third.value()), alpha.value() };
    }
    }
}

static Optional<StyleValueFFI::FfiResolvedColor> resolve_color_for_rust_interpolation(StyleValue const& input, ColorResolutionContext const& context)
{
    RefPtr<StyleValue const> resolved_relative_color;
    auto const* style_value = &input;
    if (input.is_color_function()) {
        auto const& color_function = as<ColorFunctionStyleValue>(input);
        if (color_function.origin_color()) {
            resolved_relative_color = color_function.resolve_relative_form(context);
            if (!resolved_relative_color)
                return {};
            style_value = resolved_relative_color.ptr();
        }
    }

    StyleValueFFI::FfiResolvedColor result {};
    if (auto native_components = resolve_native_color_components(*style_value, context.calculation_resolution_context); native_components.has_value()) {
        auto const& color_function = as<ColorFunctionStyleValue>(*style_value);
        result.color_type = to_underlying(color_function.color_type().value());
        result.components[0] = (*native_components)[0];
        result.components[1] = (*native_components)[1];
        result.components[2] = (*native_components)[2];
        result.components[3] = native_components->alpha();
        result.missing[0] = is_missing_color_component(color_function.channel(0));
        result.missing[1] = is_missing_color_component(color_function.channel(1));
        result.missing[2] = is_missing_color_component(color_function.channel(2));
        result.missing[3] = color_function.alpha() && is_missing_color_component(*color_function.alpha());
        result.has_native_components = true;
        return result;
    }

    auto resolved_color = style_value->to_color(context);
    if (!resolved_color.has_value())
        return {};
    auto components = Gfx::color_to_srgb(*resolved_color);
    result.color_type = to_underlying(ColorStyleValue::ColorType::sRGB);
    result.components[0] = components[0];
    result.components[1] = components[1];
    result.components[2] = components[2];
    result.components[3] = components.alpha();
    return result;
}

static RefPtr<StyleValue const> interpolate_color_in_rust(StyleValue const& from, StyleValue const& to, float delta, double alpha_multiplier, StyleValue const& color_interpolation_method, ColorResolutionContext const& context)
{
    auto resolved_from = resolve_color_for_rust_interpolation(from, context);
    auto resolved_to = resolve_color_for_rust_interpolation(to, context);
    if (!resolved_from.has_value() || !resolved_to.has_value())
        return {};
    auto const* result = StyleValueFFI::rust_interpolate_color(&*resolved_from, &*resolved_to, color_interpolation_method.rust_style_value_data(), delta, static_cast<float>(alpha_multiplier));
    if (!result)
        return {};
    return StyleValue::adopt_rust_style_value_data(result);
}

ValueComparingNonnullRefPtr<ColorMixStyleValue const> ColorMixStyleValue::create(RefPtr<StyleValue const> color_interpolation_method, ColorMixComponent first_component, ColorMixComponent second_component)
{
    return adopt_ref(*new (nothrow) ColorMixStyleValue(move(color_interpolation_method), move(first_component), move(second_component)));
}

ColorMixStyleValue::ColorMixStyleValue(RefPtr<StyleValue const> color_interpolation_method, ColorMixComponent first_component, ColorMixComponent second_component)
    : ColorStyleValue(make_color_mix_data(color_interpolation_method, first_component, second_component))
    , m_color_interpolation_method(move(color_interpolation_method))
    , m_first_component(move(first_component))
    , m_second_component(move(second_component))
{
}

ColorMixStyleValue::ColorMixStyleValue(StyleValueFFI::StyleValueData const* data)
    : ColorStyleValue(data)
    , m_color_interpolation_method([&]() -> ValueComparingRefPtr<StyleValue const> {
        auto const* child_data = static_cast<StyleValueFFI::StyleValueData const*>(data->color_mix.color_interpolation_method.pointer);
        if (!child_data)
            return nullptr;
        return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(child_data));
    }())
    , m_first_component([&] {
        auto const& color_mix = data->color_mix;
        auto color = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(color_mix.first_color.pointer)));
        ValueComparingRefPtr<StyleValue const> percentage;
        if (auto const* percentage_data = static_cast<StyleValueFFI::StyleValueData const*>(color_mix.first_percentage.pointer))
            percentage = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(percentage_data));
        return ColorMixComponent { move(color), move(percentage) };
    }())
    , m_second_component([&] {
        auto const& color_mix = data->color_mix;
        auto color = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(color_mix.second_color.pointer)));
        ValueComparingRefPtr<StyleValue const> percentage;
        if (auto const* percentage_data = static_cast<StyleValueFFI::StyleValueData const*>(color_mix.second_percentage.pointer))
            percentage = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(percentage_data));
        return ColorMixComponent { move(color), move(percentage) };
    }())
{
}

bool ColorMixStyleValue::equals(StyleValue const& other) const
{
    auto const* other_color_mix = as_if<ColorMixStyleValue>(other);
    if (!other_color_mix)
        return false;
    return color_interpolation_method_value() == other_color_mix->color_interpolation_method_value()
        && first_component() == other_color_mix->first_component()
        && second_component() == other_color_mix->second_component();
}

// https://drafts.csswg.org/css-color-5/#serial-color-mix
void ColorMixStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    auto serialize_first_percentage = [&mode](StringBuilder& builder, RefPtr<StyleValue const> const& p1, RefPtr<StyleValue const> const& p2) {
        // if BOTH the first percentage p1 and second percentage p2 are specified:
        if (p1 && p2) {
            // If both p1 equals 50% and p2 equals 50%, nothing is serialized.
            if (p1->is_percentage() && p2->is_percentage() && p1->as_percentage().percentage().value() == 50 && p2->as_percentage().percentage().value() == 50)
                return;

            // else, p1 is serialized as is.
            builder.append(' ');
            p1->serialize(builder, mode);
        }
        // else if ONLY the first percentage p1 is specified:
        else if (p1) {
            // If p1 is equal to 50%, nothing is serialized.
            if (p1->is_percentage() && p1->as_percentage().percentage().value() == 50)
                return;

            // else, p1 is serialized as is.
            builder.append(' ');
            p1->serialize(builder, mode);
        }
        // else if ONLY the second percentage p2 is specified:
        else if (p2) {
            // if p2 equals 50%, nothing is serialized.
            if (p2->is_percentage() && p2->as_percentage().percentage().value() == 50)
                return;

            // if p2 is not calc(), the value of 100% - p2 is serialized.
            if (!p2->is_calculated())
                builder.appendff(" {}%", 100 - p2->as_percentage().percentage().value());

            // else, nothing is serialized.
        }
        // else if NEITHER is specified:
        else {
            // nothing is serialized.
        }
    };

    auto serialize_second_percentage = [&mode](StringBuilder& builder, RefPtr<StyleValue const> const& p1, RefPtr<StyleValue const> const& p2) {
        // If BOTH the first percentage p1 and second percentages p2 are specified:
        if (p1 && p2) {
            // if neither p1 nor p2 is calc(), and p1 + p2 equals 100%, nothing is serialized.
            if (p1->is_percentage() && p2->is_percentage() && p1->as_percentage().percentage().value() + p2->as_percentage().percentage().value() == 100)
                return;

            // else, p2 is serialized as is.
            builder.append(' ');
            p2->serialize(builder, mode);
        }
        // else if ONLY the first percentage p1 is specified:
        else if (p1) {
            // nothing is serialized.
        }
        // else if ONLY the second percentage p2 is specified:
        else if (p2) {
            // if p2 equals 50%, nothing is serialized.
            if (p2->is_percentage() && p2->as_percentage().percentage().value() == 50)
                return;

            // if p2 is not calc(), nothing is serialized.
            if (!p2->is_calculated())
                return;

            // else, p2 is serialized as is.
            builder.append(' ');
            p2->serialize(builder, mode);
        }
        // else if NEITHER is specified:
        else {
            // nothing is serialized.
        }
    };

    builder.append("color-mix("sv);

    if (color_interpolation_method_value() && color_interpolation_method_value()->as_color_interpolation_method().color_interpolation_method() != RectangularColorSpace::Oklab) {
        color_interpolation_method_value()->serialize(builder, mode);
        builder.append(", "sv);
    }

    first_component().color->serialize(builder, mode);
    serialize_first_percentage(builder, first_component().percentage, second_component().percentage);
    builder.append(", "sv);
    second_component().color->serialize(builder, mode);
    serialize_second_percentage(builder, first_component().percentage, second_component().percentage);
    builder.append(')');
}

// https://drafts.csswg.org/css-color-5/#color-mix-percent-norm
ColorMixStyleValue::NormalizedPercentages ColorMixStyleValue::normalize_percentage_pair(Optional<Percentage> p1, Optional<Percentage> p2)
{
    double alpha_multiplier = 1.0;

    // 1. Let p1 be the first percentage and p2 the second one.
    // NB: Provided by the caller.

    // 2. If both percentages are omitted, they each default to 50% (an equal mix of the two colors).
    if (!p1.has_value() && !p2.has_value()) {
        p1 = Percentage(50);
        p2 = Percentage(50);
    }
    // 3. Otherwise, if p2 is omitted, it becomes 100% - p1
    else if (!p2.has_value()) {
        p2 = Percentage(100 - p1->value());
    }
    // 4. Otherwise, if p1 is omitted, it becomes 100% - p2
    else if (!p1.has_value()) {
        p1 = Percentage(100 - p2->value());
    }
    // 5. Otherwise, if both are provided and add up to greater than 100%, they are scaled accordingly so that they add up to 100%.
    else if (p1->value() + p2->value() > 100) {
        auto sum = p1->value() + p2->value();
        p1 = Percentage((p1->value() / sum) * 100);
        p2 = Percentage((p2->value() / sum) * 100);
    }
    // 6. Otherwise, if both are provided and add up to less than 100%, the sum is saved as an alpha multiplier. If the sum is greater than zero, they are then scaled accordingly so that they add up to 100%.
    else if (p1->value() + p2->value() < 100) {
        auto sum = p1->value() + p2->value();
        alpha_multiplier = sum / 100;
        if (sum > 0) {
            p1 = Percentage((p1->value() / sum) * 100);
            p2 = Percentage((p2->value() / sum) * 100);
        }
    }

    VERIFY(p1.has_value());
    VERIFY(p2.has_value());

    return { .first_percentage = *p1, .second_percentage = *p2, .alpha_multiplier = alpha_multiplier };
}

ColorMixStyleValue::PercentageNormalizationResult ColorMixStyleValue::normalize_percentages(ComputationContext const& computation_context) const
{
    auto p1 = first_component().percentage
        ? Percentage::from_style_value(first_component().percentage->absolutized(computation_context))
        : Optional<Percentage> {};
    auto p2 = second_component().percentage
        ? Percentage::from_style_value(second_component().percentage->absolutized(computation_context))
        : Optional<Percentage> {};

    auto result = normalize_percentage_pair(p1, p2);
    return {
        .p1 = PercentageStyleValue::create(result.first_percentage),
        .p2 = PercentageStyleValue::create(result.second_percentage),
        .alpha_multiplier = result.alpha_multiplier,
    };
}

// https://drafts.csswg.org/css-color-5/#color-mix-result
Optional<Color> ColorMixStyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    auto p1 = first_component().percentage
        ? Optional<Percentage>(Percentage::from_style_value(*first_component().percentage))
        : Optional<Percentage> {};
    auto p2 = second_component().percentage
        ? Optional<Percentage>(Percentage::from_style_value(*second_component().percentage))
        : Optional<Percentage> {};
    auto normalized = normalize_percentage_pair(p1, p2);

    auto default_color_interpolation_method = ColorInterpolationMethodStyleValue::create(RectangularColorSpace::Oklab);
    auto const& color_interpolation_method = color_interpolation_method_value()
        ? *color_interpolation_method_value()
        : static_cast<StyleValue const&>(*default_color_interpolation_method);
    auto style_value = interpolate_color_in_rust(
        *first_component().color,
        *second_component().color,
        normalized.second_percentage.as_fraction(),
        normalized.alpha_multiplier,
        color_interpolation_method,
        color_resolution_context);
    if (!style_value)
        return {};
    return style_value->to_color(color_resolution_context);
}

ValueComparingNonnullRefPtr<StyleValue const> ColorMixStyleValue::absolutized(ComputationContext const& context) const
{
    // FIXME: Follow the spec algorithm. https://drafts.csswg.org/css-color-5/#calculate-a-color-mix

    auto normalized_percentages = normalize_percentages(context);
    ColorResolutionContext color_resolution_context {
        .color_scheme = context.color_scheme,
        .current_color = {},
        .current_color_style_value = nullptr,
        .calculation_resolution_context = CalculationResolutionContext::from_computation_context(context),
    };
    auto absolutized_color_interpolation_method = color_interpolation_method_value() ? ValueComparingRefPtr<StyleValue const> { color_interpolation_method_value()->absolutized(context) } : nullptr;

    auto delta = Percentage::from_style_value(normalized_percentages.p2).as_fraction();

    auto default_color_interpolation_method = ColorInterpolationMethodStyleValue::create(RectangularColorSpace::Oklab);
    auto const& color_interpolation_method = absolutized_color_interpolation_method
        ? *absolutized_color_interpolation_method
        : static_cast<StyleValue const&>(*default_color_interpolation_method);

    // Resolve relative-color components before interpolation so channel-keyword references and `none` missing-channel
    // markers are made visible during interpolation.
    auto resolve_if_relative = [&](ValueComparingNonnullRefPtr<StyleValue const> const& component) -> ValueComparingNonnullRefPtr<StyleValue const> {
        if (!component->is_color_function())
            return component;
        auto const& color_function = as<ColorFunctionStyleValue>(*component);
        if (!color_function.origin_color())
            return component;
        auto resolved = color_function.resolve_relative_form(color_resolution_context);
        if (!resolved)
            return component;
        return resolved.release_nonnull();
    };
    auto resolved_first_color = resolve_if_relative(this->first_component().color);
    auto resolved_second_color = resolve_if_relative(this->second_component().color);

    if (auto style_value = interpolate_color_in_rust(
            *resolved_first_color,
            *resolved_second_color,
            delta,
            normalized_percentages.alpha_multiplier,
            color_interpolation_method,
            color_resolution_context))
        return style_value.release_nonnull();

    // Fall back to returning a color-mix() with absolutized values if we can't compute completely.
    // Currently, this is only the case if one of our colors relies on `currentcolor`, as that does not compute to a color value.
    auto absolutized_first_color = first_component().color->absolutized(context);
    auto absolutized_second_color = second_component().color->absolutized(context);
    if (absolutized_first_color == first_component().color && normalized_percentages.p1 == first_component().percentage
        && absolutized_second_color == second_component().color && normalized_percentages.p2 == second_component().percentage)
        return *this;

    return ColorMixStyleValue::create(
        absolutized_color_interpolation_method,
        ColorMixComponent { .color = move(absolutized_first_color), .percentage = normalized_percentages.p1 },
        ColorMixComponent { .color = move(absolutized_second_color), .percentage = normalized_percentages.p2 });
}

}
