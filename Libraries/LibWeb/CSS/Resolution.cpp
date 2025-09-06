/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Resolution.h>
#include <LibWeb/CSS/Serialize.h>

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

String Resolution::to_string(SerializationMode serialization_mode) const
{
    // https://drafts.csswg.org/cssom/#serialize-a-css-value
    // -> <resolution>
    // The resolution in dots per CSS pixel serialized as per <number> followed by the literal string "dppx".
    // AD-HOC: WPT expects us to serialize using the actual unit, like for other dimensions.
    //         https://github.com/w3c/csswg-drafts/issues/12616
    if (serialization_mode == SerializationMode::ResolvedValue) {
        StringBuilder builder;
        serialize_a_number(builder, to_dots_per_pixel());
        builder.append("dppx"sv);
        return builder.to_string_without_validation();
    }
    StringBuilder builder;
    serialize_a_number(builder, raw_value());
    builder.append(unit_name());
    return builder.to_string_without_validation();
}

double Resolution::to_dots_per_pixel() const
{
    return ratio_between_units(m_unit, ResolutionUnit::Dppx) * m_value;
}

}
