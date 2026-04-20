/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ColorFunctionDescriptor.h>

namespace Web::CSS {

using ColorType = ColorStyleValue::ColorType;

static constexpr size_t color_type_count = to_underlying(ColorType::XYZD65) + 1;

namespace {

Array<ColorFunctionDescriptor, color_type_count> build_color_function_descriptors()
{
    Array<ColorFunctionDescriptor, color_type_count> table {};

    table[to_underlying(ColorType::RGB)] = {
        "rgb"sv,
        SerializationBehavior::SrgbLegacy,
        { ChannelDescriptor { ChannelKind::Number, 255.0f, 0.0, 255.0 },
            ChannelDescriptor { ChannelKind::Number, 255.0f, 0.0, 255.0 },
            ChannelDescriptor { ChannelKind::Number, 255.0f, 0.0, 255.0 } },
        AbsolutizesToRgb::No,
    };
    table[to_underlying(ColorType::A98RGB)] = {
        "a98-rgb"sv,
        SerializationBehavior::ColorFunction,
        { ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} } },
    };
    table[to_underlying(ColorType::DisplayP3)] = {
        "display-p3"sv,
        SerializationBehavior::ColorFunction,
        { ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} } },
    };
    table[to_underlying(ColorType::DisplayP3Linear)] = {
        "display-p3-linear"sv,
        SerializationBehavior::ColorFunction,
        { ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} } },
    };
    table[to_underlying(ColorType::HSL)] = {
        "hsl"sv,
        SerializationBehavior::SrgbLegacy,
        { ChannelDescriptor { ChannelKind::Hue, 0.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 100.0f, 0.0, {} },
            ChannelDescriptor { ChannelKind::Number, 100.0f, 0.0, {} } },
        AbsolutizesToRgb::Yes,
    };
    table[to_underlying(ColorType::HWB)] = {
        "hwb"sv,
        SerializationBehavior::SrgbModern,
        { ChannelDescriptor { ChannelKind::Hue, 0.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 100.0f, 0.0, {} },
            ChannelDescriptor { ChannelKind::Number, 100.0f, 0.0, {} } },
        AbsolutizesToRgb::Yes,
    };
    table[to_underlying(ColorType::Lab)] = {
        "lab"sv,
        SerializationBehavior::ModernNamed,
        { ChannelDescriptor { ChannelKind::Number, 100.0f, 0.0, 100.0 },
            ChannelDescriptor { ChannelKind::Number, 125.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 125.0f, {}, {} } },
    };
    table[to_underlying(ColorType::LCH)] = {
        "lch"sv,
        SerializationBehavior::ModernNamed,
        { ChannelDescriptor { ChannelKind::Number, 100.0f, 0.0, 100.0 },
            ChannelDescriptor { ChannelKind::Number, 150.0f, 0.0, {} },
            ChannelDescriptor { ChannelKind::Hue, 0.0f, {}, {} } },
        AbsolutizesToRgb::No,
    };
    table[to_underlying(ColorType::OKLab)] = {
        "oklab"sv,
        SerializationBehavior::ModernNamed,
        { ChannelDescriptor { ChannelKind::Number, 1.0f, 0.0, 1.0 },
            ChannelDescriptor { ChannelKind::Number, 0.4f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 0.4f, {}, {} } },
    };
    table[to_underlying(ColorType::OKLCH)] = {
        "oklch"sv,
        SerializationBehavior::ModernNamed,
        { ChannelDescriptor { ChannelKind::Number, 1.0f, 0.0, 1.0 },
            ChannelDescriptor { ChannelKind::Number, 0.4f, 0.0, {} },
            ChannelDescriptor { ChannelKind::Hue, 0.0f, {}, {} } },
        AbsolutizesToRgb::No,
    };
    table[to_underlying(ColorType::sRGB)] = {
        "srgb"sv,
        SerializationBehavior::ColorFunction,
        { ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} } },
    };
    table[to_underlying(ColorType::sRGBLinear)] = {
        "srgb-linear"sv,
        SerializationBehavior::ColorFunction,
        { ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} } },
    };
    table[to_underlying(ColorType::ProPhotoRGB)] = {
        "prophoto-rgb"sv,
        SerializationBehavior::ColorFunction,
        { ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} } },
    };
    table[to_underlying(ColorType::Rec2020)] = {
        "rec2020"sv,
        SerializationBehavior::ColorFunction,
        { ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} } },
    };
    table[to_underlying(ColorType::XYZD50)] = {
        "xyz-d50"sv,
        SerializationBehavior::ColorFunction,
        { ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} } },
    };
    table[to_underlying(ColorType::XYZD65)] = {
        "xyz-d65"sv,
        SerializationBehavior::ColorFunction,
        { ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} },
            ChannelDescriptor { ChannelKind::Number, 1.0f, {}, {} } },
    };

    return table;
}

}

static auto const s_color_function_descriptors = build_color_function_descriptors();

ColorFunctionDescriptor const& color_function_descriptor_for(ColorType color_type)
{
    return s_color_function_descriptors[to_underlying(color_type)];
}

Optional<ColorType> color_type_from_color_function_name(StringView name)
{
    // "xyz" is an alias for "xyz-d65".
    if (name == "xyz"sv)
        return ColorType::XYZD65;

    for (size_t i = 0; i < s_color_function_descriptors.size(); ++i) {
        auto const& descriptor = s_color_function_descriptors[i];
        if (descriptor.serialization_behavior == SerializationBehavior::ColorFunction && descriptor.function_name == name)
            return static_cast<ColorType>(i);
    }
    return {};
}

}
