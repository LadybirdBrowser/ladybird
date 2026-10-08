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
