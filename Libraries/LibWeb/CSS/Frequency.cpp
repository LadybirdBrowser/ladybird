/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Frequency.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>

namespace Web::CSS {

Frequency::Frequency(double value, Type type)
    : m_type(type)
    , m_value(value)
{
}

Frequency Frequency::make_hertz(double value)
{
    return { value, Type::Hz };
}

Frequency Frequency::percentage_of(Percentage const& percentage) const
{
    return Frequency { percentage.as_fraction() * m_value, m_type };
}

String Frequency::to_string(SerializationMode serialization_mode) const
{
    if (serialization_mode == SerializationMode::ResolvedValue)
        return MUST(String::formatted("{}hz", to_hertz()));
    return MUST(String::formatted("{}{}", raw_value(), unit_name()));
}

double Frequency::to_hertz() const
{
    switch (m_type) {
    case Type::Hz:
        return m_value;
    case Type::kHz:
        return m_value * 1000;
    }
    VERIFY_NOT_REACHED();
}

StringView Frequency::unit_name() const
{
    switch (m_type) {
    case Type::Hz:
        return "hz"sv;
    case Type::kHz:
        return "khz"sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<Frequency::Type> Frequency::unit_from_name(StringView name)
{
    if (name.equals_ignoring_ascii_case("hz"sv)) {
        return Type::Hz;
    } else if (name.equals_ignoring_ascii_case("khz"sv)) {
        return Type::kHz;
    }
    return {};
}

Frequency Frequency::resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const& calculated, Layout::Node const& layout_node, Frequency const& reference_value)
{
    return calculated->resolve_frequency(
                         {
                             .percentage_basis = reference_value,
                             .length_resolution_context = Length::ResolutionContext::for_layout_node(layout_node),
                         })
        .value();
}

}
