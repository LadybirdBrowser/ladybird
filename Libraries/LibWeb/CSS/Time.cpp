/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

Time::Time(double value, TimeUnit unit)
    : m_unit(unit)
    , m_value(value)
{
}

Time Time::make_seconds(double value)
{
    return { value, TimeUnit::S };
}

Time Time::percentage_of(Percentage const& percentage) const
{
    return Time { percentage.as_fraction() * m_value, m_unit };
}

String Time::to_string(SerializationMode serialization_mode) const
{
    // https://drafts.csswg.org/cssom/#serialize-a-css-value
    // -> <time>
    // The time in seconds serialized as per <number> followed by the literal string "s".
    // AD-HOC: WPT expects us to serialize using the actual unit, like for other dimensions.
    //         https://github.com/w3c/csswg-drafts/issues/12616
    if (serialization_mode == SerializationMode::ResolvedValue) {
        StringBuilder builder;
        serialize_a_number(builder, to_seconds());
        builder.append("s"sv);
        return builder.to_string_without_validation();
    }
    StringBuilder builder;
    serialize_a_number(builder, raw_value());
    builder.append(unit_name());
    return builder.to_string_without_validation();
}

double Time::to_seconds() const
{
    return ratio_between_units(m_unit, TimeUnit::S) * m_value;
}

double Time::to_milliseconds() const
{
    return ratio_between_units(m_unit, TimeUnit::Ms) * m_value;
}

Time Time::resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const& calculated, Layout::Node const& layout_node, Time const& reference_value)
{
    CalculationResolutionContext context {
        .percentage_basis = reference_value,
        .length_resolution_context = Length::ResolutionContext::for_layout_node(layout_node),
    };
    return calculated->resolve_time(context).value();
}

}
