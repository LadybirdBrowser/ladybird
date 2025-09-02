/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Frequency.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>

namespace Web::CSS {

Frequency::Frequency(double value, FrequencyUnit unit)
    : m_unit(unit)
    , m_value(value)
{
}

Frequency Frequency::make_hertz(double value)
{
    return { value, FrequencyUnit::Hz };
}

Frequency Frequency::percentage_of(Percentage const& percentage) const
{
    return Frequency { percentage.as_fraction() * m_value, m_unit };
}

String Frequency::to_string(SerializationMode serialization_mode) const
{
    // https://drafts.csswg.org/cssom/#serialize-a-css-value
    // -> <frequency>
    // The <number> component serialized as per <number> followed by the unit in its canonical form as defined in its
    // respective specification.
    if (serialization_mode == SerializationMode::ResolvedValue) {
        StringBuilder builder;
        serialize_a_number(builder, to_hertz());
        builder.append("hz"sv);
        return builder.to_string_without_validation();
    }
    StringBuilder builder;
    serialize_a_number(builder, raw_value());
    builder.append(unit_name());
    return builder.to_string_without_validation();
}

double Frequency::to_hertz() const
{
    switch (m_unit) {
    case FrequencyUnit::Hz:
        return m_value;
    case FrequencyUnit::KHz:
        return m_value * 1000;
    }
    VERIFY_NOT_REACHED();
}

Frequency Frequency::resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const& calculated, Layout::Node const& layout_node, Frequency const& reference_value)
{
    CalculationResolutionContext context {
        .percentage_basis = reference_value,
        .length_resolution_context = Length::ResolutionContext::for_layout_node(layout_node),
    };
    return calculated->resolve_frequency(context).value();
}

}
