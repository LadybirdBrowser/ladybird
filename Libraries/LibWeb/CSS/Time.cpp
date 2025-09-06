/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

Time::Time(double value, Type type)
    : m_type(type)
    , m_value(value)
{
}

Time Time::make_seconds(double value)
{
    return { value, Type::S };
}

Time Time::percentage_of(Percentage const& percentage) const
{
    return Time { percentage.as_fraction() * m_value, m_type };
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
    switch (m_type) {
    case Type::S:
        return m_value;
    case Type::Ms:
        return m_value / 1000.0;
    }
    VERIFY_NOT_REACHED();
}

double Time::to_milliseconds() const
{
    switch (m_type) {
    case Type::S:
        return m_value * 1000.0;
    case Type::Ms:
        return m_value;
    }
    VERIFY_NOT_REACHED();
}

StringView Time::unit_name() const
{
    switch (m_type) {
    case Type::S:
        return "s"sv;
    case Type::Ms:
        return "ms"sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<Time::Type> Time::unit_from_name(StringView name)
{
    if (name.equals_ignoring_ascii_case("s"sv)) {
        return Type::S;
    } else if (name.equals_ignoring_ascii_case("ms"sv)) {
        return Type::Ms;
    }
    return {};
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
