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

StyleValueFFI::ColorBase const& ColorStyleValue::color_base() const
{
    switch (m_value->tag) {
    case StyleValueFFI::StyleValueData::Tag::ColorFunction:
        return m_value->color_function._0->color_base;
    case StyleValueFFI::StyleValueData::Tag::ColorMix:
        return m_value->color_mix._0->color_base;
    case StyleValueFFI::StyleValueData::Tag::LightDark:
        return m_value->light_dark._0->color_base;
    case StyleValueFFI::StyleValueData::Tag::ContrastColor:
        return m_value->contrast_color.color_base;
    default:
        VERIFY_NOT_REACHED();
    }
}

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
