/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IntegralMath.h>
#include <LibGfx/ColorConversion.h>
#include <LibWeb/CSS/ColorInterpolation.h>
#include <LibWeb/CSS/Interpolation.h>
#include <LibWeb/CSS/StyleValues/ColorFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/HSLColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/HWBColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LCHLikeColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/LabLikeColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/RGBColorStyleValue.h>

namespace Web::CSS {

static float interpolate_color_component(float from, float to, float delta)
{
    return from + (to - from) * delta;
}

// https://drafts.csswg.org/css-color-4/#hue-interpolation
static void fixup_hue(float& hue1, float& hue2, HueInterpolationMethod hue_interpolation_method)
{
    auto difference = hue2 - hue1;
    switch (hue_interpolation_method) {
    // https://drafts.csswg.org/css-color-4/#hue-shorter
    case HueInterpolationMethod::Shorter:
        if (difference > 180.0f)
            hue1 += 360.0f;
        else if (difference < -180.0f)
            hue2 += 360.0f;
        break;
    // https://drafts.csswg.org/css-color-4/#hue-longer
    case HueInterpolationMethod::Longer:
        if (difference > 0.0f && difference < 180.0f)
            hue1 += 360.0f;
        else if (difference > -180.0f && difference <= 0.0f)
            hue2 += 360.0f;
        break;
    // https://drafts.csswg.org/css-color-4/#hue-increasing
    case HueInterpolationMethod::Increasing:
        if (hue2 < hue1)
            hue2 += 360.0f;
        break;
    // https://drafts.csswg.org/css-color-4/#hue-decreasing
    case HueInterpolationMethod::Decreasing:
        if (hue1 < hue2)
            hue1 += 360.0f;
        break;
    }
}

static Gfx::ColorComponents srgb_to_rectangular_color_space(Gfx::ColorComponents srgb, RectangularColorSpace space)
{
    if (space == RectangularColorSpace::Srgb)
        return srgb;
    if (space == RectangularColorSpace::SrgbLinear)
        return Gfx::srgb_to_linear_srgb(srgb);

    auto xyz65 = Gfx::linear_srgb_to_xyz65(Gfx::srgb_to_linear_srgb(srgb));
    switch (space) {
    case RectangularColorSpace::Srgb:
    case RectangularColorSpace::SrgbLinear:
        VERIFY_NOT_REACHED();
    case RectangularColorSpace::DisplayP3:
        return Gfx::linear_display_p3_to_display_p3(Gfx::xyz65_to_linear_display_p3(xyz65));
    case RectangularColorSpace::DisplayP3Linear:
        return Gfx::xyz65_to_linear_display_p3(xyz65);
    case RectangularColorSpace::A98Rgb:
        return Gfx::linear_a98_rgb_to_a98_rgb(Gfx::xyz65_to_linear_a98_rgb(xyz65));
    case RectangularColorSpace::ProphotoRgb:
        return Gfx::linear_prophoto_rgb_to_prophoto_rgb(Gfx::xyz50_to_linear_prophoto_rgb(Gfx::xyz65_to_xyz50(xyz65)));
    case RectangularColorSpace::Rec2020:
        return Gfx::linear_rec2020_to_rec2020(Gfx::xyz65_to_linear_rec2020(xyz65));
    case RectangularColorSpace::Lab:
        return Gfx::xyz50_to_lab(Gfx::xyz65_to_xyz50(xyz65));
    case RectangularColorSpace::Oklab:
        return Gfx::xyz65_to_oklab(xyz65);
    case RectangularColorSpace::Xyz:
    case RectangularColorSpace::XyzD65:
        return xyz65;
    case RectangularColorSpace::XyzD50:
        return Gfx::xyz65_to_xyz50(xyz65);
    }
    VERIFY_NOT_REACHED();
}

static Gfx::ColorComponents srgb_to_polar_color_space(Gfx::ColorComponents srgb, PolarColorSpace space)
{
    switch (space) {
    case PolarColorSpace::Hsl:
        return Gfx::srgb_to_hsl(srgb);
    case PolarColorSpace::Hwb:
        return Gfx::srgb_to_hwb(srgb);
    case PolarColorSpace::Lch: {
        auto xyz65 = Gfx::linear_srgb_to_xyz65(Gfx::srgb_to_linear_srgb(srgb));
        return Gfx::lab_to_lch(Gfx::xyz50_to_lab(Gfx::xyz65_to_xyz50(xyz65)));
    }
    case PolarColorSpace::Oklch: {
        auto xyz65 = Gfx::linear_srgb_to_xyz65(Gfx::srgb_to_linear_srgb(srgb));
        return Gfx::oklab_to_oklch(Gfx::xyz65_to_oklab(xyz65));
    }
    }
    VERIFY_NOT_REACHED();
}

static bool is_component_none(StyleValue const& component)
{
    return component.to_keyword() == Keyword::None;
}

static MissingComponents extract_missing_components(StyleValue const& style_value)
{
    if (!style_value.is_color())
        return {};

    auto const& color = style_value.as_color();
    switch (color.color_type()) {
    case ColorStyleValue::ColorType::HSL: {
        auto const& hsl = as<HSLColorStyleValue>(color);
        return { is_component_none(hsl.h()), is_component_none(hsl.s()), is_component_none(hsl.l()), is_component_none(hsl.alpha()) };
    }
    case ColorStyleValue::ColorType::HWB: {
        auto const& hwb = as<HWBColorStyleValue>(color);
        return { is_component_none(hwb.h()), is_component_none(hwb.w()), is_component_none(hwb.b()), is_component_none(hwb.alpha()) };
    }
    case ColorStyleValue::ColorType::Lab: {
        auto const& lab = as<LabColorStyleValue>(color);
        return { is_component_none(lab.l()), is_component_none(lab.a()), is_component_none(lab.b()), is_component_none(lab.alpha()) };
    }
    case ColorStyleValue::ColorType::OKLab: {
        auto const& oklab = as<OKLabColorStyleValue>(color);
        return { is_component_none(oklab.l()), is_component_none(oklab.a()), is_component_none(oklab.b()), is_component_none(oklab.alpha()) };
    }
    case ColorStyleValue::ColorType::LCH: {
        auto const& lch = as<LCHColorStyleValue>(color);
        return { is_component_none(lch.l()), is_component_none(lch.c()), is_component_none(lch.h()), is_component_none(lch.alpha()) };
    }
    case ColorStyleValue::ColorType::OKLCH: {
        auto const& oklch = as<OKLCHColorStyleValue>(color);
        return { is_component_none(oklch.l()), is_component_none(oklch.c()), is_component_none(oklch.h()), is_component_none(oklch.alpha()) };
    }
    case ColorStyleValue::ColorType::RGB: {
        auto const& rgb = as<RGBColorStyleValue>(color);
        return { is_component_none(rgb.r()), is_component_none(rgb.g()), is_component_none(rgb.b()), is_component_none(rgb.alpha()) };
    }
    default:
        if (color.is_color_function()) {
            auto const& func = as<ColorFunctionStyleValue>(color);
            return { is_component_none(func.channel(0)), is_component_none(func.channel(1)), is_component_none(func.channel(2)), is_component_none(func.alpha()) };
        }
        return {};
    }
}

// https://drafts.csswg.org/css-color-4/#interpolation-missing
// Analogous component categories for carrying forward missing components across color spaces.
// Each input color space component is classified into a category. If a missing component in
// the source space has an analogous component in the interpolation space, it is carried forward.
// https://drafts.csswg.org/css-color-4/#interpolation-missing
static ComponentCategories categories_for_rectangular_space(RectangularColorSpace space)
{
    switch (space) {
    case RectangularColorSpace::Srgb:
    case RectangularColorSpace::SrgbLinear:
    case RectangularColorSpace::DisplayP3:
    case RectangularColorSpace::DisplayP3Linear:
    case RectangularColorSpace::A98Rgb:
    case RectangularColorSpace::ProphotoRgb:
    case RectangularColorSpace::Rec2020:
        return { ComponentCategory::Red, ComponentCategory::Green, ComponentCategory::Blue };
    case RectangularColorSpace::Xyz:
    case RectangularColorSpace::XyzD50:
    case RectangularColorSpace::XyzD65:
        // NOTE: The spec says XYZ spaces are considered super-saturated RGB for this purpose.
        return { ComponentCategory::Red, ComponentCategory::Green, ComponentCategory::Blue };
    case RectangularColorSpace::Lab:
    case RectangularColorSpace::Oklab:
        return { ComponentCategory::Lightness, ComponentCategory::OpponentA, ComponentCategory::OpponentB };
    }
    VERIFY_NOT_REACHED();
}

static ComponentCategories categories_for_polar_space(PolarColorSpace space)
{
    switch (space) {
    case PolarColorSpace::Hsl:
        return { ComponentCategory::Hue, ComponentCategory::Colorfulness, ComponentCategory::Lightness };
    case PolarColorSpace::Hwb:
        // NOTE: Whiteness and Blackness have no analogs in other color spaces.
        return { ComponentCategory::Hue, ComponentCategory::NotAnalogous, ComponentCategory::NotAnalogous };
    case PolarColorSpace::Lch:
    case PolarColorSpace::Oklch:
        return { ComponentCategory::Lightness, ComponentCategory::Colorfulness, ComponentCategory::Hue };
    }
    VERIFY_NOT_REACHED();
}

static ComponentCategories categories_for_color_type(ColorStyleValue::ColorType color_type)
{
    switch (color_type) {
    case ColorStyleValue::ColorType::HSL:
        return { ComponentCategory::Hue, ComponentCategory::Colorfulness, ComponentCategory::Lightness };
    case ColorStyleValue::ColorType::HWB:
        return { ComponentCategory::Hue, ComponentCategory::NotAnalogous, ComponentCategory::NotAnalogous };
    case ColorStyleValue::ColorType::Lab:
    case ColorStyleValue::ColorType::OKLab:
        return { ComponentCategory::Lightness, ComponentCategory::OpponentA, ComponentCategory::OpponentB };
    case ColorStyleValue::ColorType::LCH:
    case ColorStyleValue::ColorType::OKLCH:
        return { ComponentCategory::Lightness, ComponentCategory::Colorfulness, ComponentCategory::Hue };
    case ColorStyleValue::ColorType::RGB:
    case ColorStyleValue::ColorType::A98RGB:
    case ColorStyleValue::ColorType::DisplayP3:
    case ColorStyleValue::ColorType::DisplayP3Linear:
    case ColorStyleValue::ColorType::sRGB:
    case ColorStyleValue::ColorType::sRGBLinear:
    case ColorStyleValue::ColorType::ProPhotoRGB:
    case ColorStyleValue::ColorType::Rec2020:
    case ColorStyleValue::ColorType::XYZD50:
    case ColorStyleValue::ColorType::XYZD65:
        return { ComponentCategory::Red, ComponentCategory::Green, ComponentCategory::Blue };
    default:
        return { ComponentCategory::NotAnalogous, ComponentCategory::NotAnalogous, ComponentCategory::NotAnalogous };
    }
}

// https://drafts.csswg.org/css-color-4/#interpolation-missing
// Carry forward missing components from the input color space to the interpolation color space.
// A missing component is carried forward if it has an analogous component in the target space.
// Additionally, if ALL components of an analogous set are missing, they are all carried forward.
static MissingComponents carry_forward_missing_components(
    MissingComponents const& source_missing,
    ComponentCategories const& source_categories,
    ComponentCategories const& target_categories)
{
    MissingComponents result;

    // Same-space: all components map to themselves, including NotAnalogous ones (e.g. HWB W/B).
    if (source_categories == target_categories) {
        for (size_t i = 0; i < 3; ++i)
            result.component(i) = source_missing.component(i);
        result.alpha = source_missing.alpha;
        return result;
    }

    // Carry forward individual analogous components
    for (size_t target_index = 0; target_index < 3; ++target_index) {
        if (target_categories.component(target_index) == ComponentCategory::NotAnalogous)
            continue;
        for (size_t source_index = 0; source_index < 3; ++source_index) {
            if (source_missing.component(source_index) && source_categories.component(source_index) == target_categories.component(target_index)) {
                result.component(target_index) = true;
                break;
            }
        }
    }

    // If every component of an analogous set is missing in the source, carry forward as a set.
    // The analogous set consists of the components that remain after removing individually analogous ones.
    bool all_non_analogous_missing = true;
    bool has_non_analogous = false;
    for (size_t i = 0; i < 3; ++i) {
        bool is_individually_analogous = false;
        for (size_t j = 0; j < 3; ++j) {
            if (source_categories.component(i) != ComponentCategory::NotAnalogous && source_categories.component(i) == target_categories.component(j)) {
                is_individually_analogous = true;
                break;
            }
        }
        if (!is_individually_analogous) {
            has_non_analogous = true;
            if (!source_missing.component(i))
                all_non_analogous_missing = false;
        }
    }
    if (has_non_analogous && all_non_analogous_missing) {
        for (size_t i = 0; i < 3; ++i) {
            bool is_individually_analogous = false;
            for (size_t j = 0; j < 3; ++j) {
                if (target_categories.component(i) != ComponentCategory::NotAnalogous && target_categories.component(i) == source_categories.component(j)) {
                    is_individually_analogous = true;
                    break;
                }
            }
            if (!is_individually_analogous)
                result.component(i) = true;
        }
    }

    // Alpha is always analogous to itself.
    result.alpha = source_missing.alpha;
    return result;
}

static ValueComparingNonnullRefPtr<StyleValue const> number_or_none(float value, bool is_missing)
{
    if (is_missing)
        return KeywordStyleValue::create(Keyword::None);
    return NumberStyleValue::create(value);
}

static ValueComparingNonnullRefPtr<StyleValue const> style_value_from_rectangular_color_space(Gfx::ColorComponents const& components, RectangularColorSpace space, MissingComponents const& missing = {})
{
    auto c1 = number_or_none(components[0], missing.component(0));
    auto c2 = number_or_none(components[1], missing.component(1));
    auto c3 = number_or_none(components[2], missing.component(2));
    auto alpha = number_or_none(components.alpha(), missing.alpha);

    switch (space) {
    case RectangularColorSpace::Lab:
        return LabLikeColorStyleValue::create<LabColorStyleValue>(c1, c2, c3, alpha);
    case RectangularColorSpace::Oklab:
        return LabLikeColorStyleValue::create<OKLabColorStyleValue>(c1, c2, c3, alpha);
    case RectangularColorSpace::Srgb:
        return ColorFunctionStyleValue::create("srgb"sv, c1, c2, c3, alpha);
    case RectangularColorSpace::SrgbLinear:
        return ColorFunctionStyleValue::create("srgb-linear"sv, c1, c2, c3, alpha);
    case RectangularColorSpace::DisplayP3:
        return ColorFunctionStyleValue::create("display-p3"sv, c1, c2, c3, alpha);
    case RectangularColorSpace::DisplayP3Linear:
        return ColorFunctionStyleValue::create("display-p3-linear"sv, c1, c2, c3, alpha);
    case RectangularColorSpace::A98Rgb:
        return ColorFunctionStyleValue::create("a98-rgb"sv, c1, c2, c3, alpha);
    case RectangularColorSpace::ProphotoRgb:
        return ColorFunctionStyleValue::create("prophoto-rgb"sv, c1, c2, c3, alpha);
    case RectangularColorSpace::Rec2020:
        return ColorFunctionStyleValue::create("rec2020"sv, c1, c2, c3, alpha);
    case RectangularColorSpace::Xyz:
    case RectangularColorSpace::XyzD65:
        return ColorFunctionStyleValue::create("xyz-d65"sv, c1, c2, c3, alpha);
    case RectangularColorSpace::XyzD50:
        return ColorFunctionStyleValue::create("xyz-d50"sv, c1, c2, c3, alpha);
    }
    VERIFY_NOT_REACHED();
}

static ValueComparingNonnullRefPtr<StyleValue const> style_value_from_polar_color_space(Gfx::ColorComponents const& components, PolarColorSpace space, MissingComponents const& missing = {})
{
    auto alpha = number_or_none(components.alpha(), missing.alpha);

    switch (space) {
    case PolarColorSpace::Hsl: {
        // HSL/HWB resolve to sRGB in computed values, so convert and express as color(srgb ...).
        auto srgb = Gfx::hsl_to_srgb(components);
        return ColorFunctionStyleValue::create("srgb"sv,
            NumberStyleValue::create(srgb[0]),
            NumberStyleValue::create(srgb[1]),
            NumberStyleValue::create(srgb[2]),
            alpha);
    }
    case PolarColorSpace::Hwb: {
        auto srgb = Gfx::hwb_to_srgb(components);
        return ColorFunctionStyleValue::create("srgb"sv,
            NumberStyleValue::create(srgb[0]),
            NumberStyleValue::create(srgb[1]),
            NumberStyleValue::create(srgb[2]),
            alpha);
    }
    case PolarColorSpace::Lch:
        return LCHLikeColorStyleValue::create<LCHColorStyleValue>(
            number_or_none(components[0], missing.component(0)),
            number_or_none(components[1], missing.component(1)),
            number_or_none(components[2], missing.component(2)),
            alpha);
    case PolarColorSpace::Oklch:
        return LCHLikeColorStyleValue::create<OKLCHColorStyleValue>(
            number_or_none(components[0], missing.component(0)),
            number_or_none(components[1], missing.component(1)),
            number_or_none(components[2], missing.component(2)),
            alpha);
    }
    VERIFY_NOT_REACHED();
}

static Optional<Gfx::ColorComponents> style_value_to_color_components(StyleValue const& style_value, CalculationResolutionContext const& context)
{
    if (!style_value.is_color())
        return {};

    auto const& color = style_value.as_color();
    auto resolve_alpha = [&](StyleValue const& alpha_sv) -> Optional<float> {
        auto result = ColorStyleValue::resolve_alpha(alpha_sv, context);
        if (!result.has_value())
            return {};
        return static_cast<float>(result.value());
    };

    switch (color.color_type()) {
    case ColorStyleValue::ColorType::HSL: {
        auto const& hsl = as<HSLColorStyleValue>(color);
        auto h = ColorStyleValue::resolve_hue(hsl.h(), context);
        auto s = ColorStyleValue::resolve_with_reference_value(hsl.s(), 100.0f, context);
        auto l = ColorStyleValue::resolve_with_reference_value(hsl.l(), 100.0f, context);
        auto a = resolve_alpha(hsl.alpha());
        if (!h.has_value() || !s.has_value() || !l.has_value() || !a.has_value())
            return {};
        // ColorConversion expects S and L as fractions (0-1), not percentages
        return Gfx::ColorComponents { static_cast<float>(h.value()), static_cast<float>(s.value() / 100.0), static_cast<float>(l.value() / 100.0), a.value() };
    }
    case ColorStyleValue::ColorType::HWB: {
        auto const& hwb = as<HWBColorStyleValue>(color);
        auto h = ColorStyleValue::resolve_hue(hwb.h(), context);
        auto w = ColorStyleValue::resolve_with_reference_value(hwb.w(), 100.0f, context);
        auto b = ColorStyleValue::resolve_with_reference_value(hwb.b(), 100.0f, context);
        auto a = resolve_alpha(hwb.alpha());
        if (!h.has_value() || !w.has_value() || !b.has_value() || !a.has_value())
            return {};
        return Gfx::ColorComponents { static_cast<float>(h.value()), static_cast<float>(w.value() / 100.0), static_cast<float>(b.value() / 100.0), a.value() };
    }
    case ColorStyleValue::ColorType::Lab: {
        auto const& lab = as<LabColorStyleValue>(color);
        auto l = ColorStyleValue::resolve_with_reference_value(lab.l(), 100.0f, context);
        auto a_comp = ColorStyleValue::resolve_with_reference_value(lab.a(), 125.0f, context);
        auto b_comp = ColorStyleValue::resolve_with_reference_value(lab.b(), 125.0f, context);
        auto a = resolve_alpha(lab.alpha());
        if (!l.has_value() || !a_comp.has_value() || !b_comp.has_value() || !a.has_value())
            return {};
        return Gfx::ColorComponents { static_cast<float>(l.value()), static_cast<float>(a_comp.value()), static_cast<float>(b_comp.value()), a.value() };
    }
    case ColorStyleValue::ColorType::OKLab: {
        auto const& oklab = as<OKLabColorStyleValue>(color);
        auto l = ColorStyleValue::resolve_with_reference_value(oklab.l(), 1.0f, context);
        auto a_comp = ColorStyleValue::resolve_with_reference_value(oklab.a(), 0.4f, context);
        auto b_comp = ColorStyleValue::resolve_with_reference_value(oklab.b(), 0.4f, context);
        auto a = resolve_alpha(oklab.alpha());
        if (!l.has_value() || !a_comp.has_value() || !b_comp.has_value() || !a.has_value())
            return {};
        return Gfx::ColorComponents { static_cast<float>(l.value()), static_cast<float>(a_comp.value()), static_cast<float>(b_comp.value()), a.value() };
    }
    case ColorStyleValue::ColorType::LCH: {
        auto const& lch = as<LCHColorStyleValue>(color);
        auto l = ColorStyleValue::resolve_with_reference_value(lch.l(), 100.0f, context);
        auto c = ColorStyleValue::resolve_with_reference_value(lch.c(), 150.0f, context);
        auto h = ColorStyleValue::resolve_hue(lch.h(), context);
        auto a = resolve_alpha(lch.alpha());
        if (!l.has_value() || !c.has_value() || !h.has_value() || !a.has_value())
            return {};
        return Gfx::ColorComponents { static_cast<float>(l.value()), static_cast<float>(c.value()), static_cast<float>(h.value()), a.value() };
    }
    case ColorStyleValue::ColorType::OKLCH: {
        auto const& oklch = as<OKLCHColorStyleValue>(color);
        auto l = ColorStyleValue::resolve_with_reference_value(oklch.l(), 1.0f, context);
        auto c = ColorStyleValue::resolve_with_reference_value(oklch.c(), 0.4f, context);
        auto h = ColorStyleValue::resolve_hue(oklch.h(), context);
        auto a = resolve_alpha(oklch.alpha());
        if (!l.has_value() || !c.has_value() || !h.has_value() || !a.has_value())
            return {};
        return Gfx::ColorComponents { static_cast<float>(l.value()), static_cast<float>(c.value()), static_cast<float>(h.value()), a.value() };
    }
    case ColorStyleValue::ColorType::RGB: {
        auto const& rgb = as<RGBColorStyleValue>(color);
        auto r = ColorStyleValue::resolve_with_reference_value(rgb.r(), 255.0f, context);
        auto g = ColorStyleValue::resolve_with_reference_value(rgb.g(), 255.0f, context);
        auto b = ColorStyleValue::resolve_with_reference_value(rgb.b(), 255.0f, context);
        auto a = resolve_alpha(rgb.alpha());
        if (!r.has_value() || !g.has_value() || !b.has_value() || !a.has_value())
            return {};
        // rgb() computed values clamp channels to [0, 255] before normalizing to [0, 1].
        return Gfx::ColorComponents {
            static_cast<float>(clamp(r.value(), 0.0, 255.0) / 255.0),
            static_cast<float>(clamp(g.value(), 0.0, 255.0) / 255.0),
            static_cast<float>(clamp(b.value(), 0.0, 255.0) / 255.0),
            a.value(),
        };
    }
    default:
        if (color.is_color_function()) {
            auto const& func = as<ColorFunctionStyleValue>(color);
            auto c1 = ColorStyleValue::resolve_with_reference_value(func.channel(0), 1.0f, context);
            auto c2 = ColorStyleValue::resolve_with_reference_value(func.channel(1), 1.0f, context);
            auto c3 = ColorStyleValue::resolve_with_reference_value(func.channel(2), 1.0f, context);
            auto a = resolve_alpha(func.alpha());
            if (!c1.has_value() || !c2.has_value() || !c3.has_value() || !a.has_value())
                return {};
            return Gfx::ColorComponents { static_cast<float>(c1.value()), static_cast<float>(c2.value()), static_cast<float>(c3.value()), a.value() };
        }
        return {};
    }
}

static Gfx::ColorComponents native_components_to_srgb(Gfx::ColorComponents native, ColorStyleValue::ColorType source_type)
{
    switch (source_type) {
    case ColorStyleValue::ColorType::RGB:
    case ColorStyleValue::ColorType::sRGB:
        return native;
    case ColorStyleValue::ColorType::sRGBLinear:
        return Gfx::linear_srgb_to_srgb(native);
    case ColorStyleValue::ColorType::HSL:
        return Gfx::hsl_to_srgb(native);
    case ColorStyleValue::ColorType::HWB:
        return Gfx::hwb_to_srgb(native);
    case ColorStyleValue::ColorType::Lab: {
        auto xyz50 = Gfx::lab_to_xyz50(native);
        auto xyz65 = Gfx::xyz50_to_xyz65(xyz50);
        return Gfx::linear_srgb_to_srgb(Gfx::xyz65_to_linear_srgb(xyz65));
    }
    case ColorStyleValue::ColorType::OKLab: {
        auto xyz65 = Gfx::oklab_to_xyz65(native);
        return Gfx::linear_srgb_to_srgb(Gfx::xyz65_to_linear_srgb(xyz65));
    }
    case ColorStyleValue::ColorType::LCH: {
        auto lab = Gfx::lch_to_lab(native);
        auto xyz50 = Gfx::lab_to_xyz50(lab);
        auto xyz65 = Gfx::xyz50_to_xyz65(xyz50);
        return Gfx::linear_srgb_to_srgb(Gfx::xyz65_to_linear_srgb(xyz65));
    }
    case ColorStyleValue::ColorType::OKLCH: {
        auto oklab = Gfx::oklch_to_oklab(native);
        auto xyz65 = Gfx::oklab_to_xyz65(oklab);
        return Gfx::linear_srgb_to_srgb(Gfx::xyz65_to_linear_srgb(xyz65));
    }
    case ColorStyleValue::ColorType::DisplayP3: {
        auto linear_p3 = Gfx::display_p3_to_linear_display_p3(native);
        auto xyz65 = Gfx::linear_display_p3_to_xyz65(linear_p3);
        return Gfx::linear_srgb_to_srgb(Gfx::xyz65_to_linear_srgb(xyz65));
    }
    case ColorStyleValue::ColorType::DisplayP3Linear: {
        auto xyz65 = Gfx::linear_display_p3_to_xyz65(native);
        return Gfx::linear_srgb_to_srgb(Gfx::xyz65_to_linear_srgb(xyz65));
    }
    case ColorStyleValue::ColorType::A98RGB: {
        auto linear_a98 = Gfx::a98_rgb_to_linear_a98_rgb(native);
        auto xyz65 = Gfx::linear_a98_rgb_to_xyz65(linear_a98);
        return Gfx::linear_srgb_to_srgb(Gfx::xyz65_to_linear_srgb(xyz65));
    }
    case ColorStyleValue::ColorType::ProPhotoRGB: {
        auto linear_prophoto = Gfx::prophoto_rgb_to_linear_prophoto_rgb(native);
        auto xyz50 = Gfx::linear_prophoto_rgb_to_xyz50(linear_prophoto);
        auto xyz65 = Gfx::xyz50_to_xyz65(xyz50);
        return Gfx::linear_srgb_to_srgb(Gfx::xyz65_to_linear_srgb(xyz65));
    }
    case ColorStyleValue::ColorType::Rec2020: {
        auto linear_rec2020 = Gfx::rec2020_to_linear_rec2020(native);
        auto xyz65 = Gfx::linear_rec2020_to_xyz65(linear_rec2020);
        return Gfx::linear_srgb_to_srgb(Gfx::xyz65_to_linear_srgb(xyz65));
    }
    case ColorStyleValue::ColorType::XYZD50: {
        auto xyz65 = Gfx::xyz50_to_xyz65(native);
        return Gfx::linear_srgb_to_srgb(Gfx::xyz65_to_linear_srgb(xyz65));
    }
    case ColorStyleValue::ColorType::XYZD65:
        return Gfx::linear_srgb_to_srgb(Gfx::xyz65_to_linear_srgb(native));
    default:
        VERIFY_NOT_REACHED();
    }
}

static bool color_type_matches_rectangular_space(ColorStyleValue::ColorType source_type, RectangularColorSpace space)
{
    switch (space) {
    case RectangularColorSpace::Srgb:
        return source_type == ColorStyleValue::ColorType::sRGB || source_type == ColorStyleValue::ColorType::RGB;
    case RectangularColorSpace::SrgbLinear:
        return source_type == ColorStyleValue::ColorType::sRGBLinear;
    case RectangularColorSpace::DisplayP3:
        return source_type == ColorStyleValue::ColorType::DisplayP3;
    case RectangularColorSpace::DisplayP3Linear:
        return source_type == ColorStyleValue::ColorType::DisplayP3Linear;
    case RectangularColorSpace::A98Rgb:
        return source_type == ColorStyleValue::ColorType::A98RGB;
    case RectangularColorSpace::ProphotoRgb:
        return source_type == ColorStyleValue::ColorType::ProPhotoRGB;
    case RectangularColorSpace::Rec2020:
        return source_type == ColorStyleValue::ColorType::Rec2020;
    case RectangularColorSpace::Lab:
        return source_type == ColorStyleValue::ColorType::Lab;
    case RectangularColorSpace::Oklab:
        return source_type == ColorStyleValue::ColorType::OKLab;
    case RectangularColorSpace::Xyz:
    case RectangularColorSpace::XyzD65:
        return source_type == ColorStyleValue::ColorType::XYZD65;
    case RectangularColorSpace::XyzD50:
        return source_type == ColorStyleValue::ColorType::XYZD50;
    }
    VERIFY_NOT_REACHED();
}

static bool color_type_matches_polar_space(ColorStyleValue::ColorType source_type, PolarColorSpace space)
{
    switch (space) {
    case PolarColorSpace::Hsl:
        return source_type == ColorStyleValue::ColorType::HSL;
    case PolarColorSpace::Hwb:
        return source_type == ColorStyleValue::ColorType::HWB;
    case PolarColorSpace::Lch:
        return source_type == ColorStyleValue::ColorType::LCH;
    case PolarColorSpace::Oklch:
        return source_type == ColorStyleValue::ColorType::OKLCH;
    }
    VERIFY_NOT_REACHED();
}

// https://drafts.csswg.org/css-color-4/#powerless
static void mark_powerless_for_zero_alpha(bool has_native_components, float alpha, MissingComponents& missing)
{
    if (has_native_components)
        return;
    if (!missing.alpha && alpha == 0.0f) {
        missing.component(0) = true;
        missing.component(1) = true;
        missing.component(2) = true;
    }
}

// NB: Achromatic colors converted through the sRGB -> XYZ-D65 ->  XYZ-D50 -> Lab -> LCH chain accumulate
//     floating-point error of ~0.016 in the chroma component due to the Bradford chromatic adaptation matrices.
//     This is the worst case for all color conversion types, so the threshold is large enough to account for this.
static constexpr float achromatic_threshold = 0.02f;

static void mark_powerless_hue_after_conversion(
    Gfx::ColorComponents const& components, MissingComponents& interp_missing,
    StyleValue const& style_value, PolarColorSpace polar_color_space,
    ComponentCategories const& target_categories)
{
    if (style_value.is_color() && color_type_matches_polar_space(style_value.as_color().color_type(), polar_color_space))
        return;

    bool has_zero_colorfulness = false;
    for (size_t i = 0; i < 3; ++i) {
        if (target_categories.component(i) == ComponentCategory::Colorfulness && fabsf(components[i]) < achromatic_threshold)
            has_zero_colorfulness = true;
    }
    if (polar_color_space == PolarColorSpace::Hwb
        && components[1] + components[2] >= 1.0f - achromatic_threshold)
        has_zero_colorfulness = true;

    if (has_zero_colorfulness) {
        for (size_t i = 0; i < 3; ++i) {
            if (target_categories.component(i) == ComponentCategory::Hue)
                interp_missing.component(i) = true;
        }
    }
}

static void substitute_missing_components(
    Gfx::ColorComponents& from_components, Gfx::ColorComponents& to_components,
    MissingComponents const& from_missing, MissingComponents const& to_missing)
{
    for (size_t i = 0; i < 3; ++i) {
        if (from_missing.component(i) && !to_missing.component(i))
            from_components[i] = to_components[i];
        else if (to_missing.component(i) && !from_missing.component(i))
            to_components[i] = from_components[i];
    }

    if (from_missing.alpha && !to_missing.alpha)
        from_components.set_alpha(to_components.alpha());
    else if (to_missing.alpha && !from_missing.alpha)
        to_components.set_alpha(from_components.alpha());
    else if (from_missing.alpha && to_missing.alpha) {
        from_components.set_alpha(1.0f);
        to_components.set_alpha(1.0f);
    }
}

using ColorInterpolationMethod = ColorInterpolationMethodStyleValue::ColorInterpolationMethod;

struct PreparedInterpolationColor {
    MissingComponents missing_components;
    ComponentCategories source_categories;
    Optional<Gfx::ColorComponents> native_components;
    Optional<Gfx::ColorComponents> srgb_components;
};

static ColorSyntax color_syntax_for_interpolation(StyleValue const& style_value)
{
    if (style_value.is_keyword())
        return ColorSyntax::Legacy;

    auto const& color = style_value.as_color();
    switch (color.color_type()) {
    case ColorStyleValue::ColorType::RGB:
    case ColorStyleValue::ColorType::HSL:
    case ColorStyleValue::ColorType::HWB:
        return ColorSyntax::Legacy;
    default:
        return color.color_syntax();
    }
}

static ComponentCategories source_categories_for_interpolation(StyleValue const& style_value)
{
    if (style_value.is_color())
        return categories_for_color_type(style_value.as_color().color_type());
    return { ComponentCategory::Red, ComponentCategory::Green, ComponentCategory::Blue };
}

static InterpolationPolicy resolve_interpolation_policy(
    StyleValue const& from,
    StyleValue const& to,
    Optional<ColorInterpolationMethod> color_interpolation_method)
{
    // https://drafts.csswg.org/css-color-4/#interpolation-space
    // If the host syntax does not define what color space interpolation should take place in, it defaults to Oklab.
    // However, user agents must handle interpolation between legacy sRGB color formats (hex colors, named colors,
    // rgb(), hsl() or hwb() and the equivalent alpha-including forms) in gamma-encoded sRGB space.
    auto color_syntax = ColorSyntax::Legacy;
    if (color_syntax_for_interpolation(from) == ColorSyntax::Modern
        || color_syntax_for_interpolation(to) == ColorSyntax::Modern) {
        color_syntax = ColorSyntax::Modern;
    }

    // NB: When no explicit method is provided, derive from the color syntax.
    //     When an explicit method IS provided (e.g. color-mix), always use modern output format.
    return {
        .use_legacy_output = !color_interpolation_method.has_value() && color_syntax == ColorSyntax::Legacy,
        .color_interpolation_method = color_interpolation_method.value_or(
            ColorInterpolationMethodStyleValue::default_color_interpolation_method(color_syntax)),
    };
}

static PreparedInterpolationColor initialize_interpolation_color(
    StyleValue const& style_value,
    ColorResolutionContext const& color_resolution_context)
{
    return {
        .missing_components = extract_missing_components(style_value),
        .source_categories = source_categories_for_interpolation(style_value),
        .native_components = style_value_to_color_components(
            style_value,
            color_resolution_context.calculation_resolution_context),
        .srgb_components = {},
    };
}

static Optional<Gfx::ColorComponents> resolve_interpolation_color_to_srgb(
    StyleValue const& style_value,
    PreparedInterpolationColor& color,
    ColorResolutionContext const& color_resolution_context)
{
    if (color.srgb_components.has_value())
        return color.srgb_components;

    if (color.native_components.has_value()) {
        color.srgb_components = native_components_to_srgb(
            color.native_components.value(),
            style_value.as_color().color_type());
        return color.srgb_components;
    }

    auto resolved = style_value.to_color(color_resolution_context);
    if (!resolved.has_value())
        return {};

    color.srgb_components = Gfx::color_to_srgb(resolved.value());
    return color.srgb_components;
}

static Optional<float> resolve_interpolation_color_alpha(
    StyleValue const& style_value,
    PreparedInterpolationColor& color,
    ColorResolutionContext const& color_resolution_context)
{
    if (color.native_components.has_value())
        return color.native_components->alpha();

    auto srgb = resolve_interpolation_color_to_srgb(style_value, color, color_resolution_context);
    if (!srgb.has_value())
        return {};
    return srgb->alpha();
}

static bool prepare_interpolation_color_for_conversion(
    StyleValue const& style_value,
    PreparedInterpolationColor& color,
    ColorResolutionContext const& color_resolution_context)
{
    auto alpha = resolve_interpolation_color_alpha(style_value, color, color_resolution_context);
    if (!alpha.has_value())
        return false;

    // https://drafts.csswg.org/css-color-4/#powerless
    // NB: When a color has zero alpha, all color components are powerless and we mark them all as missing.
    //     However, if the alpha is itself `none`, it resolves to 0 but is not truly zero - it will be substituted
    //     with the other color's alpha during interpolation.
    mark_powerless_for_zero_alpha(color.native_components.has_value(), alpha.value(), color.missing_components);
    return true;
}

static Gfx::ColorComponents convert_interpolation_color_to_rectangular_space(
    StyleValue const& style_value,
    PreparedInterpolationColor& color,
    RectangularColorSpace space,
    ColorResolutionContext const& color_resolution_context)
{
    if (color.native_components.has_value() && style_value.is_color()
        && color_type_matches_rectangular_space(style_value.as_color().color_type(), space)) {
        return color.native_components.value();
    }

    auto srgb = resolve_interpolation_color_to_srgb(style_value, color, color_resolution_context);
    VERIFY(srgb.has_value());
    return srgb_to_rectangular_color_space(srgb.value(), space);
}

static Gfx::ColorComponents convert_interpolation_color_to_polar_space(
    StyleValue const& style_value,
    PreparedInterpolationColor& color,
    PolarColorSpace space,
    ColorResolutionContext const& color_resolution_context)
{
    if (color.native_components.has_value() && style_value.is_color()
        && color_type_matches_polar_space(style_value.as_color().color_type(), space)) {
        return color.native_components.value();
    }

    auto srgb = resolve_interpolation_color_to_srgb(style_value, color, color_resolution_context);
    VERIFY(srgb.has_value());
    return srgb_to_polar_color_space(srgb.value(), space);
}

static size_t hue_index_for_color_space(PolarColorSpace space)
{
    switch (space) {
    case PolarColorSpace::Hsl:
    case PolarColorSpace::Hwb:
        return 0;
    case PolarColorSpace::Lch:
    case PolarColorSpace::Oklch:
        return 2;
    }
    VERIFY_NOT_REACHED();
}

static InterpolationSpaceState convert_to_interpolation_space(
    StyleValue const& from,
    PreparedInterpolationColor& from_color,
    StyleValue const& to,
    PreparedInterpolationColor& to_color,
    ColorInterpolationMethod const& color_interpolation_method,
    ColorResolutionContext const& color_resolution_context)
{
    InterpolationSpaceState state;

    color_interpolation_method.visit(
        [&](RectangularColorSpace space) {
            state.rectangular_color_space = space;
            state.from_components = convert_interpolation_color_to_rectangular_space(
                from, from_color, space, color_resolution_context);
            state.to_components = convert_interpolation_color_to_rectangular_space(
                to, to_color, space, color_resolution_context);

            auto target_categories = categories_for_rectangular_space(space);
            state.from_missing = carry_forward_missing_components(
                from_color.missing_components, from_color.source_categories, target_categories);
            state.to_missing = carry_forward_missing_components(
                to_color.missing_components, to_color.source_categories, target_categories);
        },
        [&](ColorInterpolationMethodStyleValue::PolarColorInterpolationMethod const& polar_color_interpolation_method) {
            state.is_polar = true;
            state.polar_color_space = polar_color_interpolation_method.color_space;
            state.hue_interpolation_method = polar_color_interpolation_method.hue_interpolation_method;
            state.hue_index = hue_index_for_color_space(polar_color_interpolation_method.color_space);
            state.from_components = convert_interpolation_color_to_polar_space(
                from, from_color, polar_color_interpolation_method.color_space, color_resolution_context);
            state.to_components = convert_interpolation_color_to_polar_space(
                to, to_color, polar_color_interpolation_method.color_space, color_resolution_context);

            auto target_categories = categories_for_polar_space(polar_color_interpolation_method.color_space);
            state.from_missing = carry_forward_missing_components(
                from_color.missing_components, from_color.source_categories, target_categories);
            state.to_missing = carry_forward_missing_components(
                to_color.missing_components, to_color.source_categories, target_categories);
            state.polar_target_categories = target_categories;
        });

    if (state.is_polar) {
        mark_powerless_hue_after_conversion(
            state.from_components, state.from_missing, from, state.polar_color_space, state.polar_target_categories);
        mark_powerless_hue_after_conversion(
            state.to_components, state.to_missing, to, state.polar_color_space, state.polar_target_categories);
    }

    return state;
}

static bool reinsert_carried_forward_values(InterpolationSpaceState& state)
{
    bool both_alpha_missing = state.from_missing.alpha && state.to_missing.alpha;
    substitute_missing_components(state.from_components, state.to_components, state.from_missing, state.to_missing);
    return both_alpha_missing;
}

static void fixup_hues_if_required(InterpolationSpaceState& state)
{
    if (!state.is_polar)
        return;
    fixup_hue(state.from_components[state.hue_index], state.to_components[state.hue_index], state.hue_interpolation_method);
}

static Gfx::ColorComponents premultiply_color_components(
    Gfx::ColorComponents const& components,
    bool is_polar,
    size_t hue_index)
{
    Gfx::ColorComponents premultiplied;
    premultiplied.set_alpha(components.alpha());

    for (size_t i = 0; i < 3; ++i) {
        if (is_polar && i == hue_index)
            premultiplied[i] = components[i];
        else
            premultiplied[i] = components[i] * components.alpha();
    }

    return premultiplied;
}

static Gfx::ColorComponents interpolate_premultiplied_components(
    Gfx::ColorComponents const& from,
    Gfx::ColorComponents const& to,
    float delta)
{
    Gfx::ColorComponents interpolated;
    for (size_t i = 0; i < 3; ++i)
        interpolated[i] = interpolate_color_component(from[i], to[i], delta);
    return interpolated;
}

static Gfx::ColorComponents unpremultiply_color_components(
    Gfx::ColorComponents const& premultiplied,
    float interpolated_alpha,
    bool is_polar,
    size_t hue_index)
{
    Gfx::ColorComponents result;
    result.set_alpha(interpolated_alpha);

    for (size_t i = 0; i < 3; ++i) {
        bool was_premultiplied = !is_polar || i != hue_index;
        result[i] = was_premultiplied ? premultiplied[i] / interpolated_alpha : premultiplied[i];
    }

    return result;
}

static MissingComponents result_missing_components(InterpolationSpaceState const& state)
{
    // https://drafts.csswg.org/css-color-4/#interpolation-missing
    // NB: If both input colors have a component as missing, the result also has that component as missing.
    return {
        state.from_missing.component(0) && state.to_missing.component(0),
        state.from_missing.component(1) && state.to_missing.component(1),
        state.from_missing.component(2) && state.to_missing.component(2),
        state.from_missing.alpha && state.to_missing.alpha,
    };
}

RefPtr<StyleValue const> style_value_for_interpolated_color(InterpolatedColor const& interpolated)
{
    // https://drafts.csswg.org/css-color-4/#interpolation-space
    // NB: Legacy sRGB content interpolates in sRGB and produces a legacy rgb() result.
    if (interpolated.policy.use_legacy_output)
        return ColorStyleValue::create_from_color(Gfx::srgb_to_color(interpolated.components), ColorSyntax::Legacy);

    // NB: Return as a StyleValue in the interpolation color space.
    if (interpolated.state.is_polar)
        return style_value_from_polar_color_space(interpolated.components, interpolated.state.polar_color_space, interpolated.missing);
    return style_value_from_rectangular_color_space(interpolated.components, interpolated.state.rectangular_color_space, interpolated.missing);
}

// https://drafts.csswg.org/css-color-4/#interpolation
Optional<InterpolatedColor> perform_color_interpolation(
    StyleValue const& from, StyleValue const& to, float delta,
    Optional<ColorInterpolationMethod> color_interpolation_method,
    ColorResolutionContext const& color_resolution_context)
{
    // 1. checking the two colors for analogous components and analogous sets which will be carried forward
    auto from_color = initialize_interpolation_color(from, color_resolution_context);
    auto to_color = initialize_interpolation_color(to, color_resolution_context);

    // 2. prepare both colors for conversion. this changes any powerless components to missing values
    if (!prepare_interpolation_color_for_conversion(from, from_color, color_resolution_context)
        || !prepare_interpolation_color_for_conversion(to, to_color, color_resolution_context)) {
        return {};
    }

    // 3. converting them both to a given color space which will be referred to as the interpolation color space
    //    below.
    auto interpolation_policy = resolve_interpolation_policy(from, to, color_interpolation_method);
    auto state = convert_to_interpolation_space(
        from, from_color, to, to_color, interpolation_policy.color_interpolation_method, color_resolution_context);

    // 4. (if required) re-inserting carried forward values in the converted colors
    auto both_alpha_missing = reinsert_carried_forward_values(state);

    // 5. (if required) fixing up the hues, depending on the selected <hue-interpolation-method>
    fixup_hues_if_required(state);

    auto interpolated_alpha = interpolate_color_component(state.from_components.alpha(), state.to_components.alpha(), delta);
    auto clamped_alpha = clamp(interpolated_alpha, 0.0f, 1.0f);
    if (clamped_alpha == 0.0f && !both_alpha_missing) {
        // OPTIMIZATION: Fully transparent results can skip the premultiply/interpolate/unpremultiply cycle.
        Gfx::ColorComponents zero_result { 0.0f, 0.0f, 0.0f, 0.0f };
        return InterpolatedColor { zero_result, {}, interpolation_policy, move(state) };
    }

    // 6. changing the color components to premultiplied form
    // https://drafts.csswg.org/css-color-4/#interpolation-alpha
    // For rectangular orthogonal color coordinate systems, all component values are multiplied by the alpha value.
    // For cylindrical polar color coordinate systems, the hue angle is NOT premultiplied.
    auto from_premultiplied = premultiply_color_components(state.from_components, state.is_polar, state.hue_index);
    auto to_premultiplied = premultiply_color_components(state.to_components, state.is_polar, state.hue_index);

    // 7. linearly interpolating each component of the computed value of the color separately
    auto interpolated_components = interpolate_premultiplied_components(from_premultiplied, to_premultiplied, delta);

    // 8. undoing premultiplication
    auto result = unpremultiply_color_components(interpolated_components, clamped_alpha, state.is_polar, state.hue_index);

    auto missing = result_missing_components(state);
    return InterpolatedColor { result, missing, interpolation_policy, move(state) };
}

// https://drafts.csswg.org/css-color-4/#interpolation
RefPtr<StyleValue const> interpolate_color(
    StyleValue const& from, StyleValue const& to, float delta,
    Optional<ColorInterpolationMethod> color_interpolation_method,
    ColorResolutionContext const& color_resolution_context)
{
    auto interpolated = perform_color_interpolation(from, to, delta, color_interpolation_method, color_resolution_context);
    if (!interpolated.has_value())
        return {};
    return style_value_for_interpolated_color(*interpolated);
}

}
