/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Resolution.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>

namespace Web::CSS {

Resolution::Resolution(double value, ResolutionUnit unit)
    : m_unit(unit)
    , m_value(value)
{
}

Resolution Resolution::make_dots_per_pixel(double value)
{
    return { value, ResolutionUnit::Dppx };
}

Resolution Resolution::from_style_value(NonnullRefPtr<StyleValue const> const& style_value)
{
    if (style_value->is_resolution())
        return style_value->as_resolution().resolution();

    if (style_value->is_calculated())
        return style_value->as_calculated().resolve_resolution({}).value();

    VERIFY_NOT_REACHED();
}

void Resolution::serialize(StringBuilder& builder, SerializationMode serialization_mode) const
{
    // https://drafts.csswg.org/cssom/#serialize-a-css-value
    // -> <resolution>
    // The resolution in dots per CSS pixel serialized as per <number> followed by the literal string "dppx".
    // AD-HOC: WPT expects us to serialize using the actual unit, like for other dimensions.
    //         https://github.com/w3c/csswg-drafts/issues/12616
    if (serialization_mode == SerializationMode::ResolvedValue) {
        serialize_a_number(builder, to_dots_per_pixel());
        builder.append("dppx"sv);
        return;
    }
    serialize_a_number(builder, raw_value());
    builder.append(unit_name());
}

String Resolution::to_string(SerializationMode serialization_mode) const
{
    StringBuilder builder;
    serialize(builder, serialization_mode);
    return builder.to_string_without_validation();
}

double Resolution::to_dots_per_pixel() const
{
    return ratio_between_units(m_unit, ResolutionUnit::Dppx) * m_value;
}

}
