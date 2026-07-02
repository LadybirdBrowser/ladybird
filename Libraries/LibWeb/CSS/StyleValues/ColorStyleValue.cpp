/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ColorStyleValue.h"
#include <LibGfx/ColorConversion.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<ColorStyleValue const> ColorStyleValue::create_from_color(Color color, ColorSyntax color_syntax, Optional<FlyString> name)
{
    return ColorFunctionStyleValue::create(
        ColorType::RGB,
        NumberStyleValue::create(color.red()),
        NumberStyleValue::create(color.green()),
        NumberStyleValue::create(color.blue()),
        NumberStyleValue::create(color.alpha() / 255.0),
        color_syntax,
        name);
}

Optional<double> ColorStyleValue::resolve_hue(StyleValue const& style_value, CalculationResolutionContext const& resolution_context)
{
    // <number> | <angle> | none
    auto normalized = [](double number) {
        // +inf should be clamped to 360
        if (!isfinite(number) && number > 0)
            number = 360.0;

        // -inf and NaN should be clamped to 0
        if (!isfinite(number) || isnan(number))
            number = 0.0;

        return JS::modulo(number, 360.0);
    };

    if (style_value.is_number())
        return normalized(style_value.as_number().number());

    if (style_value.is_angle())
        return normalized(style_value.as_angle().angle().to_degrees());

    if (style_value.is_keyword()) {
        if (auto channel = keyword_to_channel_keyword(style_value.to_keyword()); channel.has_value()) {
            if (!resolution_context.relative_color.has_value())
                return {};
            auto resolved = resolution_context.relative_color->get(*channel);
            if (!resolved.has_value())
                return {};
            return normalized(resolved.value());
        }
    }

    if (style_value.is_calculated()) {
        if (style_value.as_calculated().resolves_to_number()) {
            auto maybe_number = style_value.as_calculated().resolve_number(resolution_context);

            if (!maybe_number.has_value())
                return {};

            return normalized(maybe_number.value());
        }

        if (style_value.as_calculated().resolves_to_angle()) {
            auto maybe_angle = style_value.as_calculated().resolve_angle(resolution_context);

            if (!maybe_angle.has_value())
                return {};

            return normalized(maybe_angle.value().to_degrees());
        }
    }

    return 0;
}

Optional<double> ColorStyleValue::resolve_with_reference_value(StyleValue const& style_value, float one_hundred_percent_value, CalculationResolutionContext const& resolution_context)
{
    // <percentage> | <number> | none
    auto normalize_percentage = [one_hundred_percent_value](Percentage const& percentage) {
        return percentage.as_fraction() * one_hundred_percent_value;
    };

    if (style_value.is_percentage())
        return normalize_percentage(style_value.as_percentage().percentage());

    if (style_value.is_number())
        return style_value.as_number().number();

    if (style_value.is_keyword()) {
        if (auto channel = keyword_to_channel_keyword(style_value.to_keyword()); channel.has_value()) {
            if (!resolution_context.relative_color.has_value())
                return {};
            return resolution_context.relative_color->get(*channel);
        }
    }

    if (style_value.is_calculated()) {
        auto const& calculated = style_value.as_calculated();
        if (calculated.resolves_to_number()) {
            auto maybe_number = calculated.resolve_number(resolution_context);

            if (!maybe_number.has_value())
                return {};

            return maybe_number.value();
        }

        if (calculated.resolves_to_percentage()) {
            auto percentage = calculated.resolve_percentage(resolution_context);

            if (!percentage.has_value())
                return {};

            return normalize_percentage(percentage.value());
        }
    }

    return 0;
}

Optional<double> ColorStyleValue::resolve_alpha(StyleValue const& style_value, CalculationResolutionContext const& resolution_context)
{
    // <number> | <percentage> | none
    auto normalized = [](double number) {
        if (isnan(number))
            number = 0;
        return clamp(number, 0.0, 1.0);
    };

    if (style_value.is_number())
        return normalized(style_value.as_number().number());

    if (style_value.is_percentage())
        return normalized(style_value.as_percentage().percentage().as_fraction());

    if (style_value.is_keyword()) {
        if (auto channel = keyword_to_channel_keyword(style_value.to_keyword()); channel.has_value()) {
            if (!resolution_context.relative_color.has_value())
                return {};
            auto resolved = resolution_context.relative_color->get(*channel);
            if (!resolved.has_value())
                return {};
            return normalized(resolved.value());
        }
    }

    if (style_value.is_calculated()) {
        auto const& calculated = style_value.as_calculated();
        if (calculated.resolves_to_number()) {
            auto maybe_number = calculated.resolve_number(resolution_context);

            if (!maybe_number.has_value())
                return {};

            return normalized(maybe_number.value());
        }

        if (calculated.resolves_to_percentage()) {
            auto percentage = calculated.resolve_percentage(resolution_context);

            if (!percentage.has_value())
                return {};

            return normalized(percentage.value().as_fraction());
        }
    }

    if (style_value.is_keyword() && style_value.to_keyword() == Keyword::None)
        return 0;

    return 1;
}

namespace {

Gfx::ColorComponents native_channels_to_xyz65(Gfx::ColorComponents const& native, ColorStyleValue::ColorType native_type)
{
    using ColorType = ColorStyleValue::ColorType;
    switch (native_type) {
    case ColorType::RGB: {
        Gfx::ColorComponents const srgb_fraction { native[0] / 255.0f, native[1] / 255.0f, native[2] / 255.0f, native.alpha() };
        return Gfx::linear_srgb_to_xyz65(Gfx::srgb_to_linear_srgb(srgb_fraction));
    }
    case ColorType::HSL: {
        Gfx::ColorComponents const hsl { native[0], native[1] / 100.0f, native[2] / 100.0f, native.alpha() };
        auto srgb_fraction = Gfx::hsl_to_srgb(hsl);
        return Gfx::linear_srgb_to_xyz65(Gfx::srgb_to_linear_srgb(srgb_fraction));
    }
    case ColorType::HWB: {
        Gfx::ColorComponents const hwb { native[0], native[1] / 100.0f, native[2] / 100.0f, native.alpha() };
        auto srgb_fraction = Gfx::hwb_to_srgb(hwb);
        return Gfx::linear_srgb_to_xyz65(Gfx::srgb_to_linear_srgb(srgb_fraction));
    }
    case ColorType::sRGB:
        return Gfx::linear_srgb_to_xyz65(Gfx::srgb_to_linear_srgb(native));
    case ColorType::sRGBLinear:
        return Gfx::linear_srgb_to_xyz65(native);
    case ColorType::A98RGB:
        return Gfx::linear_a98_rgb_to_xyz65(Gfx::a98_rgb_to_linear_a98_rgb(native));
    case ColorType::DisplayP3:
        return Gfx::linear_display_p3_to_xyz65(Gfx::display_p3_to_linear_display_p3(native));
    case ColorType::DisplayP3Linear:
        return Gfx::linear_display_p3_to_xyz65(native);
    case ColorType::ProPhotoRGB:
        return Gfx::xyz50_to_xyz65(Gfx::linear_prophoto_rgb_to_xyz50(Gfx::prophoto_rgb_to_linear_prophoto_rgb(native)));
    case ColorType::Rec2020:
        return Gfx::linear_rec2020_to_xyz65(Gfx::rec2020_to_linear_rec2020(native));
    case ColorType::Lab:
        return Gfx::xyz50_to_xyz65(Gfx::lab_to_xyz50(native));
    case ColorType::LCH:
        return Gfx::xyz50_to_xyz65(Gfx::lab_to_xyz50(Gfx::lch_to_lab(native)));
    case ColorType::OKLab:
        return Gfx::oklab_to_xyz65(native);
    case ColorType::OKLCH:
        return Gfx::oklab_to_xyz65(Gfx::oklch_to_oklab(native));
    case ColorType::XYZD50:
        return Gfx::xyz50_to_xyz65(native);
    case ColorType::XYZD65:
        return native;
    }
    VERIFY_NOT_REACHED();
}

void set_channels_for_target(RelativeColorContext& context, ColorStyleValue::ColorType target_color_type, Gfx::ColorComponents const& xyz65, Gfx::ColorComponents const& srgb_fraction, Gfx::ColorComponents const& linear_srgb)
{
    using ColorType = ColorStyleValue::ColorType;
    switch (target_color_type) {
    case ColorType::RGB:
        context.set(ChannelKeyword::R, srgb_fraction[0] * 255.0);
        context.set(ChannelKeyword::G, srgb_fraction[1] * 255.0);
        context.set(ChannelKeyword::B, srgb_fraction[2] * 255.0);
        break;
    case ColorType::HSL: {
        auto hsl = Gfx::srgb_to_hsl(srgb_fraction);
        context.set(ChannelKeyword::H, hsl[0]);
        context.set(ChannelKeyword::S, hsl[1] * 100.0);
        context.set(ChannelKeyword::L, hsl[2] * 100.0);
        break;
    }
    case ColorType::HWB: {
        auto hwb = Gfx::srgb_to_hwb(srgb_fraction);
        context.set(ChannelKeyword::H, hwb[0]);
        context.set(ChannelKeyword::W, hwb[1] * 100.0);
        context.set(ChannelKeyword::B, hwb[2] * 100.0);
        break;
    }
    case ColorType::Lab: {
        auto lab = Gfx::xyz50_to_lab(Gfx::xyz65_to_xyz50(xyz65));
        context.set(ChannelKeyword::L, lab[0]);
        context.set(ChannelKeyword::A, lab[1]);
        context.set(ChannelKeyword::B, lab[2]);
        break;
    }
    case ColorType::LCH: {
        auto lch = Gfx::lab_to_lch(Gfx::xyz50_to_lab(Gfx::xyz65_to_xyz50(xyz65)));
        context.set(ChannelKeyword::L, lch[0]);
        context.set(ChannelKeyword::C, lch[1]);
        context.set(ChannelKeyword::H, lch[2]);
        break;
    }
    case ColorType::OKLab: {
        auto oklab = Gfx::xyz65_to_oklab(xyz65);
        context.set(ChannelKeyword::L, oklab[0]);
        context.set(ChannelKeyword::A, oklab[1]);
        context.set(ChannelKeyword::B, oklab[2]);
        break;
    }
    case ColorType::OKLCH: {
        auto oklch = Gfx::oklab_to_oklch(Gfx::xyz65_to_oklab(xyz65));
        context.set(ChannelKeyword::L, oklch[0]);
        context.set(ChannelKeyword::C, oklch[1]);
        context.set(ChannelKeyword::H, oklch[2]);
        break;
    }
    case ColorType::sRGB:
        context.set(ChannelKeyword::R, srgb_fraction[0]);
        context.set(ChannelKeyword::G, srgb_fraction[1]);
        context.set(ChannelKeyword::B, srgb_fraction[2]);
        break;
    case ColorType::sRGBLinear:
        context.set(ChannelKeyword::R, linear_srgb[0]);
        context.set(ChannelKeyword::G, linear_srgb[1]);
        context.set(ChannelKeyword::B, linear_srgb[2]);
        break;
    case ColorType::A98RGB: {
        auto a98 = Gfx::linear_a98_rgb_to_a98_rgb(Gfx::xyz65_to_linear_a98_rgb(xyz65));
        context.set(ChannelKeyword::R, a98[0]);
        context.set(ChannelKeyword::G, a98[1]);
        context.set(ChannelKeyword::B, a98[2]);
        break;
    }
    case ColorType::DisplayP3: {
        auto p3 = Gfx::linear_display_p3_to_display_p3(Gfx::xyz65_to_linear_display_p3(xyz65));
        context.set(ChannelKeyword::R, p3[0]);
        context.set(ChannelKeyword::G, p3[1]);
        context.set(ChannelKeyword::B, p3[2]);
        break;
    }
    case ColorType::DisplayP3Linear: {
        auto linear_p3 = Gfx::xyz65_to_linear_display_p3(xyz65);
        context.set(ChannelKeyword::R, linear_p3[0]);
        context.set(ChannelKeyword::G, linear_p3[1]);
        context.set(ChannelKeyword::B, linear_p3[2]);
        break;
    }
    case ColorType::ProPhotoRGB: {
        auto prophoto = Gfx::linear_prophoto_rgb_to_prophoto_rgb(Gfx::xyz50_to_linear_prophoto_rgb(Gfx::xyz65_to_xyz50(xyz65)));
        context.set(ChannelKeyword::R, prophoto[0]);
        context.set(ChannelKeyword::G, prophoto[1]);
        context.set(ChannelKeyword::B, prophoto[2]);
        break;
    }
    case ColorType::Rec2020: {
        auto rec2020 = Gfx::linear_rec2020_to_rec2020(Gfx::xyz65_to_linear_rec2020(xyz65));
        context.set(ChannelKeyword::R, rec2020[0]);
        context.set(ChannelKeyword::G, rec2020[1]);
        context.set(ChannelKeyword::B, rec2020[2]);
        break;
    }
    case ColorType::XYZD50: {
        auto xyz50 = Gfx::xyz65_to_xyz50(xyz65);
        context.set(ChannelKeyword::X, xyz50[0]);
        context.set(ChannelKeyword::Y, xyz50[1]);
        context.set(ChannelKeyword::Z, xyz50[2]);
        break;
    }
    case ColorType::XYZD65:
        context.set(ChannelKeyword::X, xyz65[0]);
        context.set(ChannelKeyword::Y, xyz65[1]);
        context.set(ChannelKeyword::Z, xyz65[2]);
        break;
    }
}

Optional<Gfx::ColorComponents> resolve_origin_native_channels(StyleValue const& origin, ColorStyleValue::ColorType& out_native_type)
{
    if (!origin.is_color_function())
        return {};
    auto const& color_function = as<ColorFunctionStyleValue>(origin);
    if (color_function.origin_color())
        return {};
    if (!color_function.color_type().has_value())
        return {};

    auto const& descriptor = color_function.descriptor();
    auto channel_value = [&](size_t index) -> Optional<double> {
        auto const& value = color_function.channel(index);
        if (value.to_keyword() == Keyword::None)
            return 0.0;
        auto const& channel_descriptor = descriptor.channels[index];
        if (channel_descriptor.kind == ChannelKind::Hue)
            return ColorStyleValue::resolve_hue(value, {});
        return ColorStyleValue::resolve_with_reference_value(value, channel_descriptor.percent_reference, {});
    };
    auto alpha_value = [&]() -> Optional<double> {
        auto alpha = color_function.alpha();
        // An omitted alpha on a non-relative color function defaults to 1.
        if (!alpha)
            return 1.0;
        if (alpha->to_keyword() == Keyword::None)
            return 0.0;
        return ColorStyleValue::resolve_alpha(*alpha, {});
    };

    auto c1 = channel_value(0);
    auto c2 = channel_value(1);
    auto c3 = channel_value(2);
    auto alpha = alpha_value();
    if (!c1.has_value() || !c2.has_value() || !c3.has_value() || !alpha.has_value())
        return {};

    out_native_type = *color_function.color_type();
    return Gfx::ColorComponents { static_cast<float>(*c1), static_cast<float>(*c2), static_cast<float>(*c3), static_cast<float>(*alpha) };
}

}

Optional<RelativeColorContext> ColorStyleValue::extract_channels_in_color_space(StyleValue const& origin_color, ColorType target_color_type, ColorResolutionContext const& color_resolution_context)
{
    // https://drafts.csswg.org/css-color-5/#resolving-rcs
    StyleValue const* effective_origin = &origin_color;
    if (effective_origin->to_keyword() == Keyword::Currentcolor
        && color_resolution_context.current_color_style_value
        && !color_resolution_context.current_color_style_value->depends_on_current_color()) {
        effective_origin = color_resolution_context.current_color_style_value;
    }

    RefPtr<StyleValue const> resolved_origin_storage;
    if (effective_origin->is_color_function()) {
        auto const& origin_color_function = as<ColorFunctionStyleValue>(*effective_origin);
        if (origin_color_function.origin_color()) {
            resolved_origin_storage = origin_color_function.resolve_relative_form(color_resolution_context);
            if (!resolved_origin_storage)
                return {};
            effective_origin = resolved_origin_storage.ptr();
        }
    }

    // High-precision path: for ColorFunctionStyleValue origins, resolve channels directly without going through
    // Gfx::Color's u8 sRGB representation, which clips wide-gamut colors and loses precision.
    ColorType origin_native_type;
    if (auto native_channels = resolve_origin_native_channels(*effective_origin, origin_native_type); native_channels.has_value()) {
        RelativeColorContext context;
        context.set(ChannelKeyword::Alpha, native_channels->alpha());

        // If the target matches the origin's native type, use the origin's channel values directly to avoid round-trip
        // losses.
        if (origin_native_type == target_color_type) {
            switch (target_color_type) {
            case ColorType::RGB:
                context.set(ChannelKeyword::R, (*native_channels)[0]);
                context.set(ChannelKeyword::G, (*native_channels)[1]);
                context.set(ChannelKeyword::B, (*native_channels)[2]);
                return context;
            case ColorType::HSL:
                context.set(ChannelKeyword::H, (*native_channels)[0]);
                context.set(ChannelKeyword::S, (*native_channels)[1]);
                context.set(ChannelKeyword::L, (*native_channels)[2]);
                return context;
            case ColorType::HWB:
                context.set(ChannelKeyword::H, (*native_channels)[0]);
                context.set(ChannelKeyword::W, (*native_channels)[1]);
                context.set(ChannelKeyword::B, (*native_channels)[2]);
                return context;
            case ColorType::Lab:
            case ColorType::OKLab:
                context.set(ChannelKeyword::L, (*native_channels)[0]);
                context.set(ChannelKeyword::A, (*native_channels)[1]);
                context.set(ChannelKeyword::B, (*native_channels)[2]);
                return context;
            case ColorType::LCH:
            case ColorType::OKLCH:
                context.set(ChannelKeyword::L, (*native_channels)[0]);
                context.set(ChannelKeyword::C, (*native_channels)[1]);
                context.set(ChannelKeyword::H, (*native_channels)[2]);
                return context;
            case ColorType::sRGB:
            case ColorType::sRGBLinear:
            case ColorType::A98RGB:
            case ColorType::DisplayP3:
            case ColorType::DisplayP3Linear:
            case ColorType::ProPhotoRGB:
            case ColorType::Rec2020:
                context.set(ChannelKeyword::R, (*native_channels)[0]);
                context.set(ChannelKeyword::G, (*native_channels)[1]);
                context.set(ChannelKeyword::B, (*native_channels)[2]);
                return context;
            case ColorType::XYZD50:
            case ColorType::XYZD65:
                context.set(ChannelKeyword::X, (*native_channels)[0]);
                context.set(ChannelKeyword::Y, (*native_channels)[1]);
                context.set(ChannelKeyword::Z, (*native_channels)[2]);
                return context;
            }
        }

        // When both origin and target are in the sRGB gamut family, convert directly to avoid matrix round-trip
        // precision loss.
        auto is_srgb_family = [](ColorType type) {
            return type == ColorType::RGB || type == ColorType::HSL || type == ColorType::HWB
                || type == ColorType::sRGB || type == ColorType::sRGBLinear;
        };

        if (is_srgb_family(origin_native_type) && is_srgb_family(target_color_type)) {
            Gfx::ColorComponents srgb_fraction;
            switch (origin_native_type) {
            case ColorType::RGB:
                srgb_fraction = { (*native_channels)[0] / 255.0f, (*native_channels)[1] / 255.0f, (*native_channels)[2] / 255.0f, native_channels->alpha() };
                break;
            case ColorType::HSL: {
                Gfx::ColorComponents const hsl { (*native_channels)[0], (*native_channels)[1] / 100.0f, (*native_channels)[2] / 100.0f, native_channels->alpha() };
                srgb_fraction = Gfx::hsl_to_srgb(hsl);
                break;
            }
            case ColorType::HWB: {
                Gfx::ColorComponents const hwb { (*native_channels)[0], (*native_channels)[1] / 100.0f, (*native_channels)[2] / 100.0f, native_channels->alpha() };
                srgb_fraction = Gfx::hwb_to_srgb(hwb);
                break;
            }
            case ColorType::sRGB:
                srgb_fraction = *native_channels;
                break;
            case ColorType::sRGBLinear:
                srgb_fraction = Gfx::linear_srgb_to_srgb(*native_channels);
                break;
            default:
                VERIFY_NOT_REACHED();
            }
            srgb_fraction.set_alpha(native_channels->alpha());
            auto linear_srgb = Gfx::srgb_to_linear_srgb(srgb_fraction);
            set_channels_for_target(context, target_color_type, Gfx::linear_srgb_to_xyz65(linear_srgb), srgb_fraction, linear_srgb);
            return context;
        }

        auto xyz65 = native_channels_to_xyz65(*native_channels, origin_native_type);
        auto linear_srgb = Gfx::xyz65_to_linear_srgb(xyz65);
        auto srgb_fraction = Gfx::linear_srgb_to_srgb(linear_srgb);
        set_channels_for_target(context, target_color_type, xyz65, srgb_fraction, linear_srgb);

        return context;
    }

    auto maybe_color = effective_origin->to_color(color_resolution_context);
    if (!maybe_color.has_value())
        return {};
    auto color = maybe_color.value();

    RelativeColorContext context;
    auto const srgb_fraction = Gfx::ColorComponents {
        color.red() / 255.0f,
        color.green() / 255.0f,
        color.blue() / 255.0f,
        color.alpha() / 255.0f,
    };

    context.set(ChannelKeyword::Alpha, srgb_fraction.alpha());

    auto linear_srgb = Gfx::srgb_to_linear_srgb(srgb_fraction);
    set_channels_for_target(context, target_color_type, Gfx::linear_srgb_to_xyz65(linear_srgb), srgb_fraction, linear_srgb);

    return context;
}

void ColorStyleValue::serialize_color_component(StringBuilder& builder, SerializationMode mode, StyleValue const& component, float one_hundred_percent_value, Optional<double> clamp_min, Optional<double> clamp_max) const
{
    if (component.to_keyword() == Keyword::None) {
        builder.append("none"sv);
        return;
    }
    if (component.is_calculated() && mode == SerializationMode::Normal) {
        component.serialize(builder, mode);
        return;
    }

    auto maybe_resolved_value = resolve_with_reference_value(component, one_hundred_percent_value, {});

    if (!maybe_resolved_value.has_value()) {
        component.serialize(builder, mode);
        return;
    }

    auto resolved_value = maybe_resolved_value.value();

    if (clamp_min.has_value() && resolved_value < *clamp_min)
        resolved_value = *clamp_min;
    if (clamp_max.has_value() && resolved_value > *clamp_max)
        resolved_value = *clamp_max;

    serialize_a_number(builder, resolved_value);
}

void ColorStyleValue::serialize_alpha_component(StringBuilder& builder, SerializationMode mode, StyleValue const& component) const
{
    if (component.to_keyword() == Keyword::None) {
        builder.append("none"sv);
        return;
    }
    if (component.is_calculated() && mode == SerializationMode::Normal) {
        component.serialize(builder, mode);
        return;
    }

    auto maybe_resolved_value = resolve_alpha(component, {});

    if (!maybe_resolved_value.has_value()) {
        component.serialize(builder, mode);
        return;
    }

    serialize_a_number(builder, maybe_resolved_value.value());
}

void ColorStyleValue::serialize_hue_component(StringBuilder& builder, SerializationMode mode, StyleValue const& component) const
{
    if (component.to_keyword() == Keyword::None) {
        builder.append("none"sv);
        return;
    }
    if (component.is_calculated() && mode == SerializationMode::Normal) {
        component.serialize(builder, mode);
        return;
    }

    auto maybe_resolved_value = resolve_hue(component, {});

    if (!maybe_resolved_value.has_value()) {
        component.serialize(builder, mode);
        return;
    }

    builder.appendff("{:.4}", maybe_resolved_value.value());
}

}
