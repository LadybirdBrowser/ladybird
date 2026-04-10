/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FitContentStyleValue.h"
#include <LibWeb/CSS/PercentageOr.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<FitContentStyleValue const> FitContentStyleValue::create()
{
    return adopt_ref(*new (nothrow) FitContentStyleValue());
}

ValueComparingNonnullRefPtr<FitContentStyleValue const> FitContentStyleValue::create(NonnullRefPtr<StyleValue const> length_percentage)
{
    return adopt_ref(*new (nothrow) FitContentStyleValue(move(length_percentage)));
}

ValueComparingNonnullRefPtr<StyleValue const> FitContentStyleValue::absolutized(ComputationContext const& computation_context) const
{
    if (!m_length_percentage)
        return *this;

    auto absolutized_length_percentage = m_length_percentage->absolutized(computation_context);
    if (absolutized_length_percentage == m_length_percentage)
        return *this;

    return FitContentStyleValue::create(absolutized_length_percentage);
}

void FitContentStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    if (!m_length_percentage) {
        builder.append("fit-content"sv);
        return;
    }
    builder.append("fit-content("sv);
    m_length_percentage->serialize(builder, mode);
    builder.append(')');
}

bool FitContentStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    return m_length_percentage == other.as_fit_content().m_length_percentage;
}

Optional<LengthPercentage> FitContentStyleValue::length_percentage() const
{
    if (!m_length_percentage)
        return {};

    return LengthPercentage::from_style_value(*m_length_percentage);
}

}
