/*
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Flex.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

Flex::Flex(double value, Type type)
    : m_type(type)
    , m_value(value)
{
}

Flex Flex::make_fr(double value)
{
    return { value, Type::Fr };
}

Flex Flex::percentage_of(Percentage const& percentage) const
{
    return Flex { percentage.as_fraction() * m_value, m_type };
}

String Flex::to_string(SerializationMode serialization_mode) const
{
    // https://drafts.csswg.org/cssom/#serialize-a-css-value
    // AD-HOC: No spec definition, so copy the other <dimension> definitions
    if (serialization_mode == SerializationMode::ResolvedValue) {
        StringBuilder builder;
        serialize_a_number(builder, to_fr());
        builder.append("fr"sv);
        return builder.to_string_without_validation();
    }
    StringBuilder builder;
    serialize_a_number(builder, raw_value());
    builder.append(unit_name());
    return builder.to_string_without_validation();
}

double Flex::to_fr() const
{
    switch (m_type) {
    case Type::Fr:
        return m_value;
    }
    VERIFY_NOT_REACHED();
}

StringView Flex::unit_name() const
{
    switch (m_type) {
    case Type::Fr:
        return "fr"sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<Flex::Type> Flex::unit_from_name(StringView name)
{
    if (name.equals_ignoring_ascii_case("fr"sv))
        return Type::Fr;

    return {};
}

}
