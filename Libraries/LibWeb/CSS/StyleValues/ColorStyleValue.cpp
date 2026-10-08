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
#include <LibWeb/CSS/StyleComputeFFI.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

// Marshals the plain-data parts of a ColorResolutionContext for the Rust resolver;
// length_storage keeps the marshalled length context alive across the call.
StyleValueFFI::FfiColorResolutionInput make_rust_color_resolution_input(ColorResolutionContext const& context, Optional<ComputedValuesFFI::FfiLengthResolutionContext>& length_storage)
{
    StyleValueFFI::FfiColorResolutionInput input {};
    if (context.color_scheme.has_value()) {
        input.has_scheme = true;
        input.scheme = to_underlying(*context.color_scheme);
    }
    if (context.current_color.has_value()) {
        input.has_current_color = true;
        input.current_color_rgba[0] = context.current_color->red();
        input.current_color_rgba[1] = context.current_color->green();
        input.current_color_rgba[2] = context.current_color->blue();
        input.current_color_rgba[3] = context.current_color->alpha();
    }
    if (context.current_color_style_value_data)
        input.current_color_value = context.current_color_style_value_data;
    else if (context.current_color_style_value)
        input.current_color_value = context.current_color_style_value->rust_style_value_data();
    if (context.calculation_resolution_context.length_resolution_context.has_value()) {
        length_storage = to_ffi_length_resolution_context(*context.calculation_resolution_context.length_resolution_context);
        input.length = &*length_storage;
    }
    return input;
}

// The base class color_type()/color_syntax() accessors read the ColorBase prefix through the
// color_function arm without knowing which color variant they have; every color variant payload
// must keep the prefix immediately after the Rust discriminant.
static_assert(offsetof(StyleValueFFI::StyleValueData::ColorFunction_Body, color_base) == sizeof(StyleValueFFI::StyleValueData::Tag));
static_assert(offsetof(StyleValueFFI::StyleValueData::ColorMix_Body, color_base) == sizeof(StyleValueFFI::StyleValueData::Tag));
static_assert(offsetof(StyleValueFFI::StyleValueData::LightDark_Body, color_base) == sizeof(StyleValueFFI::StyleValueData::Tag));
static_assert(offsetof(StyleValueFFI::StyleValueData::ContrastColor_Body, color_base) == sizeof(StyleValueFFI::StyleValueData::Tag));

ValueComparingNonnullRefPtr<ColorStyleValue const> ColorStyleValue::create_from_color(Color color, ColorSyntax color_syntax, Optional<Utf16FlyString> name)
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

}
