/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>

namespace Web::CSS {

enum class ChannelKind : u8 {
    Number,
    Hue,
};

struct ChannelDescriptor {
    ChannelKind kind;
    float percent_reference;
    Optional<double> serialize_clamp_min;
    Optional<double> serialize_clamp_max;
};

enum class SerializationBehavior : u8 {
    SrgbLegacy,    // rgb(), hsl(): commas or slash chosen via ColorSyntax; serializes via the sRGB shortcut.
    SrgbModern,    // hwb(): modern named function in the sRGB family; serializes via the sRGB shortcut.
    ModernNamed,   // lab(), oklab(), lch(), oklch(): modern named function, no sRGB shortcut.
    ColorFunction, // color(<space> c1 c2 c3 / a).
};

enum class AbsolutizesToRgb : bool {
    No,
    Yes,
};

struct ColorFunctionDescriptor {
    StringView function_name;
    SerializationBehavior serialization_behavior;
    Array<ChannelDescriptor, 3> channels;
    AbsolutizesToRgb absolutizes_to_rgb { AbsolutizesToRgb::No };
};

ColorFunctionDescriptor const& color_function_descriptor_for(ColorStyleValue::ColorType);

Optional<ColorStyleValue::ColorType> color_type_from_color_function_name(StringView);

}
